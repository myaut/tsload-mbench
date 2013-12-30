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

#define MSGLEN		128

typedef struct {
	int http_minor_version;
	int http_code;

	int header_length;
	int content_length;

	char message[MSGLEN];
} http_header_t;

static int parse_http_header(char* response_buf, http_header_t* header);

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

	mutex_lock(&resolver_mutex);
	if(nsk_resolve(hww->server, &hww->he) != NSK_OK ||
	   nsk_setaddr(&hww->clnt_addr, &hww->he, hww->port) != NSK_OK) {
		mutex_unlock(&resolver_mutex);
		return 1;
	}

	mutex_unlock(&resolver_mutex);

	hww->url_len = strlen(hww->url);
	hww->server_len = strlen(hww->server);

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
	int recv_len = 0, recv_ret = -1;
	int bufnum = 0;

	boolean_t receiving = B_FALSE, connected = B_TRUE;

	nsk_socket socket;

	http_header_t header;

	/* Connect */
	if(nsk_connect(&socket, &hww->clnt_addr, NSK_STREAM) != NSK_OK) {
		return 1;
	}

	nsk_setnb(&socket, B_TRUE);

	/* Send HTTP request */
	request_len = hww->url_len + hww->server_len + 55;
	request = malloc(request_len);

	snprintf(request, request_len,
			"GET %s HTTP/1.1\n"
			"Host: %s\n"
			"User-Agent: TSLoad HTTP Module\n\n",
				hww->url, hww->server);

	nsk_send(&socket, request, request_len);

	free(request);

	/* Receive response */
	response_buf = malloc(RESPONSE_BUF_SIZE);

	while(connected) {
		if(receiving) {
			recv_ret = nsk_recv(&socket, response_buf, RESPONSE_BUF_SIZE);

			if(recv_ret < 0)
				break;

			recv_len -= recv_ret;
			receiving = B_FALSE;

			/* Read status code if first buffer to get */
			if(bufnum++ == 0) {
				if(parse_http_header(response_buf, &header) == 1) {
					connected = B_FALSE;
					break;
				}

				recv_len += header.header_length + header.content_length;

				/* Log error, but not frequently than 1 code per step.
				 * But if codes are random, that still overflows log */
				if(header.http_code != 200 &&
				   ((rq->rq_step > hww->logged_step) ||
					 hww->logged_code != header.http_code)) {
						logmsg(LOG_ERROR, "got response: %s", response_buf);

						hww->logged_code = header.http_code;
						hww->logged_step = rq->rq_step;
				}
			}

			/* Received all, disconnect */
			if(recv_len <= 0) {
				break;
			}
		}

		status = nsk_poll(&socket, 10 * T_MS);

		switch(status) {
		case NSK_POLL_FAILURE:
			ret = 1;
			/* FALLTHROUGH */
		case NSK_POLL_DISCONNECT:
			connected = B_FALSE;
			break;
		case NSK_POLL_NEW_DATA:
			receiving = B_TRUE;
			break;
		}
	}

	free(response_buf);

	/* Release resources */
	nsk_disconnect(&socket);

	return ret;
}

static int parse_http_header(char* response_buf, http_header_t* header) {
	char* eol = NULL;
	char* eoh = response_buf;
	char* p = NULL;
	size_t pos = 0;
	size_t data_len;
	int http_minor_ver;

	int ret = 1;

	sscanf(response_buf, "HTTP/1.%d %d %128s\n",
			&header->http_minor_version,
			&header->http_code,
			header->message);

	while(pos < (RESPONSE_BUF_SIZE - 4)) {
		if(eol == NULL && eoh[0] == '\n')
			eol = eoh;

		if(eoh[0] == 'C')
			sscanf(eoh, "Content-Length: %d", &header->content_length);

		if(strncmp(eoh, "\r\n\r\n", 4) == 0) {
			header->header_length = eoh - response_buf + 4;

			ret = 0;
			break;
		}

		++eoh;
		++pos;
	}

	*eol = '\0';

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
