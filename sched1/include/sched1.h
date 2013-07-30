/*
 * sched1.h
 *
 *  Created on: 30.07.2013
 *      Author: myaut
 */

#ifndef SCHED1_H_
#define SCHED1_H_

#include <wlparam.h>
#include <cpuinfo.h>
#include <atomic.h>
#include <syncqueue.h>
#include <threads.h>

struct sched1_params {
	hi_cpu_object_t* strand;
	wlp_integer_t num_cycles;
};

struct sched1_workload {
	thread_event_t notifier;

	thread_t ping;
	thread_t pong;

	squeue_t sq;

	atomic_t done;

	unsigned num_iterations;
};

#endif /* SCHED1_H_ */
