/*
 * http.h
 *
 *  Created on: Dec 23, 2013
 *      Author: myaut
 */

#ifndef MOD_LOAD_HTTP_
#define MOD_LOAD_HTTP_

/* FIXME: Should move socket API to tscommon */
#include <plat/client.h>

plat_host_entry* plat_clnt_resolve(const char* host);
int plat_clnt_setaddr(plat_clnt_addr* clnt_sa, plat_host_entry* he, int clnt_port);

int plat_clnt_connect(plat_clnt_socket* clnt_socket, plat_clnt_addr* clnt_sa);
int plat_clnt_disconnect(plat_clnt_socket* clnt_socket);

int plat_clnt_poll(plat_clnt_socket* clnt_socket, ts_time_t timeout);

int plat_clnt_send(plat_clnt_socket* clnt_socket, void* data, size_t len);
int plat_clnt_recv(plat_clnt_socket* clnt_socket, void* data, size_t len);

#define CLNT_OK				0
#define CLNT_ERR_RESOLVE	-1
#define CLNT_ERR_SOCKET		-2
#define CLNT_ERR_CONNECT	-3

#define CLNT_POLL_OK			0
#define CLNT_POLL_NEW_DATA		1
#define CLNT_POLL_DISCONNECT 	2
#define CLNT_POLL_FAILURE		3

#include <wlparam.h>

#define MAXHOSTNAMELEN	256
#define MAXURLLEN		2048

#define RESPONSE_BUF_SIZE	4096
#define USER_AGENT			"User-Agent: TSLoad HTTP Module"

struct http_workload {
	wlp_string_t 	server[MAXHOSTNAMELEN];
	wlp_integer_t 	port;
	wlp_string_t 	url[MAXURLLEN];

	plat_clnt_addr		clnt_addr;
	int url_len;

	long logged_step;
	int logged_code;
};


#endif
