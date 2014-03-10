/*
 * llc.c
 *
 *  Created on: 05.01.2014
 *      Author: myaut
 */

#define LOG_SOURCE "llc"
#include <log.h>

#include <mempool.h>
#include <defs.h>
#include <workload.h>
#include <wltype.h>
#include <modules.h>
#include <modapi.h>
#include <cpuinfo.h>

#include <llc.h>

#include <sys/mman.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <asm/unistd.h>

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("llc");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT wlp_descr_t llc_params[] = {
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_accesses",
		"Number of memory accesses in request",
		offsetof(struct llc_workload, num_accesses) },
	{ WLP_CPU_OBJECT, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"cpu_object",
		"Root CPU object where cache is located",
		offsetof(struct llc_workload, cpu_object) },
	{ WLP_FLOAT, WLPF_OPTIONAL,
		WLP_NO_RANGE(),
		WLP_FLOAT_DEFAULT(2.0),
		"mem_size",
		"Size of memory area in last level caches",
		offsetof(struct llc_workload, mem_size) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"offset",
		"Randomly generated offset in memory area",
		offsetof(struct llc_request, offset) },
	{ WLP_INTEGER, WLPF_OUTPUT,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"cache_misses",
		"Number of cache-misses during request",
		offsetof(struct llc_request, cache_misses) },
	{ WLP_NULL }
};

module_t* self = NULL;

static size_t llc_page_size = 4096;

static hi_cpu_object_t* get_last_level_cache(hi_cpu_object_t* parent, int* pcount, int* plevel);

static inline int
sys_perf_event_open(struct perf_event_attr *attr,
		      pid_t pid, int cpu, int group_fd,
		      unsigned long flags);
static uint64_t rdpmc(unsigned int counter);
static uint64_t read_counter(struct perf_event_mmap_page * pc);

MODEXPORT int llc_wl_config(workload_t* wl) {
	struct llc_workload* llcw =
				(struct llc_workload*) wl->wl_params;
	struct llc_data* data;

	struct perf_event_attr attr = {
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_LL |
				(PERF_COUNT_HW_CACHE_OP_READ		<<  8) |
				(PERF_COUNT_HW_CACHE_RESULT_MISS	<< 16),
		.exclude_kernel = 1,
	};

	size_t ll_cache_size;

	int count = 0;
	int level = 0;

	hi_cpu_object_t* cache = get_last_level_cache(llcw->cpu_object, &count, &level);

	if(cache == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to find last level cache for '%s'",
				  HI_CPU_TO_OBJ(llcw->cpu_object)->name);
		return 1;
	}

	ll_cache_size = cache->cache.c_size * count;

	data = mp_malloc(sizeof(struct llc_data));
	data->size = llcw->mem_size * ll_cache_size;
	data->line_size = cache->cache.c_line_size;
	data->ptr = NULL;

	data->perf_fd =  sys_perf_event_open(&attr, wl->wl_tp->tp_workers[0].w_thread.t_system_id,
			-1, -1, 0);
	if(data->perf_fd < 0) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to open perf fd memory!");
		return 1;
	}

	data->perf_mem = mmap(NULL, llc_page_size, PROT_READ, MAP_SHARED, data->perf_fd, 0);
	if(data->perf_mem == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap perf fd memory!");
		return 1;
	}

	data->ptr = (char*) mmap(NULL, data->size, PROT_READ | PROT_WRITE,
							MAP_PRIVATE | MAP_ANON, -1, 0);
	madvise(data->ptr, data->size, MADV_HUGEPAGE);

	if(data->ptr == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap memory!");
		return 1;
	}

	wl->wl_private = (void*) data;

	return 0;
}

MODEXPORT int llc_wl_unconfig(workload_t* wl) {
	struct llc_data* data =
		(struct llc_data*) wl->wl_private;

	munmap(data->ptr, data->size);

	munmap(data->perf_mem, llc_page_size);
	close(data->perf_fd);

	mp_free(data);

	return 0;
}

#define ASM_END				"\n"
#define ASM_UNROLL8(expr)			\
	expr 	expr 	expr	expr	\
	expr 	expr 	expr	expr

MODEXPORT int llc_run_request(request_t* rq) {
	struct llc_workload* llcw =
			(struct llc_workload*) rq->rq_workload->wl_params;
	struct llc_request* llcrq =
			(struct llc_request*) rq->rq_params;
	struct llc_data* data =
		(struct llc_data*) rq->rq_workload->wl_private;

	struct perf_event_mmap_page *pc = data->perf_mem;

	register uint32_t* ptr = (uint32_t*) data->ptr;
	int i;
	register int j;
	register uint32_t c;

	size_t size = data->size / sizeof(uint32_t);
	size_t line_size = data->line_size / sizeof(uint32_t);

	ptrdiff_t offset = (llcrq->offset % data->size);

	llcrq->offset = offset;
	offset /= sizeof(uint32_t);

	llcrq->cache_misses = read_counter(pc);

	for(i = 0; i < llcw->num_accesses; ) {
		for(	j = offset;
				(j < (size - 8 * line_size)) && (i < llcw->num_accesses);
				j += line_size) {
			asm volatile(
				ASM_UNROLL8(
					" add 0(%1), %0"	ASM_END
					" add %2, %1"		ASM_END)
					: "=r" (c)
					: "r" (ptr + j),
					  "r" (line_size));

			ptr[j] = c;

			i += 8;
		}

		offset = 0;
	}

	llcrq->cache_misses = read_counter(pc) - llcrq->cache_misses;

	return 0;
}

wl_type_t llc_wlt = {
	"llc",							/* wlt_name */

	WLC_CPU_MISC,					/* wlt_class */

	llc_params,						/* wlt_params */
	sizeof(struct llc_workload),	/* wlt_params_size*/
	sizeof(struct llc_request),		/* wlt_rqparams_size */

	llc_wl_config,					/* wlt_wl_config */
	llc_wl_unconfig,				/* wlt_wl_unconfig */

	NULL,							/* wlt_wl_step */

	llc_run_request					/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &llc_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &llc_wlt);

	return MOD_OK;
}

/* Return last-level cache object */
static hi_cpu_object_t* get_last_level_cache(hi_cpu_object_t* parent, int* pcount, int* plevel) {
	hi_object_t* parent_obj = HI_CPU_TO_OBJ(parent);
	hi_object_child_t* child;
	hi_cpu_object_t* object;
	hi_cpu_object_t* cache = NULL;

	hi_for_each_child(child, parent_obj) {
		object = HI_CPU_FROM_OBJ(child->object);

		if(object->type != HI_CPU_CACHE) {
			cache = get_last_level_cache(object, pcount, plevel);

			if(cache == NULL)
				continue;
		}

		if(*plevel == object->cache.c_level)
			++*pcount;
		else if(*plevel < object->cache.c_level) {
			*pcount = 1;
			*plevel = object->cache.c_level;

			cache = object;
		}
	}

	return cache;
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
