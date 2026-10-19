#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/server.h"
#include "sway/desktop/launcher.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include "sway/log.h"
#include "sway/privileged.h"
#include "sway/spawn.h"
#include "stringop.h"
#include "util.h"

struct cmd_results *cmd_exec_validate(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, argv[-1], EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	if (!config->active || config->validating) {
		return cmd_results_new(CMD_DEFER, NULL);
	}
	return error;
}

pid_t exec_spawn(const char *cmd, bool no_startup_id,
		struct wl_client **privileged_client) {
	int sockets[2] = { -1, -1 };
	struct wl_client *client = NULL;
	if (privileged_client) {
		*privileged_client = NULL;
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
			sway_log_errno(SWAY_ERROR, "socketpair failed");
			return -1;
		}
		if (!sway_set_cloexec(sockets[0], true) || !sway_set_cloexec(sockets[1], true)) {
			close(sockets[0]);
			close(sockets[1]);
			return -1;
		}
		client = wl_client_create(server.wl_display, sockets[0]);
		if (client == NULL) {
			sway_log_errno(SWAY_ERROR, "wl_client_create failed");
			close(sockets[0]);
			close(sockets[1]);
			return -1;
		}
		privileged_client_add(client);
	}

	sway_log(SWAY_DEBUG, "Executing %s%s", cmd, client ? " (privileged)" : "");

	struct launcher_ctx *ctx = launcher_ctx_create_internal();

	char *token_env = NULL, *startup_env = NULL;
	const char *env[3] = { NULL };
	int nenv = 0;
	if (ctx) {
		const char *token = launcher_ctx_get_token_name(ctx);
		token_env = format_str("XDG_ACTIVATION_TOKEN=%s", token);
		if (token_env) {
			env[nenv++] = token_env;
		}
		if (!no_startup_id) {
			startup_env = format_str("DESKTOP_STARTUP_ID=%s", token);
			if (startup_env) {
				env[nenv++] = startup_env;
			}
		}
	}

	struct spawn_options opts = {
		.setsid = true,
		.wayland_fd = client ? sockets[1] : -1,
		.stdin_fd = -1,
		.env = env,
	};
	pid_t child = spawn_shell(cmd, &opts);
	free(token_env);
	free(startup_env);
	if (client) {
		close(sockets[1]);
	}
	if (child < 0) {
		launcher_ctx_destroy(ctx);
		if (client) {
			wl_client_destroy(client);
		}
		return -1;
	}

	if (client) {
		*privileged_client = client;
	}

	sway_log(SWAY_DEBUG, "Child process created with pid %d", child);
	if (ctx != NULL) {
		sway_log(SWAY_DEBUG, "Recording workspace for process %d", child);
		ctx->pid = child;
	}
	return child;
}

static struct cmd_results *exec_process(int argc, char **argv, bool privileged) {
	struct cmd_results *error = NULL;
	char *cmd = NULL;
	bool no_startup_id = false;
	if (strcmp(argv[0], "--no-startup-id") == 0) {
		no_startup_id = true;
		--argc; ++argv;
		if ((error = checkarg(argc, argv[-1], EXPECTED_AT_LEAST, 1))) {
			return error;
		}
	}

	if (argc == 1 && (argv[0][0] == '\'' || argv[0][0] == '"')) {
		cmd = strdup(argv[0]);
		strip_quotes(cmd);
	} else {
		cmd = join_args(argv, argc);
	}

	struct wl_client *client = NULL;
	pid_t child = exec_spawn(cmd, no_startup_id, privileged ? &client : NULL);
	free(cmd);
	if (child < 0) {
		return cmd_results_new(CMD_FAILURE, "fork() failed");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_exec_process(int argc, char **argv) {
	return exec_process(argc, argv, false);
}

struct cmd_results *cmd_exec_privileged_process(int argc, char **argv) {
	return exec_process(argc, argv, true);
}

struct cmd_results *cmd_exec_always(int argc, char **argv) {
	struct cmd_results *error;
	if ((error = cmd_exec_validate(argc, argv))) {
		return error;
	}
	return cmd_exec_process(argc, argv);
}
