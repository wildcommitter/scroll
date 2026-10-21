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

/**
 * Security contexts with this sandbox engine are VM windows (waypipe from a
 * privileged helper). Only a privileged client can commit one whose engine
 * contains this string, so window rules can match it as an unspoofable
 * identity.
 */
#define PRIVILEGED_SANDBOX_ENGINE "scroll-vm"

struct wlr_security_context_v1_state;

// commit_filter for the security context manager: refuses engines containing
// PRIVILEGED_SANDBOX_ENGINE from unprivileged parents
bool privileged_security_context_filter(
	const struct wlr_security_context_v1_state *state,
	struct wl_client *parent_client, void *data);

#endif
