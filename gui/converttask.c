/*
 * See converttask.h. Same shape as filltask over gui/task.c: the
 * worker owns its arguments, the result crosses as data, the done
 * callback runs on the loop thread.
 */

#include "converttask.h"

#include <string.h>

#include <glib.h>

#include "mcme/bedrock_write.h" /* McmeWriteRefusal */
#include "mcme/convert.h"       /* mcme_convert_bedrock_java, the drops */
#include "mcme/core.h"          /* mcme_last_error — one global, copied now */
#include "mcme/world.h"         /* the probe, the kind's own name */
#include "task.h"

void mcme_convertresult_free(McmeConvertResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    g_free(result->out_dir);
    g_free(result);
}

typedef struct {
    char *world_dir;
} ConvertWork;

typedef struct {
    McmeConvertDoneFn fn;
    void *userdata;
} ConvertDone;

/* The refusal, in the core's own words when it spoke — copied now,
   the one global's contract (G2), the ask's same fallback. */
static char *refuse_text(void)
{
    char *t = g_strdup(mcme_last_error());

    if (t != NULL && t[0] != '\0') return t;
    g_free(t);
    return g_strdup("the world's store would not answer");
}

/* The output's name: the input trimmed of its trailing slashes with
   "_java" beside it — a sibling by construction, so the CLI's "an
   output world is written outside its input" rule cannot be broken
   by any path the hand gives. */
static char *derive_out(const char *in)
{
    size_t n = strlen(in);
    char *out;

    while (n > 0 && in[n - 1] == '/') --n;
    out = g_malloc(n + sizeof "_java");
    memcpy(out, in, n);
    memcpy(out + n, "_java", sizeof "_java"); /* the NUL rides */
    return out;
}

static void *convert_work(void *data)
{
    ConvertWork *w = data;
    McmeConvertResult *r = g_new0(McmeConvertResult, 1);
    McmeWorldInfo info;
    McmeWriteRefusal refusal = MCME_WRITE_REFUSAL_NONE;

    /* The probe speaks for the directory first — the door's own
       words before anything looks at a store. */
    if (!mcme_world_probe(&info, w->world_dir)) {
        r->text = refuse_text();
        goto out;
    }
    if (info.kind == MCME_WORLD_JAVA) {
        /* cmd_convert's verdict on the pair, in its own words: the
           sentence the verb speaks for a Java input, the kind name
           the core spells (G19's door — one truth, no guess). */
        r->text = g_strdup_printf(
            "'%s' is a %s world; only Bedrock input converts today",
            w->world_dir, mcme_world_kind_name(info.kind));
        mcme_world_info_free(&info);
        goto out;
    }
    if (info.kind != MCME_WORLD_BEDROCK || info.db_path == NULL) {
        r->text = g_strdup("that directory is not a world mcme can "
                           "write");
        mcme_world_info_free(&info);
        goto out;
    }
    mcme_world_info_free(&info);

    r->out_dir = derive_out(w->world_dir);
    if (!mcme_convert_bedrock_java(w->world_dir, r->out_dir,
                                   &r->report, &refusal)) {
        r->text = refuse_text();
        g_free(r->out_dir);
        r->out_dir = NULL;
        goto out;
    }
    r->ok = true;

out:
    g_free(w->world_dir);
    g_free(w);
    return r;
}

static void convert_deliver(void *done_ud, void *result)
{
    ConvertDone *d = done_ud;

    d->fn(d->userdata, (McmeConvertResult *)result);
    g_free(d);
}

bool mcme_converttask_start(McmeWire *wire, const char *world_dir,
                            McmeConvertDoneFn done, void *userdata)
{
    ConvertWork *w;
    ConvertDone *d;

    if (wire == NULL || world_dir == NULL || done == NULL)
        return false;

    w = g_try_new0(ConvertWork, 1);
    d = g_try_new0(ConvertDone, 1);
    if (w == NULL || d == NULL) {
        g_free(w);
        g_free(d);
        return false;
    }
    w->world_dir = g_strdup(world_dir);
    d->fn = done;
    d->userdata = userdata;

    if (!mcme_task_start(wire, convert_work, w, convert_deliver, d)) {
        g_free(w->world_dir);
        g_free(w);
        g_free(d);
        return false;
    }
    return true;
}
