#include <unistd.h>

#include "httpd_common.h"
#include "httpd_serve.h"

noreturn void
httpd_serve(int sfd)
{
	(void)sfd;
	for (;;) {
		sleep(10);
	}
}
