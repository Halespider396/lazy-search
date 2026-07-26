#ifndef TUI_H
#define TUI_H

#include "package.h"

/* Runs the interactive fuzzy-search TUI over `list`. On selection,
 * writes "repo/name" into out_selection (for easy piping into
 * `pacman -S` / `yay -S` from a wrapper script) and returns 0.
 * Returns -1 if the user cancelled (Esc / Ctrl-C). */
int tui_run(PackageList *list, char *out_selection, size_t out_size);

#endif
