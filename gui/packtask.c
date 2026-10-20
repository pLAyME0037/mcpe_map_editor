/*
 * See packtask.h. One worker per started task; it outlives the call
 * that started it (g_thread_unref right after creation — nobody joins,
 * the wire is the rendezvous), and the delivery runs on the loop
 * thread, which is the only thread allowed to touch a widget.
 */

#include "packtask.h"

#include <glib.h>

#include "mcme/resource.h" /* mcme_pack_open — the CLI's dispatch target */

typedef struct {
    McmeWire *wire;
    char *dir;
    McmePackDoneFn done;
    void *userdata;
} Job;

typedef struct {
    McmePackDoneFn done;
    void *userdata;
    McmePackResult *result;
} Delivery;

McmePackResult *mcme_pack_summarize(const char *dir)
{
    McmePackResult *r = g_new0(McmePackResult, 1);
    McmePack *pack = NULL;

    if (!mcme_pack_open(dir, &pack)) {
        /* The refusal, copied before anything else can write the
           core's one error slot: only this thread calls core code
           while the loop thread waits on the wire. */
        r->ok = false;
        r->text = g_strdup(mcme_last_error());
        return r;
    }

    /* The same four lines `mcme pack info` prints in its human shape
       (src/app/main.c cmd_pack_info); the --json half stays the CLI's
       own presentation, and the reading is one implementation either
       way — mcme_pack_open. */
    r->ok = true;
    r->text = g_strdup_printf("%s\n  pack_format  %d\n  description  %s"
                              "\n  pack.png     %s\n",
                              pack->root, (int)pack->format,
                              pack->description,
                              pack->has_icon ? "yes" : "no");
    mcme_pack_free(pack);
    return r;
}

void mcme_packresult_free(McmePackResult *result)
{
    if (result == NULL) return;

    g_free(result->text);
    g_free(result);
}

static void deliver(void *userdata)
{
    Delivery *d = userdata;

    d->done(d->userdata, d->result); /* done owns the result now */
    g_slice_free(Delivery, d);
}

static gpointer run_job(gpointer data)
{
    Job *job = data;
    Delivery *d = g_slice_new(Delivery);

    d->done = job->done;
    d->userdata = job->userdata;
    d->result = mcme_pack_summarize(job->dir);
    if (!mcme_wire_post(job->wire, deliver, d)) {
        mcme_packresult_free(d->result);
        g_slice_free(Delivery, d);
    }
    g_free(job->dir);
    g_slice_free(Job, job);
    return NULL;
}

bool mcme_packtask_start(McmeWire *wire, const char *dir,
                         McmePackDoneFn done, void *userdata)
{
    Job *job;
    GThread *t;

    if (wire == NULL || dir == NULL || done == NULL) return false;

    job = g_try_new0(Job, 1);
    if (job == NULL) return false;
    job->wire = wire;
    job->dir = g_strdup(dir);
    job->done = done;
    job->userdata = userdata;

    t = g_thread_new("pack-info", run_job, job);
    g_thread_unref(t); /* nobody joins: the wire is the rendezvous */
    return true;
}
