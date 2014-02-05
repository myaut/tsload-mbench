#ifndef MOD_LOAD_BIGMEM_H_
#define MOD_LOAD_BIGMEM_H_

#include <wlparam.h>
#include <randgen.h>

#define ACCESS_MM			0
#define ACCESS_MR			1
#define ACCESS_RR			2

#define INSTRUCTION_SUM		0
#define INSTRUCTION_MUL		1
#define INSTRUCTION_CMP		2

#define MAX_STEP_SIZE		4096
#define UNROLL_COUNT		8		/* Also change macro UNROLL8 in bigmem.c */
#define MIN_STEP			24

#define BIGMEM_TRACE

struct bigmem_workload {
	wlp_integer_t mempool_size;
};

struct bigmem_request {
	wlp_integer_t cycles;
	wlp_integer_t offset;
	wlp_integer_t step;
	wlp_strset_t access;
	wlp_strset_t instruction;
};

struct bigmem_workload_private {
	void* ptr;
};

#endif
