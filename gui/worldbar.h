/* The world bar's text half (G13): the entries' parsing and the ask's
 * own validation. Pure C — no GTK — so the GUI selftest pins what the
 * widgets feed the chain without initializing anything (that test's
 * standing rule: nothing has run GTK when the leak check stands).
 */
#ifndef MCME_GUI_WORLDBAR_H
#define MCME_GUI_WORLDBAR_H

#include <stdbool.h>
#include <stdint.h>

/* A coordinate as a whole block position — the CLI's parse_coord
 * shape, verbless (G9): the whole token consumed, inside int32's
 * range because the store's own keys are int32. Leading spaces are
 * strtol's own, and always were. NULL never parses. */
bool mcme_worldbar_coord(const char *text, long *out);

/* The bar's one question: a world directory and the whole
 * coordinates to ask it at — the dimension's number (an empty or
 * absent text is the flag's absent: overworld 0), answered through
 * the out-parameters. On refusal: false and a static sentence in
 * *why (owned here; point at it, never free it). */
bool mcme_worldbar_ask(const char *world_dir,
                       const char *x_text, const char *y_text,
                       const char *z_text, const char *dim_text,
                       long *out_x, long *out_y, long *out_z,
                       int32_t *out_dim, const char **why);

#endif /* MCME_GUI_WORLDBAR_H */
