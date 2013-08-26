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
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"duration",
		"Duration of CPU-intensive request",
		offsetof(struct sched1_params, duration) },
	{ WLP_NULL }
};

/* a794dbe1-13fb-46fb-91a3-6a1f607e3ea1 */
#define ETRC_GUID_MBENCH_SCHED1	{0xa794dbe1, 0x13fb, 0x46fb, {0x91, 0xa3, 0x6a, 0x1f, 0x60, 0x7e, 0x3e, 0xa1}}

ETRC_DEFINE_PROVIDER(mbench__sched1, ETRC_GUID_MBENCH_SCHED1);

ETRC_DEFINE_EVENT(mbench__sched1, ping__request__start, 1);
ETRC_DEFINE_EVENT(mbench__sched1, ping__request__finish, 2);
ETRC_DEFINE_EVENT(mbench__sched1, ping__send__pong, 3);
ETRC_DEFINE_EVENT(mbench__sched1, pong__on__cpu, 4);

module_t* self = NULL;

thread_result_t sched1_ping_thread(thread_arg_t arg);
thread_result_t sched1_pong_thread(thread_arg_t arg);

void sched1_matrix_init(struct sched1_matrix* matrix, unsigned int size);
void sched1_matrix_destroy(struct sched1_matrix* matrix);
void sched1_matrix_mul(struct sched1_matrix* matrix);

void sched1_wl_training(workload_t* wl);

static ts_time_t tm_abs_diff(ts_time_t a, ts_time_t b) {
	if(a > b)
		return a - b;
	return b - a;
}

MODEXPORT int sched1_wl_config(workload_t* wl) {
	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = NULL;
	hi_cpu_object_t* strand = (hi_cpu_object_t*) sched1->strand;
	cpumask_t* mask;

	if(strand->type != HI_CPU_STRAND) {
		wl_notify(wl, WLS_FAIL, -1, "strand parameter should be strand object");

		return -1;
	}

	mask = cpumask_create();
	cpumask_set(mask, strand->id);

	workload = mp_malloc(sizeof(struct sched1_workload));

	atomic_set(&workload->done, B_FALSE);

	wl->wl_private = workload;

	squeue_init(&workload->sq, "sched1-sq");

	event_init(&workload->notifier, "start_notifier");

	t_init(&workload->ping, (void*) wl, sched1_ping_thread,
			"ping-%d", strand->id);
	t_init(&workload->pong, (void*) wl, sched1_pong_thread,
			"pong-%d", strand->id);

	sched_set_affinity(&workload->ping, mask);
	sched_set_affinity(&workload->pong, mask);

	cpumask_destroy(mask);

	sched1_wl_training(wl);

	return 0;
}

MODEXPORT int sched1_wl_unconfig(workload_t* wl) {
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	if(workload) {
		atomic_set(&workload->done, B_TRUE);

		event_notify_all(&workload->notifier);

		t_destroy(&workload->ping);
		t_destroy(&workload->pong);

		event_destroy(&workload->notifier);

		squeue_destroy(&workload->sq, mp_free);

		sched1_matrix_destroy(&workload->matrix);
	}

	return 0;
}

void sched1_wl_training(workload_t* wl) {
	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	int size = 8;
	int mode = 1;
	ts_time_t start, end, t1, t2;
	double C, D;

	int 	  best_size = 0;
	ts_time_t best_time = TS_TIME_MAX;
	ts_time_t cur_time;

	int hits = 0;

	/* Determine matrix size for desired duration
	 *
	 * Assuming:
	 * 		t = C * size^3 + D
	 *
	 * measure it for size = 8 and size = 12, then solve system of equations:
	 * {	t = 1728 * C + D
	 * {	t =  512 * C + D
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
			size = 12;

			break;
		case 2:
			t2 = tm_diff(start, end);

			C = ((double) (t2 - t1)) / 1216.0;
			D = ((double) t1) - (8 * C);

			logmsg(LOG_INFO, "Calculated C=%f D=%f", C, D);

			size = (int) cbrt(((double) sched1->duration - D) / C);

			best_size = size;

			break;
		default:
			cur_time = tm_diff(start, end);

			if(mode == 3 ||
			   tm_abs_diff(cur_time, sched1->duration) < tm_abs_diff(best_time, sched1->duration)) {
					best_time = cur_time;
					best_size = size;
			}
			else {
				/* We didn't improve results this time */
				++hits;
			}

			logmsg(LOG_INFO, "Desired duration %lld, size %d, real duration %lld, best duration %lld",
					sched1->duration, size, cur_time, best_time);

			if(cur_time < sched1->duration) {
				++size;
			}
			else {
				--size;
			}

			break;
		}

		++mode;

		sched1_matrix_destroy(&workload->matrix);
	} while(hits < 3);

	sched1_matrix_init(&workload->matrix, best_size);
}

MODEXPORT int sched1_wl_step(workload_t* wl, unsigned num_requests) {
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	workload->num_iterations = num_requests;
	event_notify_all(&workload->notifier);

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

	return MOD_OK;
}

thread_result_t sched1_ping_thread(thread_arg_t arg) {
	THREAD_ENTRY(arg, workload_t, wl);

	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	int* msg = NULL;
	int iter = 0;
	volatile int cycle = 0;

	logmsg(LOG_INFO, "Ping thread id: %lu", thread->t_system_id);

	while(atomic_read(&workload->done) != B_TRUE) {
		event_wait(&workload->notifier);

		if(atomic_read(&workload->done) == B_TRUE)
			break;

		for(iter = 0; iter < workload->num_iterations; ++iter) {
			/* Simulate busy working */
			ETRC_PROBE0(mbench__sched1, ping__request__start);
			sched1_matrix_mul(&workload->matrix);
			ETRC_PROBE0(mbench__sched1, ping__request__finish);

			/* Awake pong thread for nothing */
			msg = mp_malloc(sizeof(int));
			*msg = iter;

			ETRC_PROBE0(mbench__sched1, ping__send__pong);
			squeue_push(&workload->sq, (void*) msg);
		}
	}

	/* Finish pong thread */
	squeue_push(&workload->sq, NULL);

	THREAD_END:
		THREAD_FINISH(arg);
}

thread_result_t sched1_pong_thread(thread_arg_t arg) {
	THREAD_ENTRY(arg, workload_t, wl);

	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	int* msg = NULL;

	logmsg(LOG_INFO, "Pong thread id: %lu", thread->t_system_id);

	while(B_TRUE) {
		msg = squeue_pop(&workload->sq);

		ETRC_PROBE0(mbench__sched1, pong__on__cpu);

		if(msg == NULL) {
			THREAD_EXIT(0);
		}

		mp_free(msg);
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
