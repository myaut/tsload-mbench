/*
 * nodemem.c
 *
 *  Created on: 05.01.2014
 *      Author: myaut
 */

#define LOG_SOURCE "nodemem"
#include <log.h>

#include <mempool.h>
#include <defs.h>
#include <workload.h>
#include <wltype.h>
#include <modules.h>
#include <modapi.h>
#include <cpuinfo.h>

#include <nodemem.h>

#include <sys/mman.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <float.h>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <asm/unistd.h>
#include <numaif.h>

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("nodemem");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT wlp_descr_t nodemem_params[] = {
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_accesses",
		"Number of memory accesses per request",
		offsetof(struct nodemem_workload, num_accesses) },
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_pools",
		"Number of memory pools",
		offsetof(struct nodemem_workload, num_pools) },
	{ WLP_CPU_OBJECT, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"node_object",
		"Node object where worker is bound",
		offsetof(struct nodemem_workload, node_object) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"offset",
		"Randomly generated offset in memory area",
		offsetof(struct nodemem_request, offset) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"pool_id",
		"ID of pool for this request",
		offsetof(struct nodemem_request, pool_id) },
	{ WLP_FLOAT, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"local_to_remote",
		"Local/remote mem accesses ratio",
		offsetof(struct nodemem_request, local_to_remote) },
	{ WLP_INTEGER, WLPF_OUTPUT,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"node_misses",
		"Number of local node memory misses during request",
		offsetof(struct nodemem_request, node_misses) },
	{ WLP_NULL }
};

module_t* self = NULL;

static size_t nodemem_line_size = 64;		/* FIXME: Should be taken from llc */
static size_t nodemem_tlb_size = 512;
static size_t nodemem_page_size = 4096;

static hi_cpu_object_t* find_remote_node(hi_cpu_object_t* local_node);

static inline int
sys_perf_event_open(struct perf_event_attr *attr,
		      pid_t pid, int cpu, int group_fd,
		      unsigned long flags);
static uint64_t rdpmc(unsigned int counter);
static uint64_t read_counter(struct perf_event_mmap_page * pc);

MODEXPORT int nodemem_wl_config(workload_t* wl) {
	struct nodemem_workload* nodememw =
				(struct nodemem_workload*) wl->wl_params;
	struct nodemem_data* data;

	int ret;
	unsigned long nodemask;
	ptrdiff_t j;

	struct perf_event_attr attr = {
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_NODE |
				(PERF_COUNT_HW_CACHE_OP_READ		<<  8) |
				(PERF_COUNT_HW_CACHE_RESULT_MISS	<< 16),
		.exclude_kernel = 1
	};

	hi_cpu_object_t* remote_node = NULL;

	if(nodememw->node_object->type != HI_CPU_NODE) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Invalid cpu object '%s'",
						  HI_CPU_TO_OBJ(nodememw->node_object)->name);
		return 1;
	}

	remote_node = find_remote_node(nodememw->node_object);

	if(remote_node == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Couldn't to find remote node for '%s'",
				  HI_CPU_TO_OBJ(nodememw->node_object)->name);
		return 1;
	}

	data = mp_malloc(sizeof(struct nodemem_data));
	data->pool_size = nodememw->num_accesses * nodemem_page_size;
	data->size = nodememw->num_pools * data->pool_size;
	data->local_ptr = NULL;
	data->remote_ptr = NULL;

	data->perf_fd =  sys_perf_event_open(&attr, wl->wl_tp->tp_workers[0].w_thread.t_system_id,
										 -1, -1, 0);
	if(data->perf_fd < 0) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to open perf fd memory!");
		return 1;
	}

	data->perf_mem = mmap(NULL, nodemem_page_size, PROT_READ, MAP_SHARED, data->perf_fd, 0);
	if(data->perf_mem == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap perf fd memory!");
		return 1;
	}

	data->local_ptr = (char*) mmap(NULL, data->size, PROT_READ | PROT_WRITE,
									MAP_PRIVATE | MAP_ANON , -1, 0);
	data->remote_ptr = (char*) mmap(NULL, data->size, PROT_READ | PROT_WRITE,
									MAP_PRIVATE | MAP_ANON , -1, 0);

	if(data->local_ptr == NULL || data->remote_ptr == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap memory!");
		return 1;
	}

	madvise(data->local_ptr, data->size, MADV_HUGEPAGE);
	madvise(data->remote_ptr, data->size, MADV_HUGEPAGE);

	nodemask = 1 << nodememw->node_object->id;
	ret = mbind(data->local_ptr, data->size, MPOL_BIND, &nodemask,
					 32, MPOL_MF_MOVE);
	if(ret != 0) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mbind local memory: %d!", errno);
		return 1;
	}

	nodemask = 1 << remote_node->id;
	ret = mbind(data->remote_ptr, data->size, MPOL_BIND, &nodemask,
					 32, MPOL_MF_MOVE);
	if(ret != 0) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mbind remote memory: %d!", errno);
		return 1;
	}

	for(j = 0; j < data->size; j += nodemem_page_size) {
		*(((char*) data->local_ptr) + j) = 's';
		*(((char*) data->remote_ptr) + j) = 'q';
	}

	wl->wl_private = (void*) data;

	return 0;
}

MODEXPORT int nodemem_wl_unconfig(workload_t* wl) {
	struct nodemem_data* data =
		(struct nodemem_data*) wl->wl_private;

	munmap(data->local_ptr, data->size);
	munmap(data->remote_ptr, data->size);

	munmap(data->perf_mem, nodemem_page_size);
	close(data->perf_fd);

	mp_free(data);

	return 0;
}

#define ASM_END				"\n"
#define ASM_UNROLL8(expr)			\
	expr 	expr 	expr	expr	\
	expr 	expr 	expr	expr

MODEXPORT int nodemem_run_request(request_t* rq) {
	struct nodemem_workload* nodememw =
			(struct nodemem_workload*) rq->rq_workload->wl_params;
	struct nodemem_request* nodememrq =
			(struct nodemem_request*) rq->rq_params;
	struct nodemem_data* data =
		(struct nodemem_data*) rq->rq_workload->wl_private;

	struct perf_event_mmap_page *pc = data->perf_mem;

	register uint32_t* ptr;
	int i;
	register ptrdiff_t j;
	register uint32_t c;

	int poolid = 0;
	size_t size = data->pool_size / sizeof(uint32_t);
	size_t line_size = (nodemem_line_size) / sizeof(uint32_t);
	size_t page_size = (nodemem_page_size) / sizeof(uint32_t);

	ptrdiff_t remote_step;
	ptrdiff_t local_step;
	ptrdiff_t step;
	ptrdiff_t offset;

	double a;

	nodememrq->pool_id %= nodememw->num_pools;
	if(nodememrq->pool_id < 0)
		nodememrq->pool_id = -nodememrq->pool_id;

	nodememrq->offset %= nodemem_page_size;
	offset = nodememrq->offset / sizeof(uint32_t);

	if(nodememrq->local_to_remote == 0.0 || nodememrq->local_to_remote == 1.0) {
		local_step = page_size;
		remote_step = page_size;
	}
	else {
		a = nodememrq->local_to_remote;

		local_step  = ((ptrdiff_t) ((1 / (1 - a)) * page_size)) % size;
		remote_step = ((ptrdiff_t) ((1 / a) * page_size)) % size;
	}

	nodememrq->node_misses = read_counter(pc);

	for(i = 0; i < nodememw->num_accesses; ) {
		if(poolid++ & 1) {
			ptr = (uint32_t*) data->remote_ptr;
			step = remote_step;
		}
		else {
			ptr = (uint32_t*) data->local_ptr;
			step = local_step;
		}
		ptr += size * nodememrq->pool_id;

		if(size < (8 * step + page_size))
			continue;

		for(	j = 0 ;
				(j < (size - (8 * step + page_size))) && (i < nodememw->num_accesses);
				j += 8 * step) {
			asm volatile(
				ASM_UNROLL8(
					" add 0(%1), %0"	ASM_END
					" add %2, %1"		ASM_END)
					: "=a" (c)
					: "d" (ptr + j + offset),
					  "c" (step));

			i += 8;
		}

		offset += line_size;
		if(offset >= page_size) {
			offset = 0;
		}
	}

	nodememrq->node_misses = read_counter(pc) - nodememrq->node_misses;

	return 0;
}

wl_type_t nodemem_wlt = {
	"nodemem",							/* wlt_name */

	WLC_CPU_MISC,					/* wlt_class */

	nodemem_params,						/* wlt_params */
	sizeof(struct nodemem_workload),	/* wlt_params_size*/
	sizeof(struct nodemem_request),		/* wlt_rqparams_size */

	nodemem_wl_config,					/* wlt_wl_config */
	nodemem_wl_unconfig,				/* wlt_wl_unconfig */

	NULL,							/* wlt_wl_step */

	nodemem_run_request					/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &nodemem_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &nodemem_wlt);

	return MOD_OK;
}

/* Return last-level cache object */
static hi_cpu_object_t* find_remote_node(hi_cpu_object_t* local_node) {
	list_head_t* cpu_list = hi_cpu_list(B_FALSE);
	hi_object_t*  object;
	hi_cpu_object_t* node;

	hi_for_each_object(object, cpu_list) {
		node = HI_CPU_FROM_OBJ(object);

		if(node->type == HI_CPU_NODE && node != local_node) {
			return node;
		}
	}

	return NULL;
}

static inline int
sys_perf_event_open(struct perf_event_attr *attr,
		      pid_t pid, int cpu, int group_fd,
		      unsigned long flags)
{
	int fd;

	fd = syscall(__NR_perf_event_open, attr, pid, cpu,
		     group_fd, flags);

	return fd;
}


static uint64_t rdpmc(unsigned int counter)
{
	unsigned int low, high;

	asm volatile("rdpmc" : "=a" (low), "=d" (high) : "c" (counter));

	return low | ((uint64_t)high) << 32;
}

#define barrier() asm volatile("" ::: "memory")

static uint64_t read_counter(struct perf_event_mmap_page * pc) {
	uint32_t seq;
	uint32_t idx;
	uint64_t count;

	do {
		seq = pc->lock;
		barrier();

		idx = pc->index;
		count = pc->offset;
		if (idx)
			count += rdpmc(idx - 1);

		barrier();
	} while (pc->lock != seq);

	return count;
}
