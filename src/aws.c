// SPDX-License-Identifier: BSD-3-Clause

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libaio.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "aws.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/util.h"
#include "utils/w_epoll.h"

static int listenfd; /* server socket file descriptor */

static int epollfd; /* epoll file descriptor */

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
	// HTTP responses will have the code 200 for existing files

	conn->send_len = snprintf(conn->send_buffer, BUFSIZ,
							  "HTTP/1.1 200 OK\r\n"
							  "Content-Length: %ld\r\n"
							  "Connection: close\r\n"
							  "\r\n",
							  conn->file_size);
	dlog(LOG_INFO, "Header to send: %s\n", conn->send_buffer);
	conn->state = STATE_SENDING_HEADER;
}

static void connection_prepare_send_404(struct connection *conn)
{
	// HTTP responses will have the code 404 for not existing files.
	conn->send_len = snprintf(conn->send_buffer, BUFSIZ,
							  "HTTP/1.1 404 Not Found\r\n"
							  "\r\n");
	dlog(LOG_INFO, "Header to send: %s\n", conn->send_buffer);
	conn->state = STATE_SENDING_404;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	if (strstr(conn->request_path, AWS_REL_STATIC_FOLDER) != NULL)
		return RESOURCE_TYPE_STATIC;
	else if (strstr(conn->request_path, AWS_REL_DYNAMIC_FOLDER) != NULL)
		return RESOURCE_TYPE_DYNAMIC;
	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	struct connection *con =
		(struct connection *)calloc(1, sizeof(struct connection));

	DIE(con == NULL, "malloc error");
	con->sockfd = sockfd;
	con->ctx = ctx;
	memset(con->recv_buffer, 0, BUFSIZ);
	memset(con->send_buffer, 0, BUFSIZ);

	return con;
}

void connection_start_async_io(struct connection *conn)
{
	conn->piocb[0] = &conn->iocb;
	io_prep_pread(conn->piocb[0], conn->fd, conn->send_buffer, BUFSIZ,
				  conn->file_pos);
	io_submit(ctx, 1, conn->piocb);
}

void connection_remove(struct connection *conn)
{
	conn->state = STATE_CONNECTION_CLOSED;
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	close(conn->sockfd);
	close(conn->fd);
	free(conn);
}

void handle_new_connection(void)
{
	socklen_t _addr_len = sizeof(struct sockaddr);
	struct sockaddr _addr;
	int sfd = accept(listenfd, &_addr, &_addr_len);

	int getfl = fcntl(sfd, F_GETFL) | O_NONBLOCK;

	fcntl(sfd, F_SETFL, getfl);
	struct connection *con = connection_create(sfd);

	w_epoll_add_ptr_in(epollfd, sfd, con);
	http_parser_init(&con->request_parser, HTTP_REQUEST);
	con->request_parser.data = con;
}

void print_resource_type(enum resource_type res_type)
{
	switch (res_type) {
	case RESOURCE_TYPE_STATIC:
		dlog(LOG_INFO, "Resource type: STATIC\n");
		break;
	case RESOURCE_TYPE_DYNAMIC:
		dlog(LOG_INFO, "Resource type: DYNAMIC\n");
		break;
	default:
		dlog(LOG_INFO, "Resource type: NONE\n");
		break;
	}
}

void receive_data(struct connection *conn)
{
	ssize_t readb =
		recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);
	DIE(readb < 0, "recv error");
	dlog(LOG_INFO, "recv bytes: %ld\n", readb);

	conn->recv_len += readb;
	if (strstr(conn->recv_buffer, "\r\n\r\n") == NULL) {
		conn->state = STATE_RECEIVING_DATA;
	} else {
		dlog(LOG_INFO, "Received: %s\n", conn->recv_buffer);
		conn->state = STATE_REQUEST_RECEIVED;
		// scap de con din input
		w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		// adaug con in output
		w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);
		// parsing header
		if (parse_header(conn) < 0) {
			dlog(LOG_INFO, "parse header error\n");
		} else {
			dlog(LOG_INFO, "parse header success\n");
			dlog(LOG_INFO, "request path: %s\n", conn->request_path);
		}
		// resource type
		conn->res_type = connection_get_resource_type(conn);
		print_resource_type(conn->res_type);
	}
}

int connection_open_file(struct connection *conn)
{
	char path[BUFSIZ];

	strcpy(path, AWS_DOCUMENT_ROOT);
	path[strlen(path) - 1] = '\0';
	strcat(path, conn->request_path);
	dlog(LOG_INFO, "path: %s\n", path);

	conn->fd = open(path, O_RDONLY);
	if (conn->fd > 0) {
		dlog(LOG_INFO, "open file success\n");
	} else {
		// probabil nu s-a gasit fisierul
		dlog(LOG_INFO, "open file error\n");
		return -1;
	}
	struct stat *st = (struct stat *)malloc(sizeof(struct stat));

	DIE(st == NULL, "malloc error");

	fstat(conn->fd, st);
	conn->file_size = st->st_size;
	dlog(LOG_INFO, "Size of file to send: %ld\n", conn->file_size);

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	struct io_event *ev = (struct io_event *)malloc(sizeof(struct io_event));

	DIE(ev == NULL, "malloc error");
	io_getevents(conn->ctx, 1, 1, ev, NULL);

	conn->file_pos += ev->res;
	conn->send_len = ev->res;
	if (conn->send_len) {
		conn->send_pos = 0;
		conn->state = STATE_ASYNC_ONGOING;
	} else {
		conn->state = STATE_DATA_SENT;
	}
}

int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = { .on_message_begin = 0,
											  .on_header_field = 0,
											  .on_header_value = 0,
											  .on_path = aws_on_path_cb,
											  .on_url = 0,
											  .on_fragment = 0,
											  .on_query_string = 0,
											  .on_body = 0,
											  .on_headers_complete = 0,
											  .on_message_complete = 0 };
	return http_parser_execute(&conn->request_parser, &settings_on_path,
							   conn->recv_buffer, conn->recv_len);
}

enum connection_state connection_send_static(struct connection *conn)
{
	ssize_t bsent = sendfile(conn->sockfd, conn->fd, (off_t *)&conn->file_pos,
							 conn->file_size - conn->file_pos);

	if (bsent < 0)
		dlog(LOG_INFO, "sendfile error\n");

	dlog(LOG_INFO, "sent file bytes: %ld\n", bsent);
	if (conn->file_pos == conn->file_size)
		conn->state = STATE_DATA_SENT;
	else
		conn->state = STATE_SENDING_DATA;

	return conn->state;
}

int connection_send_data(struct connection *conn)
{
	ssize_t bsent = send(conn->sockfd, conn->send_buffer + conn->send_pos,
						 conn->send_len - conn->send_pos, 0);

	if (bsent < 0)
		return -1;
	conn->send_pos += bsent;
	if (conn->send_pos == conn->send_len) {
		switch (conn->state) {
		case STATE_SENDING_404:
			conn->state = STATE_404_SENT;
			break;
		case STATE_SENDING_HEADER:
			conn->state = STATE_HEADER_SENT;
			break;
		case STATE_ASYNC_ONGOING:
			conn->state = STATE_SENDING_DATA;
			break;
		default:
			dlog(LOG_INFO, "we got ourselves in a weird state\n");
			break;
		}
	}
	dlog(LOG_INFO, "sent bytes: %ld\n", bsent);
	return bsent;
}

int connection_send_dynamic(struct connection *conn)
{
	connection_start_async_io(conn);
	connection_complete_async_io(conn);
	return 0;
}

void handle_input(struct connection *conn)
{
	DIE(conn == NULL, "conn error");

	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	// dlog(LOG_INFO, "input state: %d\n", conn->state);
	switch (conn->state) {
	case STATE_INITIAL:
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		break;
	default:
		// printf("input default case %d\n", conn->state);
		break;
	}
}

void connection_send_resource(struct connection *conn)
{
	switch (conn->res_type) {
		{
		case RESOURCE_TYPE_STATIC:
			connection_send_static(conn);
			break;
		case RESOURCE_TYPE_DYNAMIC:
			connection_send_dynamic(conn);
			break;
		default:
			dlog(LOG_INFO, "unknown resource\n");
			break;
		}
	}
}

int can_send_resource(struct connection *conn)
{
	if (conn->res_type == RESOURCE_TYPE_NONE)
		return 0;
	return 1;
}

// check if the resource is valid
// return 0 if it is, -1 otherwise
int check_valid_resource(struct connection *conn)
{
	if (can_send_resource(conn) == 0) {
		dlog(LOG_INFO, "404\n");
		return -1;
	}
	return 0;
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or
	 * notification of completion of an asynchronous I/O operation or invalid
	 * requests.
	 */
	DIE(conn == NULL, "conn error");
	// dlog(LOG_INFO, "output state that came in: %d\n", conn->state);
	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		if (check_valid_resource(conn) < 0 || connection_open_file(conn) < 0)
			connection_prepare_send_404(conn);
		else
			connection_prepare_send_reply_header(conn);
		break;
	case STATE_SENDING_404:
	case STATE_SENDING_HEADER:
	case STATE_ASYNC_ONGOING:
		connection_send_data(conn);
		break;
	case STATE_HEADER_SENT:
	case STATE_SENDING_DATA:
		connection_send_resource(conn);
		break;
	case STATE_404_SENT:
	case STATE_DATA_SENT:
	case STATE_CONNECTION_CLOSED:
		connection_remove(conn);
		break;

	default:
		// ERR("Unexpected state\n");
		// dlog(LOG_INFO, "default case: %d\n", conn->state);
		// exit(1);
		break;
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
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
	rc = io_setup(256, &ctx);
	DIE(rc < 0, "io_setup error");

	/* Initialize multiplexing. */
	epollfd = w_epoll_create();

	/* Create server socket. */

	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);

	/* Add server socket to epoll object*/

	w_epoll_add_fd_in(epollfd, listenfd);

	/* Uncomment the following line for debugging. */
	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		 AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* Wait for events. */
		rc = w_epoll_wait_infinite(epollfd, &rev);

		DIE(rc < 0, "epoll wait error");

		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN) {
				dlog(LOG_INFO, "New connection\n");
				handle_new_connection();
			}
		} else {
			handle_client(rev.events, (struct connection *)rev.data.ptr);
		}
	}

	return 0;
}
