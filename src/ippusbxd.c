#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

#include "logging.h"
#include "http.h"
#include "tcp.h"
#include "usb.h"

struct service_thread_param {
	struct tcp_conn_t *tcp;
	struct usb_sock_t *usb_sock;
	pthread_t thread_handle;
};
static void *service_connection(void *arg_void)
{
	struct service_thread_param *arg = (struct service_thread_param *)arg_void;

	// clasify priority
	while (!arg->tcp->is_closed) {
		struct usb_conn_t *usb = NULL;

		// Client's request
		struct http_message_t *msg = http_message_new();
		if (msg == NULL) {
			ERR("Failed to create message");
			break;
		}

		while (!msg->is_completed) {
			struct http_packet_t *pkt;
			pkt = tcp_packet_get(arg->tcp, msg);
			if (pkt == NULL)
				break;
			if (usb == NULL) {
				usb = usb_conn_aquire(arg->usb_sock, 1);
				if (usb == NULL) {
					ERR("Failed to aquire usb interface");
					packet_free(pkt);
					goto cleanup_subconn;
				}
			}

			NOTE("%.*s", (int)pkt->filled_size, pkt->buffer);
			usb_conn_packet_send(usb, pkt);
			packet_free(pkt);
		}
		message_free(msg);

		// Server's responce
		msg = http_message_new();
		if (msg == NULL) {
			ERR("Failed to create message");
			goto cleanup_subconn;
		}
		while (!msg->is_completed) {
			struct http_packet_t *pkt;
			pkt = usb_conn_packet_get(usb, msg);
			if (pkt == NULL)
				break;

			//NOTE("%.*s", (int)pkt->filled_size, pkt->buffer);
			tcp_packet_send(arg->tcp, pkt);
			packet_free(pkt);
		}

cleanup_subconn:
		if (msg != NULL)
			message_free(msg);
		if (usb != NULL)
			usb_conn_release(usb);
	}



	tcp_conn_close(arg->tcp);
	free(arg);
	return NULL;
}

static void start_daemon(uint32_t requested_port, int debug_mode)
{
	// Capture USB device
	struct usb_sock_t *usb_sock = usb_open();
	if (usb_sock == NULL)
		goto cleanup_usb;

	// Capture a socket
	struct tcp_sock_t *tcp_socket = tcp_open(requested_port);
	if (tcp_socket == NULL)
		goto cleanup_tcp;

	uint32_t real_port = tcp_port_number_get(tcp_socket);
	if (requested_port != 0 && requested_port != real_port) {
		ERR("Received port number did not match requested port number. "
		    "The requested port number may be too high.");
		goto cleanup_tcp;
	}
	printf("%u\n", real_port);

	// Lose connection to caller
	if (fork() > 0 && !debug_mode)
		exit(0);

	for (;;) {
		struct service_thread_param *args = calloc(1, sizeof(*args));
		if (args == NULL) {
			ERR("Failed to alloc space for thread args");
			goto cleanup_thread;
		}

		args->usb_sock = usb_sock;
		args->tcp = tcp_conn_accept(tcp_socket);
		if (args->tcp == NULL) {
			ERR("Failed to open tcp connection");
			goto cleanup_thread;
		}

		int status = pthread_create(&args->thread_handle, NULL,
		                            &service_connection, args);
		if (status) {
			ERR("Failed to spawn thread, error %d", status);
			goto cleanup_thread;
		}

		continue;

	cleanup_thread:
		if (args != NULL) {
			if (args->tcp != NULL)
				tcp_conn_close(args->tcp);
			free(args);
		}
		break;
	}

cleanup_tcp:
	if (tcp_socket!= NULL)
		tcp_close(tcp_socket);
cleanup_usb:
	if (usb_sock != NULL)
		usb_close(usb_sock);
	return;
}

int main(int argc, char *argv[])
{
	int c;
	long long port = 0;
	int show_help = 0;
	int debug_mode = 0;
	setting_log_target = LOGGING_STDERR;

	while ((c = getopt(argc, argv, "hdp:u:s:l")) != -1) {
		switch (c) {
		case '?':
		case 'h':
			show_help = 1;
			break;
		case 'p':
			// Request specific port
			port = atoi(optarg);
			if (port < 0) {
				ERR("Port number must be non-negative");
				return 1;
			}
			if (port > (long long)UINT_MAX) {
				ERR("Port number must be %u or less, "
				    "but not negative", UINT_MAX);
				return 2;
			}
			break;
		case 'u':
			// [u]sb device to bind with
			break;
		case 'l':
			// Redirect logging to syslog
			setting_log_target = LOGGING_SYSLOG;
			break;
		case 'd':
			// Redirect logging to syslog
			debug_mode = 1;
			break;
		}
	}

	if (show_help) {
		printf(
		"Usage: %s -v <vendorid> -m <productid> -p <port>\n"
		"Options:\n"
		"  -h           Show this help message\n"
		"  -v <vid>     Vendor ID of desired printer\n"
		"  -m <pid>     Product ID of desired printer\n"
		"  -s <serial>  Serial number of desired printer\n"
		"  -p <portnum> Port number to bind against\n"
		"  -l           Redirect logging to syslog\n"
		"  -d           Debug mode for verbose output\n"
		, argv[0]);
		return 0;
	}

	start_daemon((uint32_t)port, debug_mode);
	return 0;
}
