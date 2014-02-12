/*
 * bigmem.c
 *
 *  Created on: Dec 23, 2013
 *      Author: myaut
 */

#define LOG_SOURCE "bigmem"
#include <log.h>

#include <mempool.h>
#include <defs.h>
#include <workload.h>
#include <wltype.h>
#include <modules.h>
#include <modapi.h>
#include <threads.h>

#include <bigmem.h>

#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("bigmem");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT const char* access_set[] = { "mm", "mr", "rr" };
MODEXPORT const char* instructions_set[] = { "sum", "mul", "cmp" };

MODEXPORT wlp_descr_t bigmem_params[] = {
	{ WLP_SIZE, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"mempool_size",
		"Memory pool size",
		offsetof(struct bigmem_workload, mempool_size) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"cycles",
		"Number of cycles",
		offsetof(struct bigmem_request, cycles) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"offset",
		"Starting offset inside memory pool",
		offsetof(struct bigmem_request, offset) },
	{ WLP_INTEGER, WLPF_REQUEST,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"step",
		"Iteration step over memory pool",
		offsetof(struct bigmem_request, step) },
	{ WLP_STRING_SET, WLPF_REQUEST,
		WLP_STRING_SET_RANGE(access_set),
		WLP_NO_DEFAULT(),
		"access",
		"Indicates if operation is memory-intensive or not ",
		offsetof(struct bigmem_request, access) },
	{ WLP_STRING_SET, WLPF_REQUEST,
		WLP_STRING_SET_RANGE(instructions_set),
		WLP_NO_DEFAULT(),
		"instruction",
		"Instruction to be executed (sum, mul, cmp)",
		offsetof(struct bigmem_request, instruction) },
	{ WLP_NULL }
};

module_t* self = NULL;

MODEXPORT int bigmem_wl_config(workload_t* wl) {
	struct bigmem_workload* bmwp = (struct bigmem_workload*) wl->wl_params;
	struct bigmem_workload_private* bm;
	void* ptr;

	/* Allocate huge mmap segment */
	ptr = mmap(NULL, bmwp->mempool_size, PROT_READ | PROT_WRITE,
							MAP_SHARED | MAP_ANON, -1, 0);

	if(ptr == NULL) {
		wl_notify( wl, WLS_CFG_FAIL, 0,
				   "Couldn't allocate %lld bytes of memory", (unsigned long long) bmwp->mempool_size );
		return 1;
	}

	bm = mp_malloc(sizeof(struct bigmem_workload_private));
	bm->ptr = ptr;

	wl->wl_private = bm;

	return 0;
}

MODEXPORT int bigmem_wl_unconfig(workload_t* wl) {
	struct bigmem_workload* bmwp = (struct bigmem_workload*) wl->wl_params;
	struct bigmem_workload_private* bm = (struct bigmem_workload_private*) wl->wl_private;

	munmap(bm->ptr, bmwp->mempool_size);

	mp_free(bm);

	return 0;
}

#define		UNROLL8(expr1, expr2)			\
	expr1; expr2; expr1; expr1;				\
	expr2; expr2; expr1; expr2;

#define 	INSTR_RR(op, r1, r2, r3)	r3 = r2 op r1
#define 	INSTR_MR(op, r2)	r2 = r2 op *p++
#define 	INSTR_MM(op)	*p++ = *p++ op *p++

/* min macro will re-evaluate expression, so
 * *p++ will give offset of to words. But we don't care
 * because we have large pool of words and do not check correctenss
 * of operations */
#define 	CMP_RR(op, r1, r2, r3)	r3 = op(r2, r1)
#define 	CMP_MR(op, r2)	r2 = op(r2, *p++)
#define 	CMP_MM(op)	*p++ = op(*p++, *p++)

#define BIGMEM_CYCLE(expr1, expr2)		\
		while(cycles > 0) {				\
			UNROLL8(expr1, expr2);		\
			cycles -= UNROLL_COUNT;		\
		}

#define BIGMEM_CYCLE_MEM(expr1, expr2)	\
		while(cycles > 0) {				\
			p2 = p + step;				\
			UNROLL8(expr1, expr2);		\
			cycles -= UNROLL_COUNT;		\
			p = p2;						\
		}

#define ALIGN(offset)	((offset) & ~((ptrdiff_t)15))

MODEXPORT int bigmem_run_request(request_t* rq) {
	struct bigmem_workload* bmwp = (struct bigmem_workload*) rq->rq_workload->wl_params;
	struct bigmem_workload_private* bm = (struct bigmem_workload_private*) rq->rq_workload->wl_private;
	struct bigmem_request* bmrq = (struct bigmem_request*) rq->rq_params;

	int total_cycles = bmrq->cycles;
	int max_cycles;
	int cycles = 0;
	ptrdiff_t offset, step, step_bytes;

	volatile register long r1 = 7890, r2 = 3456, r3 = 0;
	volatile register long r4 = 123, r5 = 890123, r6 = 0;
	volatile register long *p, *p2;

	if(bmrq->offset < 0)
		bmrq->offset = -bmrq->offset;

	bmrq->step = bmrq->step % MAX_STEP_SIZE;
	bmrq->offset = bmrq->offset % bmwp->mempool_size;

	offset = ALIGN(bmrq->offset);
	step_bytes = ALIGN(bmrq->step);

	if(step_bytes < MIN_STEP)
		step_bytes = MIN_STEP;

	step = step_bytes / sizeof(long);

	max_cycles = (bmwp->mempool_size - MAX_STEP_SIZE) / step_bytes;
	cycles = min(max_cycles - (offset / step_bytes), total_cycles);

#ifdef BIGMEM_TRACE
	logmsg(LOG_TRACE, "Running bigmem request %s:%s %d cycles (%d+) @%llu+%llu(%llu)",
						instructions_set[bmrq->instruction],
						access_set[bmrq->access],
						total_cycles, cycles,
						(unsigned long long) offset,
						(unsigned long long) step,
						(unsigned long long) step_bytes);
#endif

	while(cycles > 0) {
		p = bm->ptr + offset;
		total_cycles -= cycles;

		switch(bmrq->instruction) {
		case INSTRUCTION_SUM:
			switch(bmrq->access) {
			case ACCESS_RR:
				BIGMEM_CYCLE(INSTR_RR(+, r1, r2, r3), INSTR_RR(+, r4, r5, r6));
				break;
			case ACCESS_MR:
				BIGMEM_CYCLE_MEM(INSTR_MR(+, r3), INSTR_MR(+, r6));
				break;
			case ACCESS_MM:
				BIGMEM_CYCLE_MEM(INSTR_MM(+), INSTR_MM(+));
				break;
			}
			break;
			case INSTRUCTION_MUL:
				switch(bmrq->access) {
				case ACCESS_RR:
					BIGMEM_CYCLE(INSTR_RR(*, r1, r2, r3), INSTR_RR(*, r4, r5, r6));
					break;
				case ACCESS_MR:
					BIGMEM_CYCLE_MEM(INSTR_MR(*, r3), INSTR_MR(*, r6));
					break;
				case ACCESS_MM:
					BIGMEM_CYCLE_MEM(INSTR_MM(*), INSTR_MM(+));
					break;
				}
				break;
			case INSTRUCTION_CMP:
				switch(bmrq->access) {
				case ACCESS_RR:
					BIGMEM_CYCLE(CMP_RR(min, r1, r2, r3), CMP_RR(max, r4, r5, r6));
					break;
				case ACCESS_MR:
					BIGMEM_CYCLE_MEM(CMP_MR(min, r3), CMP_MR(max, r6));
					break;
				case ACCESS_MM:
					BIGMEM_CYCLE_MEM(CMP_MM(min), CMP_MM(max));
					break;
				}
				break;
		}

		offset = 0;
	}

	return 0;
}

wl_type_t bigmem_wlt = {
	"bigmem",							/* wlt_name */

	WLC_CPU_MEMORY,					/* wlt_class */

	bigmem_params,					/* wlt_params */
	sizeof(struct bigmem_workload),	/* wlt_params_size*/
	sizeof(struct bigmem_request),	/* wlt_rq_params_size*/

	bigmem_wl_config,					/* wlt_wl_config */
	bigmem_wl_unconfig,				/* wlt_wl_unconfig */

	NULL,							/* wlt_wl_step */

	bigmem_run_request				/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &bigmem_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &bigmem_wlt);

	return MOD_OK;
}
