/*
 * tlb.c
 *
 *  Created on: 05.01.2014
 *      Author: myaut
 */

#define LOG_SOURCE "tlb"
#include <log.h>

#include <mempool.h>
#include <defs.h>
#include <workload.h>
#include <wltype.h>
#include <modules.h>
#include <modapi.h>
#include <cpuinfo.h>

#include <tlb.h>

#include <sys/mman.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <asm/unistd.h>

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("tlb");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT wlp_descr_t tlb_params[] = {
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_accesses",
		"Number of memory accesses in request",
		offsetof(struct tlb_workload, num_accesses) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"offset",
		"Randomly generated offset in memory area",
		offsetof(struct tlb_request, offset) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"step",
		"Randomly generated step",
		offsetof(struct tlb_request, step) },
	{ WLP_INTEGER, WLPF_OUTPUT,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"tlb_misses",
		"Number of dTLB misses during request",
		offsetof(struct tlb_request, tlb_misses) },
	{ WLP_NULL }
};

module_t* self = NULL;

static size_t tlb_page_size = 4096;
static size_t tlb_line_size = 64;
static size_t tlb_size = 512;
static size_t tlb_pools_count = 4;

static inline int
sys_perf_event_open(struct perf_event_attr *attr,
		      pid_t pid, int cpu, int group_fd,
		      unsigned long flags);
static uint64_t rdpmc(unsigned int counter);
static uint64_t read_counter(struct perf_event_mmap_page * pc);

MODEXPORT int tlb_wl_config(workload_t* wl) {
	struct tlb_workload* tlbw =
				(struct tlb_workload*) wl->wl_params;
	struct tlb_data* data;

	struct perf_event_attr attr = {
		.type = PERF_TYPE_HW_CACHE,
		.config = PERF_COUNT_HW_CACHE_DTLB |
				(PERF_COUNT_HW_CACHE_OP_READ		<<  8) |
				(PERF_COUNT_HW_CACHE_RESULT_MISS	<< 16),
		.exclude_kernel = 1,
	};

	data = mp_malloc(sizeof(struct tlb_data));
	data->size = tlb_pools_count * tlb_size * tlb_page_size;
	data->ptr = NULL;

	data->perf_fd =  sys_perf_event_open(&attr, wl->wl_tp->tp_workers[0].w_thread.t_system_id,
			-1, -1, 0);
	if(data->perf_fd < 0) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to open perf fd memory!");
		return 1;
	}

	data->perf_mem = mmap(NULL, tlb_page_size, PROT_READ, MAP_SHARED, data->perf_fd, 0);
	if(data->perf_mem == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap perf fd memory!");
		return 1;
	}

	data->ptr = (char*) mmap(NULL, data->size, PROT_READ | PROT_WRITE,
							MAP_PRIVATE | MAP_ANON, -1, 0);

	if(data->ptr == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap memory!");
		return 1;
	}

	madvise(data->ptr, data->size, MADV_NOHUGEPAGE);

	wl->wl_private = (void*) data;

	return 0;
}

MODEXPORT int tlb_wl_unconfig(workload_t* wl) {
	struct tlb_data* data =
		(struct tlb_data*) wl->wl_private;

	munmap(data->ptr, data->size);

	munmap(data->perf_mem, tlb_page_size);
	close(data->perf_fd);

	mp_free(data);

	return 0;
}

#define ASM_END				"\n"
#define ASM_UNROLL8(expr)			\
	expr 	expr 	expr	expr	\
	expr 	expr 	expr	expr

MODEXPORT int tlb_run_request(request_t* rq) {
	struct tlb_workload* tlbw =
			(struct tlb_workload*) rq->rq_workload->wl_params;
	struct tlb_request* tlbrq =
			(struct tlb_request*) rq->rq_params;
	struct tlb_data* data =
		(struct tlb_data*) rq->rq_workload->wl_private;

	struct perf_event_mmap_page *pc = data->perf_mem;

	register uint32_t* ptr = (uint32_t*) data->ptr;
	int i;
	register int j;
	register uint32_t c;

	size_t size = data->size / sizeof(uint32_t);
	size_t page_size = tlb_page_size / sizeof(uint32_t);

	ptrdiff_t step = (tlbrq->step * tlb_line_size) % (2 * tlb_page_size);
	ptrdiff_t offset = tlbrq->offset % (data->size / tlb_pools_count);

	tlbrq->step = step;
	tlbrq->offset = offset;

	step /= sizeof(uint32_t);
	offset /= sizeof(uint32_t);
	if(offset >= (size - 8 * step))
		offset = 0;

	tlbrq->tlb_misses = read_counter(pc);

	for(i = 0; i < tlbw->num_accesses; ) {
		for(	j = offset;
				(j < (size - 8 * step)) && (i < tlbw->num_accesses);
				j += 8 * step) {
			asm volatile(
				ASM_UNROLL8(
					" add 0(%1), %0"	ASM_END
					" add %2, %1"		ASM_END)
					: "=r" (c)
					: "r" (ptr + j),
					  "r" (step));

			ptr[j] = c;

			i += 8;
		}
	}

	tlbrq->tlb_misses = read_counter(pc) - tlbrq->tlb_misses;

	return 0;
}

wl_type_t tlb_wlt = {
	"tlb",							/* wlt_name */

	WLC_CPU_MISC,					/* wlt_class */

	tlb_params,						/* wlt_params */
	sizeof(struct tlb_workload),	/* wlt_params_size*/
	sizeof(struct tlb_request),		/* wlt_rqparams_size */

	tlb_wl_config,					/* wlt_wl_config */
	tlb_wl_unconfig,				/* wlt_wl_unconfig */

	NULL,							/* wlt_wl_step */

	tlb_run_request					/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &tlb_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &tlb_wlt);

	return MOD_OK;
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
