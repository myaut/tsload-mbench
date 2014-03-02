/*
 * llc.h
 *
 *  Created on: 05.01.2014
 *      Author: myaut
 */

#ifndef LLC_H_
#define LLC_H_

#include <wlparam.h>
#include <cpuinfo.h>

#include <stdlib.h>

struct llc_workload {
	wlp_integer_t num_accesses;
	wlp_float_t mem_size;

	hi_cpu_object_t* cpu_object;
};

struct llc_request {
	wlp_integer_t offset;
	wlp_integer_t cache_misses;
};

struct llc_data {
	void* ptr;

	int perf_fd;
	void* perf_mem;

	size_t size;
	size_t line_size;
};

#endif /* LLC_H_ */
