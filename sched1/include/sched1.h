/*
 * sched1.h
 *
 *  Created on: 30.07.2013
 *      Author: myaut
 */

#ifndef SCHED1_H_
#define SCHED1_H_

#include <list.h>
#include <wlparam.h>
#include <cpuinfo.h>
#include <atomic.h>
#include <syncqueue.h>
#include <threads.h>
#include <tstime.h>

#define SCHED1_TRACE_TIMES

#define ELEMENT(matrix, M, i, j) matrix->M[i + j * matrix->size]

struct sched1_matrix {
	int size;	/* Matrices are square to simplify algorithm */

	int* A;
	int* B;
	int* M;
};

struct sched1_params {
	hi_cpu_object_t* strand;

	int ping_count;
	int pong_count;

	ts_time_t cpu_duration;
	ts_time_t sleep_duration;
};

struct sched1_pong_thread {
	thread_t thread;
};

struct sched1_ping_thread {
	thread_t thread;
	list_head_t rqqueue;
	thread_mutex_t rqqmutex;
	thread_cv_t cv;

	ts_time_t last_rq_delta;
};

struct sched1_workload {
	thread_event_t notifier;

	struct sched1_ping_thread* ping_threads;
	struct sched1_pong_thread* pong_threads;

	atomic_t done;
	atomic_t step;
	atomic_t rqid;

	squeue_t sq;

	unsigned num_requests;

	thread_mutex_t rq_chain_mutex;
	list_head_t*   rq_chain;

	struct sched1_matrix matrix;
};

#endif /* SCHED1_H_ */
