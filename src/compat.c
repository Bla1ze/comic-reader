#define _GNU_SOURCE
/* The SDK's sysroot is glibc 2.23, which has no getentropy() (MuPDF's random
 * numbers use it). Ask the kernel directly, as glibc 2.25+ does. */
#include <errno.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_getrandom
#define SYS_getrandom 278 /* aarch64 */
#endif

int getentropy(void *buf, size_t len)
{
	char *p = buf;
	if (len > 256) { errno = EIO; return -1; }
	while (len) {
		long n = syscall(SYS_getrandom, p, len, 0);
		if (n < 0) { if (errno == EINTR) continue; return -1; }
		p += n;
		len -= (size_t)n;
	}
	return 0;
}
