/*
 * One spec text — the block's name with its optional {SNBT states}
 * — parsed the way the CLI's verbs have always parsed it (G9): the
 * body moved here whole from src/app/main.c, only the verb left
 * behind (the callers wear their own). See resource.h.
 */

#include "mcme/resource.h"

#include <stdio.h>
#include <string.h>

bool mcme_block_parse_spec(const char *spec, McmeArena *arena,
                           char *name, size_t name_cap,
                           const McmeTag **states)
{
    const char *open;
    size_t spec_len, name_len;

    if (spec == NULL || name == NULL || name_cap == 0 ||
        states == NULL)
        return false;

    open = strchr(spec, '{');
    spec_len = strlen(spec);

    if (open == NULL) {
        name_len = spec_len;
        *states = NULL;
    } else {
        name_len = (size_t)(open - spec);
        if (spec_len == 0 || spec[spec_len - 1] != '}') {
            mcme_set_error("block '%s' has '{' without a closing '}'",
                           spec);
            return false;
        }
        McmeTag *tag = NULL;
        if (!mcme_nbt_parse_snbt(arena, open,
                                 spec_len - name_len, &tag)) {
            char why[256];
            snprintf(why, sizeof why, "%s", mcme_last_error());
            mcme_set_error("states of block '%s' do not parse: %s",
                           spec, why);
            return false;
        }
        if (tag == NULL || tag->type != MCME_TAG_Compound) {
            mcme_set_error("states of block '%s' are not a compound",
                           spec);
            return false;
        }
        *states = tag;
    }

    if (name_len == 0) {
        mcme_set_error("block '%s' has an empty name", spec);
        return false;
    }
    if (name_len >= name_cap) {
        mcme_set_error("block name '%s' is too long", spec);
        return false;
    }
    memcpy(name, spec, name_len);
    name[name_len] = '\0';
    return true;
}
