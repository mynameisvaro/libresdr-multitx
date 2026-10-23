/* Line-based command shell on the console. */
#ifndef SHELL_H
#define SHELL_H

struct shell_cmd {
	const char *name;
	const char *args;
	const char *help;
	int (*fn)(int argc, char **argv);
	/* 0: the command runs with interrupts off, so the protocol pauses while
	 * it touches the radio; SHELL_LIVE: it runs alongside the protocol (and
	 * takes irq_save itself where it needs to) */
	int flags;
};

#define SHELL_LIVE 1

void shell_init(void);
void shell_poll(void);   /* call from the main loop; never blocks */

#endif
