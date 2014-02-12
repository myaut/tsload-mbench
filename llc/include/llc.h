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
	wlp_integer_t num_cycles;
	wlp_integer_t num_accesses;

	hi_cpu_object_t* cpu_object;
};

struct llc_data {
	size_t size;
	size_t line_size;
};

#endif /* LLC_H_ */
