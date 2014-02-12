/*
 * llc.c
 *
 *  Created on: 05.01.2014
 *      Author: myaut
 */

#define LOG_SOURCE "llc"
#include <log.h>

#include <mempool.h>
#include <defs.h>
#include <workload.h>
#include <wltype.h>
#include <modules.h>
#include <modapi.h>
#include <cpuinfo.h>

#include <llc.h>

#include <sys/mman.h>

#include <stdlib.h>
#include <string.h>

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("llc");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT wlp_descr_t llc_params[] = {
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_wait_cycles",
		"Number of cycles between memory accesses",
		offsetof(struct llc_workload, num_cycles) },
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"num_accesses",
		"Number of memory accesses in request",
		offsetof(struct llc_workload, num_accesses) },
	{ WLP_CPU_OBJECT, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_NO_DEFAULT(),
		"cpu_object",
		"Root CPU object where cache is located",
		offsetof(struct llc_workload, cpu_object) },
	{ WLP_NULL }
};

module_t* self = NULL;

static hi_cpu_object_t* get_last_level_cache(hi_cpu_object_t* parent, int* pcount, int* plevel);

MODEXPORT int llc_wl_config(workload_t* wl) {
	struct llc_workload* llcw =
				(struct llc_workload*) wl->wl_params;
	struct llc_data* data;

	size_t ll_cache_size;

	int count = 0;
	int level = 0;

	hi_cpu_object_t* cache = get_last_level_cache(llcw->cpu_object, &count, &level);

	if(cache == NULL) {
		wl_notify(wl, WLS_CFG_FAIL, -1, "Failed to find last level cache for '%s'",
				  HI_CPU_TO_OBJ(llcw->cpu_object)->name);
		return 1;
	}

	ll_cache_size = cache->cache.c_size * count;

	data = mp_malloc(sizeof(struct llc_data));
	data->size = ll_cache_size / wl->wl_tp->tp_num_threads +
					cache->cache.c_associativity * cache->cache.c_line_size;
	data->line_size = cache->cache.c_line_size;

	logmsg(LOG_INFO, "wl: %s cache size per worker: %lu",
			wl->wl_name, (unsigned long) data->size);

	wl->wl_private = (void*) data;

	return 0;
}

MODEXPORT int llc_wl_unconfig(workload_t* wl) {
	struct llc_data* data =
		(struct llc_data*) wl->wl_private;

	mp_free(data);

	return 0;
}

MODEXPORT int llc_run_request(request_t* rq) {
	struct llc_workload* llcw =
			(struct llc_workload*) rq->rq_workload->wl_params;
	struct llc_data* data =
		(struct llc_data*) rq->rq_workload->wl_private;

	char* ptr;
	volatile int i, j, k;
	volatile char c;

	ptr = (char*) mmap(NULL, data->size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANON, -1, 0);

	for(i = 0; i < llcw->num_accesses; ++i) {
		for(j = 0; j < data->size; j += data->line_size) {
			if(i == 0)
				ptr[j] = 'x';

			c = ptr[j];

			for(k = 0; k < llcw->num_cycles; ++k) ++c;
		}
	}

	munmap(ptr, data->size);

	return 0;
}

wl_type_t llc_wlt = {
	"llc",							/* wlt_name */

	WLC_CPU_MISC,					/* wlt_class */

	llc_params,						/* wlt_params */
	sizeof(struct llc_workload),	/* wlt_params_size*/

	llc_wl_config,					/* wlt_wl_config */
	llc_wl_unconfig,				/* wlt_wl_unconfig */

	NULL,							/* wlt_wl_step */

	llc_run_request					/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &llc_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &llc_wlt);

	return MOD_OK;
}

/* Return last-level cache object */
static hi_cpu_object_t* get_last_level_cache(hi_cpu_object_t* parent, int* pcount, int* plevel) {
	hi_object_t* parent_obj = HI_CPU_TO_OBJ(parent);
	hi_object_child_t* child;
	hi_cpu_object_t* object;
	hi_cpu_object_t* cache = NULL;

	hi_for_each_child(child, parent_obj) {
		object = HI_CPU_FROM_OBJ(child->object);

		if(object->type != HI_CPU_CACHE) {
			cache = get_last_level_cache(object, pcount, plevel);

			if(cache == NULL)
				continue;
		}

		if(*plevel == object->cache.c_level)
			++*pcount;
		else if(*plevel < object->cache.c_level) {
			*pcount = 1;
			*plevel = object->cache.c_level;

			cache = object;
		}
	}

	return cache;
}

