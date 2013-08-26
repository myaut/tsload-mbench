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
#include <tstime.h>

#define ELEMENT(matrix, M, i, j) matrix->M[i + j * matrix->size]

typedef struct sched1_matrix {
	int size;	/* Matrices are square to simplify algorithm */

	int* A;
	int* B;
	int* M;
};

struct sched1_params {
	hi_cpu_object_t* strand;
	ts_time_t duration;
};

struct sched1_workload {
	thread_event_t notifier;

	thread_t ping;
	thread_t pong;

	squeue_t sq;

	atomic_t done;

	unsigned num_iterations;

	struct sched1_matrix matrix;
};

#endif /* SCHED1_H_ */
