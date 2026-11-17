/* Regression: next/previous froze the controls when nothing was playing
 * (bring-up, patch 0008). squeezelite's CLI connect blocked the caller for
 * every SYN retry on a server that never answers, and ran even with no music
 * server known. Builds upstream's own connect_cli_socket and cli_send_cmd, as
 * patched, against stubbed sockets. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../check.h"

typedef uint16_t u16_t;
typedef uint8_t u8_t;
#define LOG_ERROR(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)

static in_addr_t server_ip;
static u16_t server_cport;
static int cli_sock = -1;
static u8_t mac[6];

/* the stubbed network: a server that accepts the SYN never (select times out) */
static int n_socket, n_connect, n_select, n_send, n_close, nonblock_at_connect, fl_now;
static long select_timeout_us;
static int select_result, so_error;

static int s_socket(int d, int t, int p) { (void) d, (void) t, (void) p; n_socket++; return 7; }
static int s_fcntl(int fd, int cmd, ...) {
	(void) fd;
	if (cmd == F_GETFL) return fl_now;
	va_list ap;
	va_start(ap, cmd);
	fl_now = va_arg(ap, int);
	va_end(ap);
	return 0;
}
static int s_connect(int fd, const struct sockaddr *a, socklen_t l) {
	(void) fd, (void) a, (void) l;
	n_connect++;
	nonblock_at_connect = !!(fl_now & O_NONBLOCK);
	errno = EINPROGRESS;
	return -1;
}
static int s_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
	(void) n, (void) r, (void) w, (void) e;
	n_select++;
	select_timeout_us = tv ? tv->tv_sec * 1000000L + tv->tv_usec : -1;
	return select_result;
}
static int s_getsockopt(int fd, int lv, int o, void *v, socklen_t *l) {
	(void) fd, (void) lv, (void) o, (void) l;
	*(int *) v = so_error;
	return 0;
}
static ssize_t s_send(int fd, const void *b, size_t n, int f) { (void) fd, (void) b, (void) f; n_send++; return (ssize_t) n; }
static ssize_t s_recv(int fd, void *b, size_t n, int f) { (void) fd, (void) b, (void) n, (void) f; return 0; }
static int closesocket(int fd) { (void) fd; n_close++; return 0; }


#define socket s_socket
#define fcntl s_fcntl
#define connect s_connect
#define select s_select
#define getsockopt s_getsockopt
#define send s_send
#define recv s_recv
#include "controls_fns.c"
#undef socket
#undef fcntl
#undef connect
#undef select
#undef getsockopt
#undef send
#undef recv

static void reset(void) {
	n_socket = n_connect = n_select = n_send = n_close = nonblock_at_connect = fl_now = 0;
	select_timeout_us = 0;
	select_result = 0;
	so_error = 0;
	cli_sock = -1;
}

int main(void) {
	char cmd[] = "button fwd";

	/* nothing plays (Spotify, Bluetooth or idle): no server, no socket at all */
	reset();
	server_ip = 0;
	server_cport = 9090;
	cli_send_cmd(cmd);
	CHECK(n_socket == 0 && n_connect == 0);
	server_ip = inet_addr("192.168.1.10");
	server_cport = 0;
	cli_send_cmd(cmd);
	CHECK(n_socket == 0);

	/* a server that never answers: the connect waits at most 1 s, nonblocking */
	reset();
	server_ip = inet_addr("192.168.1.10");
	server_cport = 9090;
	cli_send_cmd(cmd);
	CHECK(n_connect == 1);
	CHECK(nonblock_at_connect);
	CHECK(n_select == 1);
	CHECK(select_timeout_us > 0 && select_timeout_us <= 1000000);
	CHECK(cli_sock == -1);
	CHECK(n_close == 1);
	CHECK(n_send == 0);
	CHECK(!(fl_now & O_NONBLOCK)); /* blocking mode given back */

	/* the server refuses after select: same, no send */
	reset();
	select_result = 1;
	so_error = ECONNREFUSED;
	cli_send_cmd(cmd);
	CHECK(cli_sock == -1 && n_send == 0);

	/* the server answers: connected and the command goes out once */
	reset();
	select_result = 1;
	so_error = 0;
	cli_send_cmd(cmd);
	CHECK(cli_sock == 7 && n_send == 1);
	cli_send_cmd(cmd); /* reuses the socket */
	CHECK(n_connect == 1 && n_send == 2);

	return check_report("up_controls");
}
