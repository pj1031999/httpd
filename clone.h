#ifndef _CLONE_H_
#define _CLONE_H_

#include <linux/sched.h>

long clone3(struct clone_args *args, size_t size);

#endif /* _CLONE_H_ */
