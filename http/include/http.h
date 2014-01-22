/*
 * http.h
 *
 *  Created on: Dec 23, 2013
 *      Author: myaut
 */

#ifndef MOD_LOAD_HTTP_
#define MOD_LOAD_HTTP_

#include <netsock.h>
#include <wlparam.h>

#define MAXHOSTNAMELEN	256
#define MAXURLLEN		2048

#define RESPONSE_BUF_SIZE	4096
#define USER_AGENT			"User-Agent: TSLoad HTTP Module"

struct http_workload {
	wlp_string_t 	server[MAXHOSTNAMELEN];
	wlp_integer_t 	port;
	wlp_string_t 	url[MAXURLLEN];

	int url_len;
	int server_len;
};


#endif
