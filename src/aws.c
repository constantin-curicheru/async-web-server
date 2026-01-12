// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	return RESOURCE_TYPE_NONE;
}


struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
    struct connection *conn = calloc(1, sizeof(struct connection));

    conn->sockfd = sockfd;
    conn->fd = -1;
    conn->state = STATE_INITIAL;

    conn->ctx = ctx;

    conn->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
}

void connection_remove(struct connection *conn)
{   
	/* TODO: Remove connection handler. */
}

void handle_new_connection(void)
{
	/* Handle a new connection request on the server socket. */
    int clientfd;
    struct sockaddr_in client_addr;
    socklen_t client_length = sizeof(client_addr);
	/* Accept new connection. */
    clientfd = accept(listenfd, (struct sockaddr *)&client_addr, &client_length);
	DIE(clientfd < 0, "accept");

	/* Set socket to be non-blocking. */
    int rc;
	int flags;
    flags = fcntl(clientfd, F_GETFL, 0);
	DIE(flags < 0, "fcntl F_GETFL");
	rc = fcntl(clientfd, F_SETFL, flags | O_NONBLOCK);
	DIE(rc < 0, "fcntl F_SETFL");

	/* Instantiate new connection handler. */
    struct connection *conn = connection_create(clientfd);

	/* Add socket to epoll. */
    rc = w_epoll_add_ptr_in(epollfd, clientfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");

	/* Initialize HTTP_REQUEST parser. */
    http_parser_init(&conn->request_parser, HTTP_REQUEST);
    conn->request_parser.data = conn;
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */

	return -1;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	return STATE_NO_STATE;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	return -1;
}


int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */
	return 0;
}


void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */

	switch (conn->state) {
	default:
		printf("shouldn't get here %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */

	switch (conn->state) {
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
    if (event & (EPOLLERR | EPOLLHUP)) {
        connection_remove(conn);
        return;
    }

    if (event & EPOLLIN) {
        handle_input(conn);
    }

    if (event & EPOLLOUT) {
        handle_output(conn);
    }
}

int main(void)
{
	int rc;

	/* Initialize asynchronous operations. */
    ctx = 0;
	rc = io_setup(128, &ctx);
	DIE(rc < 0, "io_setup");

	/* Initialize multiplexing. */
    epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* Create server socket. */
    listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

    // making it non-blocking
    int flags = fcntl(listenfd, F_GETFL, 0);
    fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);

	/* Add server socket to epoll object*/
    rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* Uncomment the following line for debugging. */
	// dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* TODO: Wait for events. */
        rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
        if (rev.data.fd == listenfd) {
            // listenfd reciver client
            handle_new_connection();
        } else {
            // client sent request
            handle_client(rev.events, (struct connection *)rev.data.ptr);
        }
	}

	return 0;
}
