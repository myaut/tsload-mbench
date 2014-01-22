/*
 * http.c
 *
 *  Created on: Dec 23, 2013
 *      Author: myaut
 */

#define LOG_SOURCE "http"
#include <log.h>

#include <mempool.h>
#include <defs.h>
#include <workload.h>
#include <wltype.h>
#include <modules.h>
#include <modapi.h>
#include <threads.h>

#include <http.h>

#include <curl/curl.h>

#include <stdlib.h>
#include <string.h>

DECLARE_MODAPI_VERSION(MOD_API_VERSION);
DECLARE_MOD_NAME("http");
DECLARE_MOD_TYPE(MOD_TSLOAD);

MODEXPORT wlp_descr_t http_params[] = {
	{ WLP_RAW_STRING, WLPF_NO_FLAGS,
		WLP_STRING_LENGTH(MAXHOSTNAMELEN),
		WLP_STRING_DEFAULT("localhost"),
		"server",
		"HTTP server hostname",
		offsetof(struct http_workload, server)
	},
	{ WLP_INTEGER, WLPF_NO_FLAGS,
		WLP_NO_RANGE(),
		WLP_INT_DEFAULT(80),
		"port",
		"HTTP server port",
		offsetof(struct http_workload, port) },
	{ WLP_RAW_STRING, WLPF_NO_FLAGS,
		WLP_STRING_LENGTH(MAXURLLEN),
		WLP_NO_DEFAULT(),
		"url",
		"HTTP request url",
		offsetof(struct http_workload, url)
	},
	{ WLP_NULL }
};

module_t* self = NULL;
thread_mutex_t resolver_mutex;

MODEXPORT int http_wl_config(workload_t* wl) {
	struct http_workload* hwp = (struct http_workload*) wl->wl_params;
	char* url = mp_malloc(MAXURLLEN);
	snprintf(url, MAXURLLEN, "http://%s:%d%s", hwp->server, hwp->port, hwp->url);

	wl->wl_private = url;

	return 0;
}

MODEXPORT int http_wl_unconfig(workload_t* wl) {
	mp_free(wl->wl_private);

	return 0;
}

MODEXPORT int http_run_request(request_t* rq) {
  CURL *curl;
  CURLcode res;

  const char* url = rq->rq_workload->wl_private;

  curl = curl_easy_init();
  if(curl) {
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1);

	res = curl_easy_perform(curl);

	if(res != CURLE_OK)
	  logmsg(LOG_WARN, "curl_easy_perform() failed: %s\n",
			  	  	    curl_easy_strerror(res));

	curl_easy_cleanup(curl);

	return 0;
  }

  return 1;
}

wl_type_t http_wlt = {
	"http",							/* wlt_name */

	WLC_NET_CLIENT,					/* wlt_class */

	http_params,					/* wlt_params */
	sizeof(struct http_workload),	/* wlt_params_size*/

	http_wl_config,					/* wlt_wl_config */
	http_wl_unconfig,				/* wlt_wl_unconfig */

	NULL,							/* wlt_wl_step */

	http_run_request				/* wlt_run_request */
};

MODEXPORT int mod_config(module_t* mod) {
	self = mod;

	wl_type_register(mod, &http_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &http_wlt);

	return MOD_OK;
}
