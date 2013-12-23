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
	struct http_workload* hww =
				(struct http_workload*) wl->wl_params;
	plat_host_entry* he = NULL;

	mutex_lock(&resolver_mutex);
	he = plat_clnt_resolve(hww->server);

	if(plat_clnt_setaddr(&hww->clnt_addr, he, hww->port) != CLNT_OK) {
		mutex_unlock(&resolver_mutex);
		return 1;
	}

	mutex_unlock(&resolver_mutex);

	hww->url_len = strlen(hww->url);
	hww->logged_code = 200;
	hww->logged_step = -1;

	return 0;
}

MODEXPORT int http_wl_unconfig(workload_t* wl) {
	return 0;
}

MODEXPORT int http_run_request(request_t* rq) {
	struct http_workload* hww =
			(struct http_workload*) rq->rq_workload->wl_params;

	char* response_buf;
	char* request;
	size_t request_len;

	int ret = 0;
	int status = 0;
	int recv_len = 0;
	int bufnum = 0;

	int http_minor_ver;
	int http_code;

	boolean_t receiving = B_FALSE, connected = B_TRUE;

	plat_clnt_socket socket;

	/* Connect */
	if(plat_clnt_connect(&socket, &hww->clnt_addr) != CLNT_OK) {
		return 1;
	}

	/* Send HTTP request */
	request_len = hww->url_len + 48;
	request = malloc(request_len);

	snprintf(request, request_len, "GET %s HTTP/1.1\n"
			"User-Agent: TSLoad HTTP Module\n\n", hww->url);

	plat_clnt_send(&socket, request, request_len);

	free(request);

	/* Receive response */
	response_buf = malloc(RESPONSE_BUF_SIZE);

	while(connected) {
		if(receiving) {
			recv_len = plat_clnt_recv(&socket, response_buf, RESPONSE_BUF_SIZE);
			receiving = B_FALSE;

			if(recv_len == 0) {
				connected = B_FALSE;
				break;
			}

			/* Read status code if first buffer to get */
			if(bufnum++ == 0) {
				char* eol = NULL;
				sscanf(response_buf, "HTTP/1.%d %d", &http_minor_ver, &http_code);

				eol = strchr(response_buf, '\n');
				if(eol != NULL) {
					*eol = '\0';
				}

				/* Log error, but not frequently than 1 code per step.
				 * But if codes are random, that still overflows log */
				if(http_code != 200 &&
				   ((rq->rq_step > hww->logged_step) ||
					 hww->logged_code != http_code)) {
						logmsg(LOG_ERROR, "got response: %s", response_buf);

						hww->logged_code = http_code;
						hww->logged_step = rq->rq_step;
				}
			}
		}

		status = plat_clnt_poll(&socket, 0);

		switch(status) {
		case CLNT_POLL_FAILURE:
			ret = 1;
			/* FALLTHROUGH */
		case CLNT_POLL_DISCONNECT:
			connected = B_FALSE;
			break;
		case CLNT_POLL_NEW_DATA:
			receiving = B_TRUE;
			break;
		}
	}

	free(response_buf);

	/* Release resources */
	plat_clnt_disconnect(&socket);

	return ret;
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

	mutex_init(&resolver_mutex, "resolver_mutex");
	wl_type_register(mod, &http_wlt);

	return MOD_OK;
}

MODEXPORT int mod_unconfig(module_t* mod) {
	wl_type_unregister(mod, &http_wlt);
	mutex_destroy(&resolver_mutex);

	return MOD_OK;
}
