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
	/* Prepare the connection buffer to send the reply header. */
	conn->send_len = snprintf(conn->send_buffer, BUFSIZ,
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"\r\n",
		conn->file_size);
	conn->send_pos = 0;
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* Prepare the connection buffer to send the 404 header. */
	const char *not_found_response =
		"HTTP/1.1 404 Not Found\r\n"
		"Content-Length: 0\r\n"
		"Connection: close\r\n"
		"\r\n";
	conn->send_len = strlen(not_found_response);
	memcpy(conn->send_buffer, not_found_response, conn->send_len);
	conn->send_pos = 0;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	if (strstr(conn->request_path, AWS_ABS_STATIC_FOLDER))
		return RESOURCE_TYPE_STATIC;
	if (strstr(conn->request_path, AWS_ABS_DYNAMIC_FOLDER))
		return RESOURCE_TYPE_DYNAMIC;

	return RESOURCE_TYPE_NONE;
}


struct connection *connection_create(int sockfd)
{
	/* Initialize connection structure on given socket. */
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
	/* Remove connection handler. */
	if (conn->fd >= 0)
		close(conn->fd);

	if (conn->sockfd >= 0) {
		w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		tcp_close_connection(conn->sockfd);
	}

	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
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
	/* Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	char buff[32];
	int rc;

	rc = get_peer_address(conn->sockfd, buff, 32);
	// if error getting peer address
	if (rc < 0) {
		ERR("get_peer_address");
		connection_remove(conn);
		return;
	}

	ssize_t rc_recv = recv(conn->sockfd,
				   conn->recv_buffer + conn->recv_len,
				   BUFSIZ - conn->recv_len, 0);

	if (rc_recv < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		connection_remove(conn);
		return;
	}

	if (rc_recv == 0) {
		connection_remove(conn);
		return;
	}

	conn->recv_len += rc_recv;

	if (strstr(conn->recv_buffer, "\r\n\r\n"))
		conn->state = STATE_REQUEST_RECEIVED;
	else
		conn->state = STATE_RECEIVING_DATA;
}

int connection_open_file(struct connection *conn)
{
	/* Open file and update connection fields. */
	conn->res_type = connection_get_resource_type(conn);
	dlog(LOG_INFO, "in %s conn->req path: %s\n", __func__, conn->request_path);

	if (conn->res_type == RESOURCE_TYPE_NONE)
		return -1;

	conn->fd = open(conn->request_path, O_RDONLY);
	if (conn->fd < 0)
		return -1;

	struct stat st;

	if (fstat(conn->fd, &st) < 0) {
		close(conn->fd);
		conn->fd = -1;
		return -1;
	}
	conn->file_size = st.st_size;
	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
}

int parse_header(struct connection *conn)
{
	/* Parse the HTTP header and extract the file path. */
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

	size_t bytes_parsed;

	bytes_parsed = http_parser_execute(&conn->request_parser,
									   &settings_on_path,
									   conn->recv_buffer,
									   conn->recv_len);
	dlog(LOG_INFO, "in %s bytes_parsed=%zu, recv_len=%zu, path=%s\n",
		 __func__, bytes_parsed, conn->recv_len, conn->request_path);
	if (bytes_parsed == conn->recv_len) {
		// add a '.' at the beginning of the path
		char temp[BUFSIZ];

		strcpy(temp, conn->request_path);
		conn->request_path[0] = '.';
		strcpy(conn->request_path + 1, temp);
		return 0;
	}

	return -1;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* Send static data using sendfile(2). */
	ssize_t rc_sendfile = sendfile(conn->sockfd,
								conn->fd,
								&conn->file_pos,
								conn->file_size - conn->file_pos);
	if (rc_sendfile < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return STATE_SENDING_DATA;
		connection_remove(conn);
		return STATE_CONNECTION_CLOSED;
	}

	if (conn->file_pos >= conn->file_size)
		return STATE_DATA_SENT;

	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	ssize_t rc_send = send(conn->sockfd,
						conn->send_buffer + conn->send_pos,
						conn->send_len - conn->send_pos,
						0);
	if (rc_send < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		connection_remove(conn);
		return -1;
	}

	conn->send_pos += rc_send;
	return rc_send;
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
	/* Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	int rc;

	switch (conn->state) {
	case STATE_INITIAL:
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		if (conn->state == STATE_CONNECTION_CLOSED)
			return;
		if (conn->state == STATE_REQUEST_RECEIVED)
			handle_input(conn);
		break;
	case STATE_REQUEST_RECEIVED:
		rc = parse_header(conn);
		dlog(LOG_INFO, "in %s parsed header rc=%d\n", __func__, rc);
		if (rc < 0)
			return;

		rc = connection_open_file(conn);
		// if file was not found
		if (rc < 0) {
			connection_prepare_send_404(conn);
			conn->state = STATE_SENDING_404;
			w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			return;
		}
		// file opened successfully
		connection_prepare_send_reply_header(conn);
		conn->state = STATE_SENDING_HEADER;
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		break;

	default:
		printf("shouldn't get here %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	/* Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */
	dlog(LOG_INFO, "in %s state=%d\n", __func__, conn->state);
	switch (conn->state) {
	case STATE_SENDING_HEADER:
		{
			int rc = connection_send_data(conn);

			if (rc < 0)
				return;

			if (conn->send_pos < conn->send_len)
				// not all data sent yet
				return;

			// header fully sent
			conn->state = STATE_SENDING_DATA;
		}
		break;
	case STATE_SENDING_404:
		{
			int rc = connection_send_data(conn);

			if (rc < 0)
				return;

			if (conn->send_pos < conn->send_len)
				// not all data sent yet
				return;

			// 404 fully sent
			connection_remove(conn);
		}
		break;
	case STATE_SENDING_DATA:
		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:
			{
				enum connection_state state = connection_send_static(conn);

				if (state == STATE_DATA_SENT || state == STATE_CONNECTION_CLOSED)
					connection_remove(conn);
			}
			break;
		case RESOURCE_TYPE_DYNAMIC:
			{
				int rc = connection_send_dynamic(conn);

				if (rc == 0)
					connection_remove(conn);
				else if (rc < 0)
					connection_remove(conn);
			}
			break;
		default:
			ERR("Unexpected resource type\n");
			exit(1);
		}
		break;
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

	if (event & EPOLLIN)
		handle_input(conn);

	if (event & EPOLLOUT)
		handle_output(conn);
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

		/* Wait for events. */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* Switch event types; consider
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
