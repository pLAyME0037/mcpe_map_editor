/*
 * See task.h. One worker per started task; it outlives the call that
 * started it (g_thread_unref right after creation — nobody joins, the
 * wire is the rendezvous). On the impossible refusal (a live wire
 * cannot refuse: the core queue refuses only a NULL queue or
 * callback) the result is dropped with a line on stderr — running
 * `done` on the worker instead would put a widget call on the wrong
 * thread, which D6 forbids outright.
 */

#include "task.h"

#include <stdio.h>

#include <glib.h>

typedef struct {
    McmeWire *wire;
    McmeTaskWorkFn work;
    void *work_ud;
    McmeTaskDoneFn done;
    void *done_ud;
} Job;

typedef struct {
    McmeTaskDoneFn done;
    void *done_ud;
    void *result;
} Delivery;

static void deliver(void *userdata)
{
    Delivery *d = userdata;

    d->done(d->done_ud, d->result); /* done consumes the result now */
    g_slice_free(Delivery, d);
}

static gpointer run_job(gpointer data)
{
    Job *job = data;
    Delivery *d = g_slice_new(Delivery);
    void *result = job->work(job->work_ud);

    d->done = job->done;
    d->done_ud = job->done_ud;
    d->result = result;
    if (!mcme_wire_post(job->wire, deliver, d)) {
        fprintf(stderr, "mcgui: task result dropped: the wire refused\n");
        g_slice_free(Delivery, d);
    }
    g_slice_free(Job, job);
    return NULL;
}

bool mcme_task_start(McmeWire *wire, McmeTaskWorkFn work, void *work_ud,
                     McmeTaskDoneFn done, void *done_ud)
{
    Job *job;
    GThread *t;

    if (wire == NULL || work == NULL || done == NULL) return false;

    job = g_try_new0(Job, 1);
    if (job == NULL) return false;
    job->wire = wire;
    job->work = work;
    job->work_ud = work_ud;
    job->done = done;
    job->done_ud = done_ud;

    t = g_thread_new("mcme-task", run_job, job);
    g_thread_unref(t); /* nobody joins: the wire is the rendezvous */
    return true;
}
