#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"

struct cmd_results *cmd_privileged_clients(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "privileged_clients", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	if (strcasecmp(argv[0], "spawned") == 0) {
		config->privileged_clients = PRIVILEGED_CLIENTS_SPAWNED;
	} else if (strcasecmp(argv[0], "all") == 0) {
		config->privileged_clients = PRIVILEGED_CLIENTS_ALL;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected 'privileged_clients spawned|all'");
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
