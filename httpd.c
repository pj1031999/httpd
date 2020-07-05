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
	while ((opt = getopt(argc, argv, "fr:u:g:l:p")) != -1)
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
do_daemon(struct httpd_cfg const *cfg)
{
	if (cfg->foreground == false) {
		httpd_log = httpd_syslog;
		if (daemon(0, 0) == -1) {
			error("failed to daemonize: '%m'");
			return -1;
		}
	}
	return 0;
}

static int
do_chroot(struct httpd_cfg const *cfg)
{
	if (cfg->rootdir == NULL) {
		warn("chroot directory is not specified");
		return 0;
	}
	if (chroot(cfg->rootdir) == -1) {
		error("chroot failed: '%m'");
		return -1;
	}
	if (chdir("/") == -1) {
		error("chdir failed: '%m'");
		return -1;
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	struct httpd_cfg cfg;
	if (parse_args(argc, argv, &cfg) == -1)
		return 1;
	if (do_daemon(&cfg) == -1)
		return 1;
	if (do_chroot(&cfg) == -1)
		return 1;
	return 0;
}
