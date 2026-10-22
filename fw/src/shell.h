/* Line-based command shell on the console. */
#ifndef SHELL_H
#define SHELL_H

struct shell_cmd {
	const char *name;
	const char *args;
	const char *help;
	int (*fn)(int argc, char **argv);
};

void shell_init(void);
void shell_poll(void);   /* call from the main loop; never blocks */

#endif
