/* C runtime glue: start-up (.data/.bss) and the newlib system calls. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "console.h"
#include "timer.h"

extern uint32_t _data_load[], _data_start[], _data_end[];
extern uint32_t _bss_start[], _bss_end[];
extern char _heap_start[], _heap_end[];

int main(void);
void __libc_init_array(void);

/* Called from crt0 with only the stack set up: no globals before the copy. */
void _cstart(void)
{
	uint32_t *src = _data_load, *dst = _data_start;

	while (dst < _data_end)
		*dst++ = *src++;
	for (dst = _bss_start; dst < _bss_end; dst++)
		*dst = 0;
	/* .init (crtbegin registers the C++ exception tables) and constructors */
	__libc_init_array();
	main();
	for (;;)
		;
}

void *sbrk(ptrdiff_t incr)
{
	static char *brk = _heap_start;
	char *prev = brk;

	if (brk + incr > _heap_end || brk + incr < _heap_start) {
		errno = ENOMEM;
		return (void *)-1;
	}
	brk += incr;
	return prev;
}

int write(int fd, const void *buf, size_t len)
{
	(void)fd;
	console_write(buf, (int)len);
	return (int)len;
}

int read(int fd, void *buf, size_t len)
{
	(void)fd; (void)buf; (void)len;
	return 0;
}

int close(int fd)          { (void)fd; return -1; }
int isatty(int fd)         { (void)fd; return 1; }
int lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int getpid(void)           { return 1; }
int kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }

int fstat(int fd, struct stat *st)
{
	(void)fd;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR;
	return 0;
}

/* for libstdc++'s std::chrono clocks and std::this_thread::sleep_for */
int gettimeofday(struct timeval *tv, void *tz)
{
	uint64_t us = timer_ticks() / (CPU_HZ / 1000000u);

	(void)tz;
	tv->tv_sec = us / 1000000u;
	tv->tv_usec = us % 1000000u;
	return 0;
}

int usleep(unsigned long us)
{
	delay_us(us);
	return 0;
}

unsigned sleep(unsigned s)
{
	delay_ms(1000u * s);
	return 0;
}

void _exit(int status)
{
	(void)status;
	for (;;)
		;
}
