/*
 * sched1.c
 *
 *  Created on: 30.07.2013
 *      Author: myaut
 */

#define LOG_SOURCE "sched1"
#include <log.h>

#include <mempool.h>
#include <defs.h>
#include <workload.h>
#include <wltype.h>
#include <modules.h>
#include <modapi.h>
#include <cpuinfo.h>
#include <cpumask.h>
#include <schedutil.h>
#include <etrace.h>

#include <sched1.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("sched1");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT wlp_descr_t sched1_params[] = {
	{ WLP_CPU_OBJECT, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"strand",
		"Strand where threads are bound",
		offsetof(struct sched1_params, strand) },
	{ WLP_TIME, WLPF_NO_FLAGS,
		WLP_INT_RANGE(1, 16),
		WLP_INT_DEFAULT(1),
		"ping_count",
		"Number of ping-threads",
		offsetof(struct sched1_params, ping_count) },
	{ WLP_TIME, WLPF_NO_FLAGS,
		WLP_INT_RANGE(1, 16),
		WLP_INT_DEFAULT(1),
		"pong_count",
		"Number of pong-threads",
		offsetof(struct sched1_params, pong_count) },
	{ WLP_TIME, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"cpu_duration",
		"Duration of CPU-intensive request",
		offsetof(struct sched1_params, cpu_duration) },
	{ WLP_TIME, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"sleep_duration",
		"Duration of sleep-intensive request",
		offsetof(struct sched1_params, sleep_duration) },
	{ WLP_NULL }
};

/* a794dbe1-13fb-46fb-91a3-6a1f607e3ea1 */
#define ETRC_GUID_MBENCH_SCHED1	{0xa794dbe1, 0x13fb, 0x46fb, {0x91, 0xa3, 0x6a, 0x1f, 0x60, 0x7e, 0x3e, 0xa1}}

ETRC_DEFINE_PROVIDER(mbench__sched1, ETRC_GUID_MBENCH_SCHED1);

ETRC_DEFINE_EVENT(mbench__sched1, ping__request__start, 1);
ETRC_DEFINE_EVENT(mbench__sched1, ping__request__finish, 2);
ETRC_DEFINE_EVENT(mbench__sched1, ping__send__pong, 3);
ETRC_DEFINE_EVENT(mbench__sched1, pong__pop, 4);
ETRC_DEFINE_EVENT(mbench__sched1, pong__sleep, 5);
ETRC_DEFINE_EVENT(mbench__sched1, pong__wakeup, 6);

module_t* self = NULL;
ts_time_t period = 10 * T_SEC;

thread_result_t sched1_ping_thread(thread_arg_t arg);
thread_result_t sched1_pong_thread(thread_arg_t arg);

void sched1_matrix_init(struct sched1_matrix* matrix, unsigned int size);
void sched1_matrix_destroy(struct sched1_matrix* matrix);
void sched1_matrix_mul(struct sched1_matrix* matrix);

int sched1_wl_training(workload_t* wl);

static ts_time_t tm_abs_diff(ts_time_t a, ts_time_t b) {
	if(a > b)
		return a - b;
	return b - a;
}

static
ts_time_t generate_rq_iat(unsigned num_requests) {
	double u = ((double) rand()) / RAND_MAX;
	double x = log(1.0 - u) / -((double) num_requests);

	return (ts_time_t) (x * ((double) period));
}

MODEXPORT int sched1_wl_config(workload_t* wl) {
	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = NULL;
	hi_cpu_object_t* strand = (hi_cpu_object_t*) sched1->strand;
	cpumask_t* mask;

	thread_t* t;
	list_head_t* rqqueue;
	thread_mutex_t* mutex;
	thread_cv_t* cv;

	int ret;
	int ti;

	if(strand->type != HI_CPU_STRAND) {
		wl_notify(wl, WLS_FAIL, -1, "strand parameter should be strand object");

		return -1;
	}

	mask = cpumask_create();
	cpumask_set(mask, strand->id);

	workload = mp_malloc(sizeof(struct sched1_workload));

	atomic_set(&workload->done, B_FALSE);
	atomic_set(&workload->step, -1);
	atomic_set(&workload->rqid, 0);

	workload->rq_chain = mp_malloc(sizeof(list_head_t));
	list_head_init(workload->rq_chain, "sched1-rq");
	mutex_init(&workload->rq_chain_mutex, "rqchain");

	wl->wl_private = workload;

	event_init(&workload->notifier, "start_notifier");

	squeue_init(&workload->sq, "sched1-sq");

	workload->ping_threads = mp_malloc(sizeof(struct sched1_ping_thread) * sched1->ping_count);
	for(ti = 0; ti < sched1->ping_count; ++ti) {
		t  		= &workload->ping_threads[ti].thread;
		rqqueue = &workload->ping_threads[ti].rqqueue;
		mutex 	= &workload->ping_threads[ti].rqqmutex;
		cv 		= &workload->ping_threads[ti].cv;

		workload->ping_threads[ti].last_rq_delta = 0;

		list_head_init(rqqueue, "rqqueue-%d", ti);
		mutex_init(mutex, "rqqmtx-%d", ti);
		cv_init(cv, "pingcv-%d", ti);

		t_init(t, (void*) wl, sched1_ping_thread, "ping-%d", ti);
		sched_set_affinity(t, mask);
	}

	workload->pong_threads = mp_malloc(sizeof(struct sched1_pong_thread) * sched1->pong_count);
	for(ti = 0; ti < sched1->pong_count; ++ti) {
		t  = &workload->pong_threads[ti].thread;

		t_init(t, (void*) wl, sched1_pong_thread, "pong-%d", ti);
		sched_set_affinity(t, mask);
	}

	cpumask_destroy(mask);

	ret = sched1_wl_training(wl);

	if(ret > 0)
		wl_notify(wl, WLS_FAIL, -1, "Too many consecutive failures.");

	srand(tm_get_time() / T_SEC);

	return ret;
}

MODEXPORT int sched1_wl_unconfig(workload_t* wl) {
	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	int ti;

	if(workload) {
		wl_rq_chain_push(workload->rq_chain);

		for(ti = 0; ti < sched1->pong_count; ++ti) {
			/* Finish pong threads */
			squeue_push(&workload->sq, NULL);
		}

		atomic_set(&workload->step, -1);
		atomic_set(&workload->done, B_TRUE);

		event_notify_all(&workload->notifier);

		for(ti = 0; ti < sched1->ping_count; ++ti) {
			mutex_lock(&workload->ping_threads[ti].rqqmutex);
			cv_notify_one(&workload->ping_threads[ti].cv);
			mutex_unlock(&workload->ping_threads[ti].rqqmutex);

			t_destroy(&workload->ping_threads[ti].thread);
		}

		for(ti = 0; ti < sched1->ping_count; ++ti) {
			cv_destroy(&workload->ping_threads[ti].cv);
			mutex_destroy(&workload->ping_threads[ti].rqqmutex);
		}

		for(ti = 0; ti < sched1->pong_count; ++ti) {
			t_destroy(&workload->pong_threads[ti].thread);
		}

		squeue_destroy(&workload->sq, mp_free);

		event_destroy(&workload->notifier);
		mutex_destroy(&workload->rq_chain_mutex);

		sched1_matrix_destroy(&workload->matrix);

		mp_free(workload->ping_threads);
		mp_free(workload->pong_threads);
	}

	return 0;
}

int sched1_wl_training(workload_t* wl) {
	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	int size1 = 8, size2 = 12;

	int size = size1;
	int mode = 1;
	ts_time_t start, end, t1, t2;
	double C, D, K;

	int 	  best_size = 0;
	ts_time_t best_time = TS_TIME_MAX;
	ts_time_t cur_time;

	int hits = 0;
	int fails = 0;

	/* Determine matrix size for desired duration
	 *
	 * Assuming:
	 * 		t = C * size^3 + D
	 *
	 * measure it for size = size1 and size = size2, then solve system of equations:
	 * {	t = (size1^3) * C + D
	 * {	t = (size2^3)* C + D
	 *
	 * C =  */
	do {
		sched1_matrix_init(&workload->matrix, size);

		start = tm_get_clock();
		sched1_matrix_mul(&workload->matrix);
		end = tm_get_clock();

		switch(mode) {
		case 1:
			t1 = tm_diff(start, end);
			size = size2;

			break;
		case 2:
			t2 = tm_diff(start, end);

			if(t2 < t1) {
				/* Skew is high - may be we run under debugger?
				 *
				 * Try with larger sizes to mitigate skew,
				 * If too much fails, then oops! */
				if(++fails > 3) {
					best_size = 8;
					return fails;
				}

				size2 *= 2;
				size = size1;

				logmsg(LOG_WARN, "Skew when training: %lld < %lld, trying with %d:%d", t2 , t1, size1, size2);

				mode = 0;
				continue;
			}
			else {
				C = ((double) (t2 - t1)) / (double) ((size2 * size2 * size2) - (size1 * size1 * size1));
				D = ((double) t1) - (8 * C);

				logmsg(LOG_INFO, "Calculated C=%f D=%f", C, D);

				size = (int) cbrt(((double) sched1->cpu_duration - D) / C);
				best_size = size;

				fails = 0;
			}

			break;
		default:
			cur_time = tm_diff(start, end);

			K = (double) sched1->cpu_duration / (double) cur_time;

			if(mode == 3 && fabs(K - 1.) > 0.1) {
				--mode;

				best_size = size = (int) cbrt((K * C * pow(size, 3) + (K - 1.) * D) / C);

				logmsg(LOG_WARN, "Duration error is too big: K=%f, trying with size %d", K, size);

				if(++fails == 5) {
					return fails;
				}
			}
			else {
				if((mode == 3
					|| tm_abs_diff(cur_time, sched1->cpu_duration) <
						tm_abs_diff(best_time, sched1->cpu_duration))
					&& size != best_size) {
						best_time = cur_time;
						best_size = size;
				}
				else {
					/* We didn't improve results this time */
					++hits;
				}

				logmsg(LOG_INFO, "Desired duration %lld, size %d, real duration %lld, best duration %lld",
						sched1->cpu_duration, size, cur_time, best_time);

				if(cur_time < sched1->cpu_duration)
					++size;
				else
					--size;
			}

			break;
		}

		++mode;

		sched1_matrix_destroy(&workload->matrix);
	} while(hits < 3);

	sched1_matrix_init(&workload->matrix, best_size);

	return 0;
}

MODEXPORT int sched1_wl_step(workload_t* wl, unsigned num_requests) {
	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	request_t* rq;

	long rq_id;
	int rq_count;

	ts_time_t next_time = 0;
	ts_time_t last_time;

	thread_t* t;
	list_head_t* rqqueue;
	thread_mutex_t* mutex;
	thread_cv_t* cv;
	ts_time_t last_rq_delta;

	int ti;

	workload->num_requests = num_requests;

	/*
	 * Put requests onto worker's queue, then wake up it
	 * */
	for(ti = 0; ti < sched1->ping_count; ++ti) {
		t  		= &workload->ping_threads[ti].thread;
		rqqueue = &workload->ping_threads[ti].rqqueue;
		mutex 	= &workload->ping_threads[ti].rqqmutex;
		cv 		= &workload->ping_threads[ti].cv;
		last_rq_delta = workload->ping_threads[ti].last_rq_delta;

		last_time = next_time = generate_rq_iat(num_requests);

#ifdef SCHED1_INTERSTEP_PENALTY
		/* Due to step nature of request scheduler we lose information
		 * about interval between last rq arrival during previous step,
		 * making inter-step arrival interval larger than it would be
		 * for exponentially variated sequence.
		 *
		 * Account inter-step interval in first request  */
		if(next_time < last_rq_delta)
			next_time = 0;
		else
			next_time -= last_rq_delta;
#endif

		rq_count = 0;

		mutex_lock(mutex);

		while(next_time < period) {
			rq_id = atomic_inc(&workload->rqid);

			rq = wl_create_request(wl, ti);
			rq->rq_id = rq_id;
			rq->rq_start_time = next_time;

			list_add_tail(&rq->rq_node, rqqueue);

			last_time = next_time;
			next_time += generate_rq_iat(num_requests);

			++rq_count;
		}

		last_rq_delta = (period - last_time);
		workload->ping_threads[ti].last_rq_delta = last_rq_delta;

		cv_notify_one(cv);
		mutex_unlock(mutex);

		logmsg(LOG_INFO, "%s step %ld requests %d isp %9.4f", t->t_name,
				wl->wl_current_step, rq_count,
				((double) last_rq_delta) / T_MS);
	}

	atomic_set(&workload->step, wl->wl_current_step);

	return 0;
}

wl_type_t sched1_wlt = {
	"sched1",						/* wlt_name */

	WLC_OS_BENCHMARK,				/* wlt_class */

	sched1_params,					/* wlt_params */
	sizeof(struct sched1_params),	/* wlt_params_size*/

	sched1_wl_config,				/* wlt_wl_config */
	sched1_wl_unconfig,				/* wlt_wl_unconfig */

	sched1_wl_step,					/* wlt_wl_step */

	NULL							/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &sched1_wlt);

	etrc_provider_init(&mbench__sched1);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	etrc_provider_destroy(&mbench__sched1);

	wl_type_unregister(mod, &sched1_wlt);

	return MOD_OK;
}

thread_result_t sched1_ping_thread(thread_arg_t arg) {
	THREAD_ENTRY(arg, workload_t, wl);

	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	struct sched1_ping_thread* ping_thread;

	long* msg = NULL;
	int rq_count = 0;
	long rq_id;
	volatile int cycle = 0;

	long current_step = 0, step;
	int qlen = 0, qmax = 0;

	ts_time_t next_time = 0;	/* Time of current request */
	ts_time_t prev_time = 0;	/* Time of previos request */
	ts_time_t cur_time = 0;		/* Clock time */
	ts_time_t last_time = 0;	/* Clock time of last request */
	ts_time_t step_time = 0;

	boolean_t miss = B_FALSE;
	double    qmean;

	int pong_id = 0;
	squeue_t* sq = NULL;

	request_t* rq;

	list_head_t rqqueue;

	ping_thread = container_of(thread, struct sched1_ping_thread, thread);

	list_head_init(&rqqueue, "rqqueue");

	logmsg(LOG_INFO, "[%s] thread id: %lu", thread->t_name, thread->t_system_id);

	while(atomic_read(&workload->done) != B_TRUE) {
		rq_count = 0;
		qmean = 0.0;

		/* Wait till next step */
		mutex_lock(&ping_thread->rqqmutex);

		if(list_empty(&ping_thread->rqqueue)) {
			cv_wait(&ping_thread->cv, &ping_thread->rqqmutex);

			if(atomic_read(&workload->done) == B_TRUE) {
				mutex_unlock(&ping_thread->rqqmutex);
				break;
			}
		}

		list_splice_init(&ping_thread->rqqueue, list_head_node(&rqqueue));
		mutex_unlock(&ping_thread->rqqmutex);

		step_time = tm_get_clock();

		while(!list_empty(&rqqueue)) {
			/*
			 * Extract single request from queue. If it's time has come, start
			 * execute it immediately, sleep otherwise.
			 * */
			cur_time = tm_get_clock();

#ifdef SCHED1_ACCOUNT_QSTATS
			qlen = 0;

			/* Measure queue length */
			list_for_each_entry(request_t, rq, &rqqueue, rq_node) {
				if(rq->rq_start_time > (cur_time - step_time))
					break;

				++qlen;
				qmean += ((double) (cur_time - max(rq->rq_start_time,
											   last_time))) / ((double) T_SEC);
			}

			last_time = cur_time;

			if(qlen > qmax)
				qmax = qlen;
#endif

			rq = list_first_entry(request_t, &rqqueue, rq_node);

			/* step provides normalized time so we do not account
			 * time spent in sched1_wl_step() */
			rq->rq_start_time += step_time;
			next_time = rq->rq_start_time;

			list_del(&rq->rq_node);

			rq_id = rq->rq_id;

			miss = next_time < cur_time;
			if(next_time > cur_time) {
				tm_sleep_nano(tm_diff(cur_time, next_time));
#ifdef SCHED1_TRACE_TIMES
				cur_time = tm_get_clock();
#endif
			}

			/* Simulate busy working */
			ETRC_PROBE1(mbench__sched1, ping__request__start, long, rq_id);
			sched1_matrix_mul(&workload->matrix);
			ETRC_PROBE1(mbench__sched1, ping__request__finish, long, rq_id);

			ETRC_PROBE1(mbench__sched1, ping__send__pong, long, rq_id);
			squeue_push(&workload->sq, (void*) rq);

			++rq_count;

#ifdef SCHED1_TRACE_TIMES
			logmsg(LOG_TRACE, "[%s]: step %3ld rq: %5ld wait: %9.4f ms iat: %9.4f ms time: %9.4f ms %s qlen: %d",
					thread->t_name,
					current_step, rq_id,
					((double) cur_time - next_time) / T_MS,
					((double) next_time - prev_time) / T_MS,
					((double) tm_get_clock() - next_time) / T_MS,
					(miss)? "missed" : "",
					qlen);

			prev_time = next_time;
#endif
		}

		logmsg(LOG_INFO, "[%s]: step %ld requests %d qmax %d qlen %f",
				thread->t_name,
				current_step, rq_count, qmax, qmean);
		qmax = 0;
	}

	THREAD_END:
		THREAD_FINISH(arg);
}

thread_result_t sched1_pong_thread(thread_arg_t arg) {
	THREAD_ENTRY(arg, workload_t, wl);

	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	request_t* rq = NULL;
	unsigned long rq_id;

	ts_time_t	sleep_time;

	struct sched1_pong_thread* pong_thread = container_of(thread, struct sched1_pong_thread, thread);

	logmsg(LOG_INFO, "[%s] thread id: %lu", thread->t_name, thread->t_system_id);

	while(B_TRUE) {
		ETRC_PROBE1(mbench__sched1, pong__pop, unsigned long, 0);

		rq = (request_t*) squeue_pop(&workload->sq);

		if(rq == NULL) {
			THREAD_EXIT(0);
		}

		rq_id = rq->rq_id;

#ifdef SCHED1_TRACE_TIMES
		sleep_time = tm_get_clock();
#endif

		ETRC_PROBE1(mbench__sched1, pong__sleep, unsigned long, rq_id);

		tm_sleep_nano(sched1->sleep_duration);

		ETRC_PROBE1(mbench__sched1, pong__wakeup, unsigned long, rq_id);

		rq->rq_end_time = tm_get_clock();

#ifdef SCHED1_TRACE_TIMES
		logmsg(LOG_TRACE, "[%s]: step %3ld rq: %5ld sleep: %9.4f ms time: %9.4f ms",
							thread->t_name, wl->wl_current_step, rq_id,
							((double) sleep_time - rq->rq_start_time) / T_MS,
							((double) rq->rq_end_time - rq->rq_start_time) / T_MS);
#endif

		mutex_lock(&workload->rq_chain_mutex);
		list_add_tail(&rq->rq_node, workload->rq_chain);
		mutex_unlock(&workload->rq_chain_mutex);
	}

	THREAD_END:
		THREAD_FINISH(arg);
}

void sched1_matrix_init(struct sched1_matrix* matrix, unsigned int size) {
	int i, j;
	size_t alloc_size;

	if(size == 0)
		size = 1;

	alloc_size = size * size * sizeof(int);

	matrix->size = size;

	matrix->A = (int*) mp_malloc(alloc_size);
	matrix->B = (int*) mp_malloc(alloc_size);
	matrix->M = (int*) mp_malloc(alloc_size);

	for(i = 0; i < size; ++i) {
		for(j = 0; j < size; ++j) {
			ELEMENT(matrix, A, i, j) = i << 8 | j;
			ELEMENT(matrix, B, i, j) = i | j << 8;
		}
	}
}

void sched1_matrix_destroy(struct sched1_matrix* matrix) {
	mp_free((void*) matrix->A);
	mp_free((void*) matrix->B);
	mp_free((void*) matrix->M);
}

/* M = A * B
 * O(n^3) */
void sched1_matrix_mul(struct sched1_matrix* matrix) {
	int i, j, k;

	for(i = 0; i < matrix->size; ++i) {
		for(j = 0; j < matrix->size; ++j) {
			ELEMENT(matrix, M, i, j) = 0;
			for(k = 0; k < matrix->size; ++k) {
				ELEMENT(matrix, M, i, j) +=
						ELEMENT(matrix, A, i, k) +
						ELEMENT(matrix, B, k, j);
			}
		}
	}
}
