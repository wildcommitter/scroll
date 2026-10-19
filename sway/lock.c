#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_scene.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/log.h"
#include "sway/input/cursor.h"
#include "sway/input/keyboard.h"
#include "sway/input/seat.h"
#include "sway/layers.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/lock.h"
#include "sway/desktop/transaction.h"
#include "stringop.h"

// The locker started by sway_session_lock_request, until it disconnects
static struct wl_client *locker_client = NULL;
static struct wl_listener locker_client_destroy;

// $XDG_RUNTIME_DIR/scroll-locked.<pid> exists while the session is locked,
// abandoned locks included. scroll-lock waits for it.
static char *lock_state_path(void) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || dir[0] != '/') {
		return NULL;
	}
	return format_str("%s/scroll-locked.%d", dir, (int)getpid());
}

static void lock_state_set(bool locked) {
	char *path = lock_state_path();
	if (!path) {
		return;
	}
	if (locked) {
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0) {
			sway_log_errno(SWAY_ERROR, "Unable to create %s", path);
		} else {
			close(fd);
		}
	} else if (unlink(path) != 0 && errno != ENOENT) {
		sway_log_errno(SWAY_ERROR, "Unable to remove %s", path);
	}
	free(path);
}

struct sway_session_lock_output {
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *background;
	struct sway_session_lock *lock;

	struct sway_output *output;

	struct wl_list link; // sway_session_lock::outputs

	struct wl_listener destroy;

	struct wlr_session_lock_surface_v1 *surface;

	// invalid if surface is NULL
	struct wl_listener surface_destroy;
	struct wl_listener surface_map;
};

static void focus_surface(struct sway_session_lock *lock,
		struct wlr_surface *focused) {
	lock->focused = focused;

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_focus_surface(seat, focused, false);
	}
}

static void refocus_output(struct sway_session_lock_output *output) {
	// Move the seat focus to another surface if one is available
	if (output->lock->focused == output->surface->surface) {
		struct wlr_surface *next_focus = NULL;

		struct sway_session_lock_output *candidate;
		wl_list_for_each(candidate, &output->lock->outputs, link) {
			if (candidate == output || !candidate->surface) {
				continue;
			}

			if (candidate->surface->surface->mapped) {
				next_focus = candidate->surface->surface;
				break;
			}
		}

		focus_surface(output->lock, next_focus);
	}
}

static void handle_surface_map(struct wl_listener *listener, void *data) {
	struct sway_session_lock_output *surf = wl_container_of(listener, surf, surface_map);
	if (surf->lock->focused == NULL) {
		focus_surface(surf->lock, surf->surface->surface);
	}
	cursor_rebase_all();
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct sway_session_lock_output *output =
		wl_container_of(listener, output, surface_destroy);
	refocus_output(output);

	sway_assert(output->surface, "Trying to destroy a surface that the lock doesn't think exists");
	output->surface = NULL;
	wl_list_remove(&output->surface_destroy.link);
	wl_list_remove(&output->surface_map.link);
}

static void lock_output_reconfigure(struct sway_session_lock_output *output) {
	int width = output->output->width;
	int height = output->output->height;

	wlr_scene_rect_set_size(output->background, width, height);

	if (output->surface) {
		wlr_session_lock_surface_v1_configure(output->surface, width, height);
	}
}

void arrange_locks(void) {
	if (server.session_lock.lock == NULL) {
		return;
	}

	struct sway_session_lock_output *lock_output;
	wl_list_for_each(lock_output, &server.session_lock.lock->outputs, link) {
		lock_output_reconfigure(lock_output);
	}
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct sway_session_lock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	struct sway_output *output = lock_surface->output->data;

	sway_log(SWAY_DEBUG, "new lock layer surface");

	struct sway_session_lock_output *current_lock_output, *lock_output = NULL;
	wl_list_for_each(current_lock_output, &lock->outputs, link) {
		if (current_lock_output->output == output) {
			lock_output = current_lock_output;
			break;
		}
	}
	sway_assert(lock_output, "Couldn't find output to lock");
	sway_assert(!lock_output->surface, "Tried to reassign a surface to an existing output");

	lock_output->surface = lock_surface;

	wlr_scene_subsurface_tree_create(lock_output->tree, lock_surface->surface);

	lock_output->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&lock_surface->events.destroy, &lock_output->surface_destroy);
	lock_output->surface_map.notify = handle_surface_map;
	wl_signal_add(&lock_surface->surface->events.map, &lock_output->surface_map);

	lock_output_reconfigure(lock_output);
}

static void sway_session_lock_output_destroy(struct sway_session_lock_output *output) {
	if (output->surface) {
		refocus_output(output);
		wl_list_remove(&output->surface_destroy.link);
		wl_list_remove(&output->surface_map.link);
	}

	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);

	free(output);
}

static void lock_node_handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_session_lock_output *output =
		wl_container_of(listener, output, destroy);
	sway_session_lock_output_destroy(output);
}

static struct sway_session_lock_output *session_lock_output_create(
		struct sway_session_lock *lock, struct sway_output *output) {
	struct sway_session_lock_output *lock_output = calloc(1, sizeof(*lock_output));
	if (!lock_output) {
		sway_log(SWAY_ERROR, "failed to allocate a session lock output");
		return NULL;
	}

	struct wlr_scene_tree *tree = wlr_scene_tree_create(output->layers.session_lock);
	if (!tree) {
		sway_log(SWAY_ERROR, "failed to allocate a session lock output scene tree");
		free(lock_output);
		return NULL;
	}

	struct wlr_scene_rect *background = wlr_scene_rect_create(tree, 0, 0, (float[4]){
		lock->abandoned ? 1.f : 0.f,
		0.f,
		0.f,
		1.f,
	});
	if (!background) {
		sway_log(SWAY_ERROR, "failed to allocate a session lock output scene background");
		wlr_scene_node_destroy(&tree->node);
		free(lock_output);
		return NULL;
	}

	lock_output->output = output;
	lock_output->tree = tree;
	lock_output->background = background;
	lock_output->lock = lock;

	lock_output->destroy.notify = lock_node_handle_destroy;
	wl_signal_add(&tree->node.events.destroy, &lock_output->destroy);

	lock_output_reconfigure(lock_output);

	wl_list_insert(&lock->outputs, &lock_output->link);

	return lock_output;
}

static void sway_session_lock_destroy(struct sway_session_lock* lock) {
	struct sway_session_lock_output *lock_output, *tmp_lock_output;
	wl_list_for_each_safe(lock_output, tmp_lock_output, &lock->outputs, link) {
		// destroying the node will also destroy the whole lock output
		wlr_scene_node_destroy(&lock_output->tree->node);
	}

	if (server.session_lock.lock == lock) {
		server.session_lock.lock = NULL;
	}

	if (!lock->abandoned) {
		wl_list_remove(&lock->destroy.link);
		wl_list_remove(&lock->unlock.link);
		wl_list_remove(&lock->new_surface.link);
	}

	free(lock);
}

static void handle_unlock(struct wl_listener *listener, void *data) {
	struct sway_session_lock *lock = wl_container_of(listener, lock, unlock);
	sway_log(SWAY_DEBUG, "session unlocked");

	sway_session_lock_destroy(lock);
	lock_state_set(false);

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		// copied from seat_set_focus_layer -- deduplicate?
		struct sway_node *previous = seat_get_focus_inactive(seat, &root->node);
		if (previous) {
			// Hack to get seat to re-focus the return value of get_focus
			seat_set_focus(seat, NULL);
			seat_set_focus(seat, previous);
		}
	}

	// Triggers a refocus of the topmost surface layer if necessary
	// TODO: Make layer surface focus per-output based on cursor position
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *output = root->outputs->items[i];
		arrange_layers(output);
	}

	// Views are now visible, so check if we need to activate inhibition again.
	sway_idle_inhibit_v1_check_active();

	transaction_commit_dirty();
}

static void handle_abandon(struct wl_listener *listener, void *data) {
	struct sway_session_lock *lock = wl_container_of(listener, lock, destroy);
	sway_log(SWAY_INFO, "session lock abandoned");

	struct sway_session_lock_output *lock_output;
	wl_list_for_each(lock_output, &lock->outputs, link) {
		wlr_scene_rect_set_color(lock_output->background,
			(float[4]){ 1.f, 0.f, 0.f, 1.f });
	}

	lock->abandoned = true;
	wl_list_remove(&lock->destroy.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->new_surface.link);
}

static void handle_session_lock(struct wl_listener *listener, void *data) {
	struct wlr_session_lock_v1 *lock = data;
	struct wl_client *client = wl_resource_get_client(lock->resource);

	if (server.session_lock.lock) {
		if (server.session_lock.lock->abandoned) {
			sway_log(SWAY_INFO, "Replacing abandoned lock");
			sway_session_lock_destroy(server.session_lock.lock);
		} else {
			sway_log(SWAY_ERROR, "Cannot lock an already locked session");
			wlr_session_lock_v1_destroy(lock);
			return;
		}
	}

	struct sway_session_lock *sway_lock = calloc(1, sizeof(*sway_lock));
	if (!sway_lock) {
		sway_log(SWAY_ERROR, "failed to allocate a session lock object");
		wlr_session_lock_v1_destroy(lock);
		return;
	}

	wl_list_init(&sway_lock->outputs);

	sway_log(SWAY_DEBUG, "session locked");

	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_unfocus_unless_client(seat, client);
	}

	struct sway_output *output;
	wl_list_for_each(output, &root->all_outputs, link) {
		sway_session_lock_add_output(sway_lock, output);
	}

	sway_lock->new_surface.notify = handle_new_surface;
	wl_signal_add(&lock->events.new_surface, &sway_lock->new_surface);
	sway_lock->unlock.notify = handle_unlock;
	wl_signal_add(&lock->events.unlock, &sway_lock->unlock);
	sway_lock->destroy.notify = handle_abandon;
	wl_signal_add(&lock->events.destroy, &sway_lock->destroy);

	wlr_session_lock_v1_send_locked(lock);
	server.session_lock.lock = sway_lock;
	lock_state_set(true);

	// The lock screen covers everything, so check if any active inhibition got
	// deactivated due to lost visibility.
	sway_idle_inhibit_v1_check_active();
}

static void handle_session_lock_destroy(struct wl_listener *listener, void *data) {
	// if the server shuts down while a lock is active, destroy the lock
	if (server.session_lock.lock) {
		sway_session_lock_destroy(server.session_lock.lock);
	}

	lock_state_set(false);

	wl_list_remove(&server.session_lock.new_lock.link);
	wl_list_remove(&server.session_lock.manager_destroy.link);

	server.session_lock.manager = NULL;
}

static void handle_locker_client_destroy(struct wl_listener *listener, void *data) {
	wl_list_remove(&locker_client_destroy.link);
	locker_client = NULL;
}

bool sway_session_lock_request(void) {
	struct sway_session_lock *lock = server.session_lock.lock;
	if (lock && !lock->abandoned) {
		sway_log(SWAY_DEBUG, "Lock requested, but the session is already locked");
		return true;
	}
	if (!lock && locker_client) {
		sway_log(SWAY_DEBUG, "Lock requested, but the locker is still starting");
		return true;
	}
	if (!config->lock_command) {
		sway_log(SWAY_ERROR, "Lock requested, but no lock_command is configured");
		return false;
	}
	if (locker_client) {
		// The old locker abandoned the lock but kept its connection
		wl_list_remove(&locker_client_destroy.link);
		locker_client = NULL;
	}
	struct wl_client *client = NULL;
	if (exec_spawn(config->lock_command, true, &client) < 0 || !client) {
		sway_log(SWAY_ERROR, "Unable to start the locker '%s'", config->lock_command);
		return false;
	}
	locker_client = client;
	locker_client_destroy.notify = handle_locker_client_destroy;
	wl_client_add_destroy_listener(client, &locker_client_destroy);
	sway_log(SWAY_INFO, "Started the locker '%s'", config->lock_command);
	return true;
}

void sway_session_lock_add_output(struct sway_session_lock *lock,
		struct sway_output *output) {
	struct sway_session_lock_output *lock_output =
		session_lock_output_create(lock, output);

	// if we run out of memory while trying to lock the screen, the best we
	// can do is kill the sway process. Security conscious users will have
	// the sway session fall back to a login shell.
	if (!lock_output) {
		sway_log(SWAY_ERROR, "aborting: failed to allocate a lock output");
		abort();
	}
}

bool sway_session_lock_has_surface(struct sway_session_lock *lock,
		struct wlr_surface *surface) {
	struct sway_session_lock_output *lock_output;
	wl_list_for_each(lock_output, &lock->outputs, link) {
		if (lock_output->surface && lock_output->surface->surface == surface) {
			return true;
		}
	}

	return false;
}

bool sway_session_lock_init(void) {
	// A stale file from an earlier scroll with our pid
	lock_state_set(false);

	server.session_lock.manager = wlr_session_lock_manager_v1_create(server.wl_display);
	if (!server.session_lock.manager) {
		sway_log(SWAY_ERROR, "Failed to create session lock manager");
		return false;
	}
	server.session_lock.new_lock.notify = handle_session_lock;
	server.session_lock.manager_destroy.notify = handle_session_lock_destroy;
	wl_signal_add(&server.session_lock.manager->events.new_lock,
		&server.session_lock.new_lock);
	wl_signal_add(&server.session_lock.manager->events.destroy,
		&server.session_lock.manager_destroy);
	return true;
}
