/*
 * smtmul.h
 *
 *  Created on: Mar 3, 2014
 *      Author: myaut
 */

#ifndef SMTMUL_H_
#define SMTMUL_H_

#include <wlparam.h>

struct smtmul_workload {
	wlp_integer_t num_instructions;
	wlp_bool_t enable_perf;
};

struct smtmul_request {
	wlp_integer_t resource_stalls;
};

struct smtmul_data {
	int perf_fd;
	void* perf_mem;
};

#endif /* SMTMUL_H_ */
