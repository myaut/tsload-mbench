/*
 * nodemem.h
 *
 *  Created on: 03.03.2014
 *      Author: myaut
 */

#ifndef NODEMEM_H_
#define NODEMEM_H_

#include <wlparam.h>
#include <cpuinfo.h>

#include <stdlib.h>

struct nodemem_workload {
	wlp_integer_t num_accesses;

	hi_cpu_object_t* node_object;
};

struct nodemem_request {
	wlp_integer_t offset;
	wlp_integer_t node_misses;
};

struct nodemem_data {
	void* local_ptr;
	void* remote_ptr;

	int perf_fd;
	void* perf_mem;

	size_t size;
	size_t pool_size;
};

#endif /* NODEMEM_H_ */
