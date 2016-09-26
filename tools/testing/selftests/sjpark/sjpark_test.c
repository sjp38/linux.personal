#define _GNU_SOURCE

#include <unistd.h>
#include <syscall.h>
#include <stdio.h>

#include "../kselftest.h"

#if defined(__i386__)

#define __NR_sjpark 380

#elif defined(__x86_64__)

#define __NR_sjpark 329

#else

#define __NR_sjpark 288

#endif

static int sys_sjpark(unsigned long arg)
{
	return syscall(__NR_sjpark, arg);
}

int main(int argc, char **argv)
{
	if (sys_sjpark(42) == 0)
		return ksft_exit_pass();
	return ksft_exit_fail();
}
