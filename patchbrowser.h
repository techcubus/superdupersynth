/*
 * patchbrowser.h — public API for the patch file browser.
 *
 * The browser is a modal full-screen overlay.  It is sandboxed to the
 * patches/ directory and will not navigate above it.
 *
 * Two file formats are supported:
 *   .pjs    Rich format with name, author, comment metadata.
 *           Default for all saves.
 *   other   Legacy plain-text format (17 integers, one per line) matching
 *           the original C64 save convention.  Values are range-clamped on
 *           load to guard against corrupt or hand-edited files.
 */

#ifndef PATCHBROWSER_H
#define PATCHBROWSER_H

#include "patch.h"

/*
 * Create the patches/ directory if it does not exist.
 * Call once at program startup, before initscr / termw_init.
 */
void patchbrowser_init(void);

/*
 * Open the browser in LOAD mode.
 * Returns 1 and fills *p with the selected patch if the user chose a file.
 * Returns 0 if the user pressed ESC without loading anything.
 */
int patchbrowser_open_load(Patch *p);

/*
 * Open the browser in SAVE mode.
 * The user navigates to a directory and presses S to save *p there.
 * Returns 1 if the patch was saved, 0 if cancelled.
 */
int patchbrowser_open_save(const Patch *p);

#endif /* PATCHBROWSER_H */
