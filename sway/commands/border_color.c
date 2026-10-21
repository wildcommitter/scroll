#include <string.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/tree/container.h"
#include "util.h"

// scroll-secure: border_color <#rrggbb[aa]>|none sets a per-window frame color
// that wins over every other color state (see container_get_current_colors).
// Meant for for_window rules on VM windows.
struct cmd_results *cmd_border_color(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "border_color", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}
	struct sway_container *container = config->handler_context.container;
	if (!container) {
		return cmd_results_new(CMD_INVALID,
			"Only valid containers can have a border_color");
	}
	if (strcmp(argv[0], "none") == 0) {
		container->border_color.set = false;
	} else {
		uint32_t color;
		if (!parse_color(argv[0], &color)) {
			return cmd_results_new(CMD_INVALID, "Invalid color %s", argv[0]);
		}
		color_to_rgba(container->border_color.color, color);
		container->border_color.set = true;
	}
	container_update(container);
	return cmd_results_new(CMD_SUCCESS, NULL);
}
