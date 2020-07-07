#define _GNU_SOURCE

#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/in.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "httpd_common.h"
#include "httpd_serve.h"

#define BUF_SIZE 32768
#define MAX_EVENTS 16

/* clang-format off */
__unused static char const *http_extension_map[] = {
	"application/ogg",	".ogg",
	"application/pdf",      ".pdf",
	"application/wasm",     ".wasm",
	"application/xml",      ".xml",
	"application/zip",      ".zip",
	"audio/mpeg",           ".mp3",
	"image/gif",            ".gif",
	"image/jpeg",           ".jpg",
	"image/png",            ".png",
	"image/svg+xml",        ".svg",
	"text/css",             ".css",
	"text/html",            ".html",
	"text/javascript",      ".js",
	"text/plain",           ".txt",
	"text/plain",           ".asc",
	"video/mpeg",           ".mpeg",
	"video/x-msvideo",      ".avi",
	"video/mp4",            ".mp4",
	NULL,			NULL,
};

enum {
	HTTP_OK			    = 200,
	HTTP_MOVED_PERMANENTLY	    = 301,
	HTTP_FORBIDDEN		    = 403,
	HTTP_NOT_FOUND		    = 404,
	HTTP_INTERNAL_SERVER_ERROR  = 500,
	HTTP_NOT_IMPLEMENTED	    = 501
};

struct http_code {
	int code;
	char const *str;
};

static const struct http_code codes[] = {
	{ HTTP_OK,		      "OK"		      },
	{ HTTP_MOVED_PERMANENTLY,     "Moved Permanently"     },
	{ HTTP_FORBIDDEN,	      "Forbidden"	      },
	{ HTTP_NOT_FOUND,	      "Not Found"	      },
	{ HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error" },
	{ HTTP_NOT_IMPLEMENTED,	      "Not Implemented"	      },
	{ -1,			      NULL		      }
};
/* clang-format on */

static char const *
httpd_extension(char const *path)
{
	char const *cptr;

	if ((cptr = strrchr(path, '.')) == NULL) {
		return "application/octet-stream";
	}

	for (char const *const *p = http_extension_map + 1; *p != NULL;
	     p += 2) {
		if (strcmp(cptr, *p) == 0) {
			return *(p - 1);
		}
	}

	return "application/octet-stream";
}

static void
httpd_send(int fd, char const *buf, int nbytes)
{
	int nsent;

	while (nbytes > 0) {
		if ((nsent = send(fd, buf, nbytes, 0)) == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			fatal("httpd_send: failed: '%m'");
		}
		nbytes -= nsent;
		buf += nsent;
	}
}

static int
httpd_serve_reply(int fd, int errcode, char const *uri)
{
	char hbuf[2 * BUF_SIZE + 1];
	char cbuf[BUF_SIZE + 1];
	struct http_code const *code;
	int http_len;
	int content_len;

	for (code = codes; code->str != NULL; ++code) {
		if (code->code == errcode)
			break;
	}

	if (code->code == -1) {
		fatal("httpd_serve_reply: unknown errcode: %d", errcode);
	}

	content_len = snprintf(cbuf, BUF_SIZE,
	    "<html>"
	    "<head><title>%d %s</title></head>"
	    "<body>"
	    "<h1>%d %s</h1>"
	    "</body>"
	    "</html>",
	    code->code, code->str, code->code, code->str);

	if (uri) {
		http_len = snprintf(hbuf, 2 * BUF_SIZE,
		    "HTTP/1.1 %d %s\r\n"
		    "Content-Length: %d\r\n"
		    "Content-Type: %s\r\n"
		    "Server: httpd (built on " __DATE__ " " __TIME__ ")\r\n"
		    "Location: %s\r\n"
		    "\r\n"
		    "%s\r\n",
		    code->code, code->str, content_len, "text/html", uri, cbuf);
	} else {
		http_len = snprintf(hbuf, 2 * BUF_SIZE,
		    "HTTP/1.1 %d %s\r\n"
		    "Content-Length: %d\r\n"
		    "Content-Type: %s\r\n"
		    "Server: httpd (built on " __DATE__ " " __TIME__ ")\r\n"
		    "\r\n"
		    "%s\r\n",
		    code->code, code->str, content_len, "text/html", cbuf);
	}

	httpd_send(fd, hbuf, http_len);
	return 0;
}

static inline int
httpd_serve_redirect(int fd, char const *uri)
{
	char path[2 * PATH_MAX + 1];
	int len;
	len = snprintf(path, 2 * PATH_MAX, "%sindex.html", uri);
	path[len] = 0;
	return httpd_serve_reply(fd, HTTP_MOVED_PERMANENTLY, path);
}

static inline int
is_dir(char const *path)
{

	DIR *dir;
	if ((dir = opendir(path)) != NULL) {
		closedir(dir);
		return 1;
	}
	return 0;
}

static int
httpd_serve_file(int fd, char const *path)
{
	char hbuf[BUF_SIZE + 1];
	char const *cstr;
	struct stat st;
	int len;
	int ffd;
	off_t fsize;
	ssize_t sent;

	stat(path, &st);
	fsize = st.st_size;

	cstr = httpd_extension(path);

	len = snprintf(hbuf, BUF_SIZE,
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Length: %lu\r\n"
	    "Content-Type: %s\r\n"
	    "Server: (built on " __DATE__ " " __TIME__ ")\r\n"
	    "\r\n",
	    fsize, cstr);

	httpd_send(fd, hbuf, len);

	if ((ffd = open(path, O_RDONLY)) == -1) {
		return httpd_serve_reply(fd, HTTP_NOT_FOUND, NULL);
	}

	while (fsize > 0) {
		if ((sent = sendfile(fd, ffd, NULL, fsize)) == -1) {
			if (errno == EAGAIN)
				continue;
			fatal("httpd_serve_file: failed: '%m'");
		}
		fsize -= sent;
	}

	close(ffd);
	return 0;
}

static int
httpd_serve_http(int fd, char *buf, __unused int nbytes)
{
	char path[2 * PATH_MAX + 1];
	char const *getstr;
	ssize_t len;

	bzero(path, sizeof(path));

	if (strstr(buf, "Connection: close"))
		return 1;

	if ((getstr = strstr(buf, "GET")) == NULL ||
	    strstr(buf, "HTTP/1.1") == NULL) {
		warn("httpd_serve_http: not implemented");
		httpd_serve_reply(fd, HTTP_NOT_IMPLEMENTED, NULL);
		return 1;
	}

	sscanf(getstr, "%*s %4096s", path);
	len = strlen(path);
	debug("httpd_serve_http: '%s'", path);

	if (len == 0) {
		httpd_serve_reply(fd, HTTP_INTERNAL_SERVER_ERROR, NULL);
		return 1;
	}

	if (path[len - 1] == '/') {
		httpd_serve_redirect(fd, path);
		return 1;
	}

	if (access(path, F_OK) == -1) {
		httpd_serve_reply(fd, HTTP_NOT_FOUND, NULL);
		return 1;
	}

	if (is_dir(path)) {
		path[len] = '/';
		httpd_serve_redirect(fd, path);
		return 1;
	}

	return httpd_serve_file(fd, path);
}

static void
httpd_serve_fd(int fd)
{
	char buf[BUF_SIZE + 1];

	for (;;) {
		int nbytes = recv(fd, buf, BUF_SIZE, 0);

		if (nbytes == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/*
				 * Try again in the future.
				 */
				return;
			}
			warn("httpd_serve_fd: read failed: '%m'");
			close(fd);
			return;
		}
		buf[nbytes] = 0;

		if (nbytes == 0) {
			/* EOF */
			close(fd);
			return;
		}

		/*
		 * Now we can handle http request.
		 */
		if (httpd_serve_http(fd, buf, nbytes)) {
			close(fd);
			return;
		}
	}
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
