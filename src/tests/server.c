/*
 * Copyright (c) 2011 UT-Battelle, LLC.  All rights reserved.
 * Copyright (c) 2011 Oak Ridge National Labs.  All rights reserved.
 *
 * See COPYING in top-level directory
 *
 * $COPYRIGHT$
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "cci.h"

int main(int argc, char *argv[])
{
	int ret, done = 0;
	uint32_t caps = 0;
	cci_device_t **devices = NULL;
	cci_endpoint_t *endpoint = NULL;
	cci_os_handle_t ep_fd;
	cci_connection_t *connection = NULL;

	ret = cci_init(CCI_ABI_VERSION, 0, &caps);
	if (ret) {
		fprintf(stderr, "cci_init() failed with %s\n",
			cci_strerror(ret));
		exit(EXIT_FAILURE);
	}

	ret = cci_get_devices((cci_device_t const ***const)&devices);
	if (ret) {
		fprintf(stderr, "cci_get_devices() failed with %s\n",
			cci_strerror(ret));
		exit(EXIT_FAILURE);
	}

	/* create an endpoint */
	ret = cci_create_endpoint(NULL, 0, &endpoint, &ep_fd);
	if (ret) {
		fprintf(stderr, "cci_create_endpoint() failed with %s\n",
			cci_strerror(ret));
		exit(EXIT_FAILURE);
	}
	printf("opened %s\n", endpoint->name);

	while (!done) {
		int accept = 1;
		cci_event_t *event;

		ret = cci_get_event(endpoint, &event);
		if (ret != 0) {
			if (ret != CCI_EAGAIN)
				fprintf(stderr, "cci_get_event() returned %s\n",
					cci_strerror(ret));
			continue;
		}
		switch (event->type) {
		case CCI_EVENT_RECV:{
				char buf[8192];
				char *data = "data:";
				int offset = 0;
				int len = event->recv.len;

				if (len == 3) {
					done = 1;
					continue;
				}

				memset(buf, 0, 8192);
				offset = strlen(data);
				memcpy(buf, data, offset);
				memcpy(buf + offset, event->recv.ptr, len);
				offset += len;
				fprintf(stderr, "recv'd \"%s\"\n", buf);
				ret =
				    cci_send(connection, buf, offset, NULL, 0);
				if (ret)
					fprintf(stderr, "send returned %s\n",
						cci_strerror(ret));
				break;
			}
		case CCI_EVENT_SEND:
			fprintf(stderr, "completed send\n");
			break;
		case CCI_EVENT_CONNECT_REQUEST:
			/* inspect conn_req_t and decide to accept or reject */
			if (accept) {
				/* associate this connect request with this endpoint */
				cci_accept(event, NULL, &connection);

				/* add new connection to connection list, etc. */
			} else {
				cci_reject(event);
			}
			break;
		default:
			printf("event type %d\n", event->type);
			break;
		}
		cci_return_event(event);
	}

	/* clean up */
	cci_destroy_endpoint(endpoint);
	cci_free_devices((cci_device_t const **)devices);

	return 0;
}
