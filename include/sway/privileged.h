#ifndef _SWAY_PRIVILEGED_H
#define _SWAY_PRIVILEGED_H
#include <stdbool.h>
#include <wayland-server-core.h>

/**
 * Privileged clients are the ones scroll creates itself over a socketpair
 * (WAYLAND_SOCKET): swaybg, scrollnag, scrollbar, exec_privileged and
 * lock_command. With "privileged_clients spawned" (the default), only they can
 * bind the restricted globals (screen capture, virtual input, session lock,
 * layer shell, ...).
 */
void privileged_client_add(struct wl_client *client);

bool privileged_client_check(const struct wl_client *client);

#endif
