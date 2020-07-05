#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <getopt.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>

#include "httpd_common.h"
#include "httpd_serve.h"

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
	int nworkers;
	bool foreground;
};

static void
print_usage(void)
{
	fprintf(stderr,
	    "USAGE: httpd [-f] [-r path] [-u uid] [-g gid]\n"
	    "	      [-l addr] [-p port] [-w nworkers]\n"
	    "  -f	    run in foreground\n"
	    "  -r path	    set path to root directory\n"
	    "  -u uid	    set uid of httpd user\n"
	    "  -g gid	    set gid of httpd group\n"
	    "  -l addr	    set listen address\n"
	    "  -p port	    set listen port\n"
	    "  -w nworkers  set number of workers\n");
}

static int
parse_args(int argc, char *argv[], struct httpd_cfg *cfg)
{
	bzero(cfg, sizeof(struct httpd_cfg));
	int opt;
	while ((opt = getopt(argc, argv, "fr:u:g:l:p:w:")) != -1)
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
		case 'w':
			cfg->nworkers = atoi(optarg);
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

static pid_t
do_spawn(int sfd)
{
	(void)sfd;
	pid_t pid = fork();
	if (pid == 0) {
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		httpd_serve(sfd);
	}
	if (pid == -1) {
		error("do_spawn: fork failed: '%m'");
		return -1;
	}
	info("do_spawn: spawn new worker %d", pid);
	return pid;
}

static int
do_workers(int sfd, struct httpd_cfg const *cfg, pid_t **children)
{
	*children = malloc(sizeof(pid_t) * cfg->nworkers);

	for (int i = 0; i < cfg->nworkers; ++i) {
		(*children)[i] = do_spawn(sfd);
	}

	return 0;
}

static sigjmp_buf shutdown_jmp_buf;
static bool in_shutdown = false;

static void
sig_shutdown(int sig)
{
	if (in_shutdown == false) {
		in_shutdown = true;
		info("caught signal %d (%s), shutting down", sig,
		    strsignal(sig));
		siglongjmp(shutdown_jmp_buf, 1);
	}
}

static int
do_loop(int sfd, struct httpd_cfg const *cfg, pid_t *children)
{
	if (sigsetjmp(shutdown_jmp_buf, 1)) {
		for (int i = 0; i < cfg->nworkers; ++i) {
			if (children[i] != -1) {
				warn("do_loop: terminate %d", children[i]);
				kill(children[i], SIGTERM);
			}
		}
		return 0;
	}

	signal(SIGTERM, sig_shutdown);
	signal(SIGINT, sig_shutdown);
	signal(SIGQUIT, sig_shutdown);

	for (;;) {
		pid_t child;
		int status;

		if ((child = wait(&status)) == -1) {
			error("do_loop: wait failed: '%m'");
			return -1;
		}

		if (WIFEXITED(status)) {
			warn("do_loop: %d exited with code %d", child,
			    WEXITSTATUS(status));
		} else if (WIFSIGNALED(status)) {
			warn("do_loop: %d signaled with signal %d", child,
			    WTERMSIG(status));
		} else {
			warn("do_loop: %d exited in uncommon way", child);
		}

		for (int i = 0; i < cfg->nworkers; ++i) {
			if (children[i] == child) {
				if ((children[i] = do_spawn(sfd)) == -1) {
					error("do_loop: spawn failed");
				}
			}
		}
	}

	/*
	 * NOT REACHABLE
	 */
	fatal("do_loop: dead end");
}

static int
do_shutdown(int sfd, struct httpd_cfg const *cfg)
{
	close(sfd);
	free(cfg->address);
	free(cfg->rootdir);
	return 0;
}

int
main(int argc, char *argv[])
{
	struct httpd_cfg cfg;
	int sfd = -1;
	pid_t *children = NULL;

	if (parse_args(argc, argv, &cfg) == -1)
		return 1;
	if (do_logging(&cfg) == -1)
		return 1;
	if (do_daemon(&cfg) == -1)
		return 1;
	ok("httpd starting (built on " __DATE__ " " __TIME__ ")");
	if ((sfd = do_bind(&cfg)) == -1)
		return 1;
	if (do_chroot(&cfg) == -1)
		return 1;
	if (do_secure(&cfg) == -1)
		return 1;
	if (do_workers(sfd, &cfg, &children) == -1)
		return 1;
	ok("shields up, weapons armed - going live");
	if (do_loop(sfd, &cfg, children) == -1)
		return 1;
	if (do_shutdown(sfd, &cfg) == -1)
		return 1;
	ok("bye bye...");
	return 0;
}
