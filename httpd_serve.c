#define _GNU_SOURCE

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/in.h>

#include <strings.h>
#include <unistd.h>

#include "httpd_common.h"
#include "httpd_serve.h"

#define MAX_EVENTS 16

static void
httpd_serve_fd(int fd)
{
	close(fd);
}

static void
httpd_serve_loop(int sfd, int efd)
{
	int nfds;
	int fd;
	struct epoll_event ev;
	struct epoll_event evs[MAX_EVENTS];
	struct sockaddr_in sa;
	socklen_t sa_len = sizeof(sa);

	nfds = epoll_wait(efd, evs, MAX_EVENTS, -1);

	if (nfds == -1) {
		fatal("httpd_serve: epoll_wait failed: '%m'");
	}

	for (int i = 0; i < nfds; ++i) {
		/* New incoming connection. */
		if (evs[i].data.fd == sfd) {
			fd = accept4(sfd, (struct sockaddr *)&sa, &sa_len,
			    SOCK_NONBLOCK);
			ev.events = EPOLLIN | EPOLLET;
			ev.data.fd = fd;
			if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) == -1) {
				fatal("httpd_serve: epoll_ctl failed: '%m'");
			}
		} else {
			httpd_serve_fd(evs[i].data.fd);
		}
	}
}

noreturn void
httpd_serve(int sfd, int efd)
{
	for (;;) {
		httpd_serve_loop(sfd, efd);
	}

	/*
	 * NOT REACHABLE
	 */
	fatal("httpd_serve: dead end");
}
