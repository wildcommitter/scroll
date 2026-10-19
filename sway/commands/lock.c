#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/lock.h"
#include "stringop.h"

struct cmd_results *cmd_lock_command(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "lock_command", EXPECTED_AT_LEAST, 1))) {
		return error;
	}
	free(config->lock_command);
	if (argc == 1 && (argv[0][0] == '\'' || argv[0][0] == '"')) {
		config->lock_command = strdup(argv[0]);
		strip_quotes(config->lock_command);
	} else {
		config->lock_command = join_args(argv, argc);
	}
	if (!config->lock_command) {
		return cmd_results_new(CMD_FAILURE, "Unable to allocate lock_command");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_lock(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "lock", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	if (!config->lock_command) {
		return cmd_results_new(CMD_FAILURE, "No lock_command configured");
	}
	if (!sway_session_lock_request()) {
		return cmd_results_new(CMD_FAILURE, "Unable to start the locker");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
