#include "mcme/core.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "stb_ds.h"

/*
 * Thread pool.
 *
 * Used for the CPU-heavy background work: meshing chunks, loading regions,
 * running operations. Pairs with the event queue — a worker does the work,
 * then posts a result the main loop drains.
 *
 * Plain and blocking. No work stealing, no priorities, no cancellation tokens.
 * Jobs run to completion; if an operation needs to be stoppable it checks its
 * own flag (the original app does exactly this with op.stop).
 */

typedef struct {
    McmeFn fn;
    void  *userdata;
} Job;

struct McmeThreadPool {
    pthread_t       *threads;
    int              nthreads;

    pthread_mutex_t  mu;
    pthread_cond_t   work_cv;    /* signalled when work is posted or shutting down */
    pthread_cond_t   idle_cv;    /* signalled when the in-flight count hits zero */

    Job             *queue;      /* stb_ds dynamic array */
    size_t           inflight;   /* jobs queued but not yet finished */
    size_t           completed;  /* lifetime counter, for tests and progress */
    bool             shutdown;
};

static void *worker_main(void *arg)
{
    McmeThreadPool *tp = arg;

    for (;;) {
        pthread_mutex_lock(&tp->mu);
        while (arrlen(tp->queue) == 0 && !tp->shutdown) {
            pthread_cond_wait(&tp->work_cv, &tp->mu);
        }

        if (tp->shutdown && arrlen(tp->queue) == 0) {
            pthread_mutex_unlock(&tp->mu);
            return NULL;
        }

        /* Take the job. */
        Job job = tp->queue[0];
        arrdel(tp->queue, 0);
        pthread_mutex_unlock(&tp->mu);

        job.fn(job.userdata);

        pthread_mutex_lock(&tp->mu);
        tp->completed++;
        tp->inflight--;
        if (tp->inflight == 0) {
            pthread_cond_broadcast(&tp->idle_cv);
        }
        pthread_mutex_unlock(&tp->mu);
    }
}

McmeThreadPool *mcme_tp_new(int nthreads)
{
    if (nthreads <= 0) nthreads = mcme_cpu_count();

    McmeThreadPool *tp = calloc(1, sizeof(McmeThreadPool));
    if (!tp) return NULL;

    tp->nthreads = nthreads;
    tp->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!tp->threads) { free(tp); return NULL; }

    if (pthread_mutex_init(&tp->mu, NULL) != 0) { free(tp->threads); free(tp); return NULL; }
    if (pthread_cond_init(&tp->work_cv, NULL) != 0) {
        pthread_mutex_destroy(&tp->mu); free(tp->threads); free(tp); return NULL;
    }
    if (pthread_cond_init(&tp->idle_cv, NULL) != 0) {
        pthread_cond_destroy(&tp->work_cv);
        pthread_mutex_destroy(&tp->mu); free(tp->threads); free(tp); return NULL;
    }

    for (int i = 0; i < nthreads; ++i) {
        if (pthread_create(&tp->threads[i], NULL, worker_main, tp) != 0) {
            /* Degrade to however many we managed to start rather than fail. */
            tp->nthreads = i;
            break;
        }
    }

    if (tp->nthreads == 0) {
        mcme_set_error("thread pool: could not start any workers");
        mcme_tp_free(tp);
        return NULL;
    }
    return tp;
}

bool mcme_tp_submit(McmeThreadPool *tp, McmeFn fn, void *userdata)
{
    if (!tp || !fn) return false;

    pthread_mutex_lock(&tp->mu);
    if (tp->shutdown) {
        pthread_mutex_unlock(&tp->mu);
        mcme_set_error("thread pool: submit after shutdown");
        return false;
    }
    arrput(tp->queue, ((Job){ .fn = fn, .userdata = userdata }));
    tp->inflight++;
    pthread_cond_signal(&tp->work_cv);
    pthread_mutex_unlock(&tp->mu);
    return true;
}

void mcme_tp_wait(McmeThreadPool *tp)
{
    if (!tp) return;
    pthread_mutex_lock(&tp->mu);
    while (tp->inflight > 0) {
        pthread_cond_wait(&tp->idle_cv, &tp->mu);
    }
    pthread_mutex_unlock(&tp->mu);
}

size_t mcme_tp_completed(const McmeThreadPool *tp)
{
    if (!tp) return 0;
    McmeThreadPool *m = (McmeThreadPool *)tp;
    pthread_mutex_lock(&m->mu);
    size_t n = m->completed;
    pthread_mutex_unlock(&m->mu);
    return n;
}

void mcme_tp_free(McmeThreadPool *tp)
{
    if (!tp) return;

    pthread_mutex_lock(&tp->mu);
    tp->shutdown = true;
    pthread_cond_broadcast(&tp->work_cv);
    pthread_mutex_unlock(&tp->mu);

    for (int i = 0; i < tp->nthreads; ++i) {
        pthread_join(tp->threads[i], NULL);
    }

    arrfree(tp->queue);
    pthread_cond_destroy(&tp->idle_cv);
    pthread_cond_destroy(&tp->work_cv);
    pthread_mutex_destroy(&tp->mu);
    free(tp->threads);
    free(tp);
}
