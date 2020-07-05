#define _GNU_SOURCE

#include <netinet/in.h>

#include <arpa/inet.h>
#include <getopt.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>

#include "httpd_common.h"

static void
httpd_syslog(int severity, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsyslog(severity, fmt, args);
	va_end(args);
}

static void
httpd_stderr(__unused int severity, const char *fmt, ...)
{
	static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;

	va_list args;
	va_start(args, fmt);
	pthread_mutex_lock(&mux);

	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);

	pthread_mutex_unlock(&mux);
	va_end(args);
}

void (*httpd_log)(int severity, const char *fmt, ...) = httpd_stderr;

struct httpd_cfg {
	char *rootdir;
	char *address;
	int uid;
	int gid;
	int port;
	bool foreground;
};

static void
print_usage(void)
{
	fprintf(stderr,
	    "USAGE: httpd [-f] [-r path] [-u uid] [-g gid]\n"
	    "	      [-l addr] [-p port]\n"
	    "  -f	    run in foreground\n"
	    "  -r path  set path to root directory\n"
	    "  -u uid   set uid of httpd user\n"
	    "  -g gid   set gid of httpd group\n"
	    "  -l addr  set listen address\n"
	    "  -p port  set listen port\n");
}

static int
parse_args(int argc, char *argv[], struct httpd_cfg *cfg)
{
	bzero(cfg, sizeof(struct httpd_cfg));
	int opt;
	while ((opt = getopt(argc, argv, "fr:u:g:l:p:")) != -1)
		switch (opt) {
		case 'f':
			cfg->foreground = true;
			break;
		case 'r':
			cfg->rootdir = strdup(optarg);
			break;
		case 'u':
			cfg->uid = atoi(optarg);
			break;
		case 'g':
			cfg->gid = atoi(optarg);
			break;
		case 'l':
			cfg->address = strdup(optarg);
			break;
		case 'p':
			cfg->port = atoi(optarg);
			break;
		default:
			print_usage();
			return -1;
		}

	if (optind < argc) {
		print_usage();
		return -1;
	}

	return 0;
}

static int
do_logging(struct httpd_cfg const *cfg)
{
	if (cfg->foreground == false) {
		httpd_log = httpd_syslog;
	}
	return 0;
}

static int
do_daemon(struct httpd_cfg const *cfg)
{
	if (cfg->foreground == false) {
		if (daemon(0, 0) == -1) {
			error("do_daemon: failed to daemonize: '%m'");
			return -1;
		}
	}
	return 0;
}

static int
do_chroot(struct httpd_cfg const *cfg)
{
	if (cfg->rootdir == NULL) {
		warn("do_chroot: chroot directory is not specified");
		return 0;
	}
	if (chroot(cfg->rootdir) == -1) {
		error("do_chroot: chroot failed: '%m'");
		return -1;
	}
	if (chdir("/") == -1) {
		error("do_chroot: chdir failed: '%m'");
		return -1;
	}

	info("do_chroot: chrooting done");
	return 0;
}

static int
do_bind(struct httpd_cfg const *cfg)
{
	int sfd;
	struct sockaddr_in sa;

	if (cfg->address == NULL) {
		error("do_bind: listen address is not specified");
		return -1;
	}

	if (cfg->port == 0) {
		error("do_bind: listen port is not specified");
		return -1;
	}

	if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		error("do_bind: open socket failed: '%m'");
		return -1;
	}

	bzero(&sa, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(cfg->port);
	if (inet_aton(cfg->address, &sa.sin_addr) == 0) {
		error("do_bind: listen address is not valid: '%m'");
		close(sfd);
		return -1;
	}

	if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
		error("do_bind: bind failed: '%m'"); 
		close(sfd);
		return -1;
	}

	info("do_bind: binding done...");
	return sfd;
}

static int
do_secure(struct httpd_cfg const *cfg)
{
	if (cfg->gid != 0) {
		if (setresgid(cfg->gid, cfg->gid, cfg->gid) == -1) {
			error("do_secure: setresgid failed: '%m'");
			return -1;
		}
	} else {
		warn("do_secure: gid is not specified");
	}

	if (cfg->uid != 0) {
		if (setresuid(cfg->uid, cfg->uid, cfg->uid) == -1) {
			error("do_secure: setresuid failed: '%m'");
			return -1;
		}
	} else {
		warn("do_secure: uid is not specified");
	}

	info("do_secure: done...");
	return 0;
}

int
main(int argc, char *argv[])
{
	struct httpd_cfg cfg;
	int sfd = -1;
	if (parse_args(argc, argv, &cfg) == -1)
		return 1;
	if (do_logging(&cfg) == -1)
		return 1;
	if (do_daemon(&cfg) == -1)
		return 1;
	ok("httpd starting (built on " __DATE__" " __TIME__ ")");
	if ((sfd = do_bind(&cfg)) == -1)
		return 1;
	if (do_chroot(&cfg) == -1)
		return 1;
	if (do_secure(&cfg) == -1)
		return 1;
	return 0;
}
