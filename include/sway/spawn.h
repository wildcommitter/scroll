#ifndef _SWAY_SPAWN_H
#define _SWAY_SPAWN_H

#include <stdbool.h>
#include <sys/types.h>

// All fork/exec goes through a broker process forked at startup, before the
// compositor creates any socket or reads any input. The compositor itself may
// then run under a sandbox that can't exec (see sway/sandbox.c), and children
// never inherit that sandbox.

struct spawn_options {
	bool setsid;
	// Given to the child, which gets WAYLAND_SOCKET=<fd>; -1 for none
	int wayland_fd;
	// Becomes the child's stdin; -1 to inherit the broker's
	int stdin_fd;
	// NULL-terminated "NAME=value" entries set on top of the compositor's
	// current environment; may be NULL
	const char *const *env;
};

// Fork the broker. Call once, early in main().
bool spawn_init(void);

void spawn_fini(void);

// Run argv[0], looked up in PATH. Returns the child's pid, or -1.
pid_t spawn_argv(char *const argv[], const struct spawn_options *opts);

// Run cmd with sh -c. Returns the child's pid, or -1.
pid_t spawn_shell(const char *cmd, const struct spawn_options *opts);

#endif
