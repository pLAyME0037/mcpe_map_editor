/*
 * The GUI's own checks — headless, GLib only: no display, no window, no
 * gtk_init. `./mcgui --selftest` runs them and exits on the CLI's code
 * convention (0 pass, 1 fail), so a script can gate on them like any
 * other verb (wiki/decisions.md#D3).
 */
#ifndef MCME_GUI_SELFTEST_H
#define MCME_GUI_SELFTEST_H

int mcme_gui_selftest(void);

#endif
