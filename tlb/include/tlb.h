/*
 * tlb.h
 *
 *  Created on: 05.01.2014
 *      Author: myaut
 */

#ifndef TLB_H
#define TLB_H

#include <wlparam.h>
#include <cpuinfo.h>

#include <stdlib.h>

struct tlb_workload {
	wlp_integer_t num_accesses;
};

struct tlb_request {
	wlp_integer_t step;
	wlp_integer_t offset;
	wlp_integer_t tlb_misses;
};

struct tlb_data {
	void* ptr;

	int perf_fd;
	void* perf_mem;

	size_t size;
};

#endif /* TLB_H */
