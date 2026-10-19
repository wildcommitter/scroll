#ifndef _SWAY_SANDBOX_H
#define _SWAY_SANDBOX_H

#include <stdbool.h>

// Confine the compositor process: Landlock (best effort, if the kernel has
// it) and a seccomp filter (fail closed). Call once, after init, right before
// the event loop. Children are spawned by the broker (sway/spawn.c) and don't
// inherit either. config_path: the main config file, whose directory stays
// readable for reloads; may be NULL.
bool sandbox_apply(const char *config_path);

#endif
