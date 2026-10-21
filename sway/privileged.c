#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_security_context_v1.h>
#include "sway/log.h"
#include "sway/privileged.h"

struct privileged_client {
	struct wl_listener destroy;
};

static void handle_privileged_client_destroy(struct wl_listener *listener,
		void *data) {
	struct privileged_client *priv = wl_container_of(listener, priv, destroy);
	wl_list_remove(&priv->destroy.link);
	free(priv);
}

void privileged_client_add(struct wl_client *client) {
	if (privileged_client_check(client)) {
		return;
	}
	struct privileged_client *priv = calloc(1, sizeof(*priv));
	if (!priv) {
		// Fail closed: the client stays unprivileged
		sway_log(SWAY_ERROR, "Unable to allocate privileged client");
		return;
	}
	priv->destroy.notify = handle_privileged_client_destroy;
	wl_client_add_destroy_listener(client, &priv->destroy);
}

bool privileged_client_check(const struct wl_client *client) {
	// Our destroy listener doubles as the mark, so it can't outlive the client
	return wl_client_get_destroy_listener((struct wl_client *)client,
		handle_privileged_client_destroy) != NULL;
}

bool privileged_security_context_filter(
		const struct wlr_security_context_v1_state *state,
		struct wl_client *parent_client, void *data) {
	// Always strict, even with "privileged_clients all": window rules trust
	// this engine to mean "a VM", so no ordinary app may claim it. Any engine
	// containing the name is refused, because criteria are unanchored regexes:
	// sandbox_engine="scroll-vm" also matches "my-scroll-vm".
	if (state->sandbox_engine != NULL &&
			strstr(state->sandbox_engine, PRIVILEGED_SANDBOX_ENGINE) != NULL &&
			!privileged_client_check(parent_client)) {
		sway_log(SWAY_ERROR, "Refused security context with sandbox engine "
			"\"%s\" from an unprivileged client", state->sandbox_engine);
		return false;
	}
	return true;
}
