/* OCA terminal interface. */
#ifndef OCA_TUI_H
#define OCA_TUI_H

#include "oca_core.h"

/* Startup-only display preferences; all fields contain boolean values. */
typedef struct {
    int show_reasoning;
    int show_read;
    int show_write;
    int show_edit;
    int show_bash;
} oca_tui_options;

#define OCA_TUI_DEFAULT_OPTIONS { .show_reasoning = 1, .show_write = 1, .show_edit = 1, .show_bash = 1 }

/* Runs the interactive terminal. The caller retains core and workspace.
 * A NULL options pointer selects OCA_TUI_DEFAULT_OPTIONS. */
int oca_tui_run(oca_core *core, const char *workspace, const char *initial_prompt,
                const oca_tui_options *options);

#endif
