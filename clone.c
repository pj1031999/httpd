#include <sys/syscall.h>

#include <linux/sched.h>
#include <linux/types.h>
#include <unistd.h>

static inline pid_t
sys_clone3(struct clone_args *args, size_t size)
{
	return syscall(__NR_clone3, args, size);
}

long
clone3(struct clone_args *args, size_t size)
{
	return sys_clone3(args, size);
}
