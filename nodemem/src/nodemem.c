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
	{ WLP_INTEGER, WLPF_OUTPUT,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"node_misses",
		"Number of local node memory misses during request",
		offsetof(struct nodemem_request, node_misses) },
	{ WLP_NULL }
};

module_t* self = NULL;

size_t nodemem_line_size = 64;		/* FIXME: Should be taken from llc */
size_t nodemem_page_size = 4096;
size_t nodemem_pools_count = 8;		/* FIXME: Maybe a wlparam or dependent on llc size */

static hi_cpu_object_t* find_remote_node(hi_cpu_object_t* local_node);

static inline int
sys_perf_event_open(struct perf_event_attr *attr,
		      pid_t pid, int cpu, int group_fd,
		      unsigned long flags);
static uint64_t rdpmc(unsigned int counter);

MODEXPORT int nodemem_wl_config(workload_t* wl) {
	struct nodemem_workload* nodememw =
				(struct nodemem_workload*) wl->wl_params;
	struct nodemem_data* data;

	int ret;
	unsigned long nodemask;

	struct perf_event_attr attr = {
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_NODE |
				(PERF_COUNT_HW_CACHE_OP_READ		<<  8) |
				(PERF_COUNT_HW_CACHE_RESULT_MISS	<< 16),
		.exclude_kernel = 1,
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
	data->pool_size = nodememw->num_accesses * nodemem_line_size / 2;
	data->size = nodemem_pools_count * data->pool_size;
	data->local_ptr = NULL;
	data->remote_ptr = NULL;

	data->perf_fd =  sys_perf_event_open(&attr, 0, -1, -1, 0);
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
									MAP_PRIVATE | MAP_ANON, -1, 0);
	data->remote_ptr = (char*) mmap(NULL, data->size, PROT_READ | PROT_WRITE,
									MAP_PRIVATE | MAP_ANON, -1, 0);

	if(data->local_ptr == NULL || data->remote_ptr == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap memory!");
		return 1;
	}

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

	wl->wl_private = (void*) data;

	return 0;
}

MODEXPORT int nodemem_wl_unconfig(workload_t* wl) {
	struct nodemem_data* data =
		(struct nodemem_data*) wl->wl_private;

	munmap(data->local_ptr, data->size);
	munmap(data->remote_ptr, data->size);
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

	register uint32_t* ptr = NULL;
	int i;
	register int j;
	register uint32_t c;

	int poolid = (nodememrq->offset / data->pool_size) % (2 * nodemem_pools_count);

	size_t size = data->pool_size / sizeof(uint32_t);
	size_t block_size = (8 * nodemem_line_size) / sizeof(uint32_t);

	ptrdiff_t offset = nodememrq->offset % data->pool_size;
	ptrdiff_t pool_offset;

	nodememrq->offset = offset;
	offset /= sizeof(uint32_t);

	nodememrq->node_misses = rdpmc(pc->index);

	for(i = 0; i < nodememw->num_accesses; ++i) {
		if(++poolid == 2 * nodemem_pools_count)
			poolid = 0;

		pool_offset = (poolid / 2) * size;
		if(poolid & 1) {
			ptr = ((uint32_t*) data->remote_ptr) + pool_offset;
		}
		else {
			ptr = ((uint32_t*) data->local_ptr) + pool_offset;
		}

		for(	j = offset;
				(j < (size - block_size)) && (i < nodememw->num_accesses);
				j += block_size) {
			asm volatile(
				ASM_UNROLL8(
					" add 0(%1), %0"	ASM_END
					" add %2, %1"		ASM_END)
					: "=a" (c)
					: "d" (ptr + j),
					  "c" (nodemem_line_size));

			i += 8;
			ptr[j] = c;
		}

		offset = 0;


	}

	nodememrq->node_misses = rdpmc(pc->index) - nodememrq->node_misses;

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
