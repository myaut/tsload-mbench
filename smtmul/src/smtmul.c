/*
 * smtmul.c
 *
 *  Created on: Mar 3, 2014
 *      Author: myaut
 */

#include <defs.h>
#include <mempool.h>
#include <wlparam.h>
#include <workload.h>
#include <modapi.h>

#include <smtmul.h>

#include <stdlib.h>

#include <sys/mman.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <asm/unistd.h>

module_t* self = NULL;

static inline int
sys_perf_event_open(struct perf_event_attr *attr,
		      pid_t pid, int cpu, int group_fd,
		      unsigned long flags);
static uint64_t rdpmc(unsigned int counter);
static uint64_t read_counter(struct perf_event_mmap_page * pc);

MODEXPORT wlp_descr_t smtmul_params[] = {
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_instructions",
		"Number of instructions per request",
		offsetof(struct smtmul_workload, num_instructions) },
	{ WLP_BOOL, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"enable_perf",
		"Enable self-performance monitoring",
		offsetof(struct smtmul_workload, enable_perf) },
	{ WLP_INTEGER, WLPF_OUTPUT,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"resource_stalls",
		"Number of resource stalls per request",
		offsetof(struct smtmul_request, resource_stalls) },
	{ WLP_NULL }
};

static size_t smtmul_page_size = 4096;

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("smtmul");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT int smtmul_wl_config(workload_t* wl) {
	struct smtmul_workload* smtmul_wl =
			(struct smtmul_workload*) wl->wl_params;

	if(smtmul_wl->enable_perf) {
		struct smtmul_data* data = mp_malloc(sizeof(struct smtmul_data));

		struct perf_event_attr attr = {
			.type = PERF_TYPE_HARDWARE,
			.config = PERF_COUNT_HW_STALLED_CYCLES_FRONTEND,
			.exclude_kernel = 1
		};

		data->perf_fd =  sys_perf_event_open(&attr, wl->wl_tp->tp_workers[0].w_thread.t_system_id,
					-1, -1, 0);
		if(data->perf_fd < 0) {
			wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to open perf fd memory!");
			return 1;
		}

		data->perf_mem = mmap(NULL, smtmul_page_size, PROT_READ, MAP_SHARED, data->perf_fd, 0);
		if(data->perf_mem == NULL) {
			wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to mmap perf fd memory!");
			return 1;
		}

		wl->wl_private = data;
	}

	return 0;
}

MODEXPORT int smtmul_wl_unconfig(workload_t* wl) {
	struct smtmul_workload* smtmul_wl =
				(struct smtmul_workload*) wl->wl_params;

	if(smtmul_wl->enable_perf) {
		struct smtmul_data* data =
				(struct smtmul_data*) wl->wl_private;

		munmap(data->perf_mem, smtmul_page_size);
		close(data->perf_fd);

		mp_free(data);
	}

	return 0;
}

MODEXPORT int smtmul_run_request(request_t* rq) {
	struct smtmul_request* smtmul_rq = (struct smtmul_request*) rq->rq_params;
	struct smtmul_workload* smtmul_wl =
		(struct smtmul_workload*) rq->rq_workload->wl_params;

	struct smtmul_data* data =
		(struct smtmul_data*) rq->rq_workload->wl_private;
	struct perf_event_mmap_page *pc = NULL;

	int i;

	if(smtmul_wl->enable_perf) {
		pc = data->perf_mem;
		smtmul_rq->resource_stalls = read_counter(pc);
	}

	for(i = 0; i < smtmul_wl->num_instructions; i += 12) {
		asm volatile(
			"imul %%eax,%%esi\n"
			"imul %%ebx,%%esi\n"
			"imul %%ecx,%%edi\n"
			"imul %%edx,%%edi\n"
			"imul %%r8d,%%r12d\n"
			"imul %%r9d,%%r12d\n"
			"imul %%r10d,%%r13d\n"
			"imul %%r11d,%%r13d\n"
			"imul %%esi,%%edi\n"
			"imul %%r12d,%%r13d\n"
			"imul %%r13d,%%esi\n"
			"imul %%edi,%%r12d\n"
			:
			:
			: "esi", "edi", "r12", "r13");
	}

	if(smtmul_wl->enable_perf) {
		smtmul_rq->resource_stalls = read_counter(pc) - smtmul_rq->resource_stalls;
	}

	return 0;
}

wl_type_t smtmul_wlt = {
	"smtmul",						/* wlt_name */

	WLC_CPU_INTEGER,				/* wlt_class */

	smtmul_params,					/* wlt_params */
	sizeof(struct smtmul_workload),	/* wlt_params_size*/
	sizeof(struct smtmul_request),	/* wlt_rqparams_size */

	smtmul_wl_config,				/* wlt_wl_config */
	smtmul_wl_unconfig,				/* wlt_wl_unconfig */

	NULL,							/* wlt_wl_step */

	smtmul_run_request				/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &smtmul_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &smtmul_wlt);

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
