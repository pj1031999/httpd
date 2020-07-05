#ifndef _HTTPD_COMMON_
#define _HTTPD_COMMON_

#include <stdarg.h>
#include <syslog.h>

extern void (*httpd_log)(int severity, const char *fmt, ...);

#define debug(fmt, ...) httpd_log(LOG_DEBUG, ".  " fmt, ##__VA_ARGS__)
#define info(fmt, ...) httpd_log(LOG_INFO, "   " fmt, ##__VA_ARGS__)
#define ok(fmt, ...) httpd_log(LOG_NOTICE, " + " fmt, ##__VA_ARGS__)
#define warn(fmt, ...) httpd_log(LOG_WARNING, "-- " fmt, ##__VA_ARGS__)
#define error(fmt, ...) httpd_log(LOG_ERR, "!! " fmt, ##__VA_ARGS__)

#define fatal(fmt, ...)                    \
	do {                               \
		error(fmt, ##__VA_ARGS__); \
		abort();                   \
	} while (0)

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif /* __unused */

#endif /* _HTTPD_COMMON_ */
