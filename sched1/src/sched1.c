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

#include <sched1.h>

#include <stdlib.h>
#include <string.h>

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
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_cycles",
		"Number of cycles to be waited (not CPU cycles)",
		offsetof(struct sched1_params, num_cycles) },
	{ WLP_NULL }
};

module_t* self = NULL;

thread_result_t sched1_ping_thread(thread_arg_t arg);
thread_result_t sched1_pong_thread(thread_arg_t arg);

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
	workload->done = B_FALSE;

	wl->wl_private = workload;

	event_init(&workload->notifier, "start_notifier");

	t_init(&workload->ping, (void*) wl, sched1_ping_thread,
			"ping-%d", strand->id);
	t_init(&workload->pong, (void*) wl, sched1_pong_thread,
			"pong-%d", strand->id);

	sched_set_affinity(&workload->ping, mask);
	sched_set_affinity(&workload->pong, mask);

	cpumask_destroy(mask);

	return 0;
}

MODEXPORT int sched1_wl_unconfig(workload_t* wl) {
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	if(workload) {
		atomic_set(&workload->done, B_TRUE);

		t_destroy(&workload->ping);
		t_destroy(&workload->pong);

		event_destroy(&workload->notifier);

		squeue_destroy(&workload->sq, mp_free);
	}

	return 0;
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

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	return MOD_OK;
}

thread_result_t sched1_ping_thread(thread_arg_t arg) {
	THREAD_ENTRY(arg, workload_t, wl);

	struct sched1_params* sched1 = (struct sched1_params*) wl->wl_params;
	struct sched1_workload* workload = (struct sched1_workload*) wl->wl_private;

	int* msg = NULL;
	int iter = 0;
	volatile int cycle = 0;

	logmsg(LOG_INFO, "Ping thread id: %lu\n", thread->t_system_id);

	while(atomic_read(&workload->done) != B_TRUE) {
		event_wait(&workload->notifier);

		for(iter = 0; iter < workload->num_iterations; ++iter) {
			/* Simulate busy working */
			for(cycle = 0; cycle < sched1->num_cycles; ++cycle);

			/* Awake pong thread for nothing */
			msg = mp_malloc(sizeof(int));
			*msg = iter;

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

	logmsg(LOG_INFO, "Pong thread id: %lu\n", thread->t_system_id);

	while(B_TRUE) {
		msg = squeue_pop(&workload->sq);

		if(msg == NULL) {
			THREAD_EXIT(0);
		}

		mp_free(msg);
	}

	THREAD_END:
		THREAD_FINISH(arg);
}
