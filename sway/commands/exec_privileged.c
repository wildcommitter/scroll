#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/log.h"
#include "stringop.h"

struct cmd_results *cmd_exec_privileged(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = cmd_exec_validate(argc, argv))) {
		return error;
	}
	// IPC clients must not hand out privileges. Bindings created over IPC
	// still can, so the allowlist only holds with -Dipc=false.
	if (config->ipc_command) {
		return cmd_results_new(CMD_INVALID,
			"exec_privileged is not allowed over IPC");
	}
	if (config->reloading) {
		char *args = join_args(argv, argc);
		sway_log(SWAY_DEBUG, "Ignoring 'exec_privileged %s' due to reload", args);
		free(args);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	return cmd_exec_privileged_process(argc, argv);
}
