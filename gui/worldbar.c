/* The world bar's text half (G13): the parser the flags and the
 * entries share (one parse_coord, one range, one refusal shape —
 * the CLI's), and the bar's validation of its whole question.
 * Nothing here allocates: the refusals are static sentences, so
 * the caller points and the selftest's leak check stays clean —
 * except the dimension leg, which speaks the CORE's words for the
 * vocabulary core owns (G20): mcme_last_error()'s own buffer, the
 * one global's contract (G2), used immediately like every other
 * caller of it.
 */
#include "worldbar.h"

#include <stdlib.h>

#include "mcme/core.h"  /* mcme_last_error — the dimension leg's why */
#include "mcme/world.h" /* mcme_dimension_parse — the one vocabulary */

bool mcme_worldbar_coord(const char *text, long *out)
{
    char *end = NULL;
    long v;

    if (text == NULL || out == NULL) return false;
    v = strtol(text, &end, 10);
    if (end == text || *end != '\0') return false;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    *out = v;
    return true;
}

bool mcme_worldbar_ask(const char *world_dir,
                       const char *x_text, const char *y_text,
                       const char *z_text, const char *dim_text,
                       long *out_x, long *out_y, long *out_z,
                       int32_t *out_dim, const char **why)
{
    if (out_x == NULL || out_y == NULL || out_z == NULL ||
        out_dim == NULL || why == NULL)
        return false;
    if (world_dir == NULL || world_dir[0] == '\0') {
        *why = "pick a world first (the World… button)";
        return false;
    }
    if (!mcme_worldbar_coord(x_text, out_x)) {
        *why = "x is a whole coordinate";
        return false;
    }
    if (!mcme_worldbar_coord(y_text, out_y)) {
        *why = "y is a whole coordinate";
        return false;
    }
    if (!mcme_worldbar_coord(z_text, out_z)) {
        *why = "z is a whole coordinate";
        return false;
    }
    if (dim_text == NULL || dim_text[0] == '\0') {
        *out_dim = 0; /* the flag's absent: overworld */
    } else if (!mcme_dimension_parse(dim_text, out_dim)) {
        /* The core's own words for the vocabulary it owns (G20):
           the CLI's --dimension and this bar spell alike. */
        *why = mcme_last_error();
        return false;
    }
    *why = NULL;
    return true;
}
