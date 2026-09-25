/*
 * ecm_mailbox.c
 *
 * See ecm_mailbox.h for the design rationale. Summary of what runs
 * where:
 *
 *   RT thread             : ecm_mailbox_rt_pump_group() only (inline,
 *                           in the header) -- calls ecx_mbxhandler(),
 *                           which does the actual FPWR/FPRD socket I/O
 *                           for pending mailbox traffic.
 *   mailbox thread        : ecm_run.c's own mailbox_thread_fn() calls
 *                           ecm_mailbox_run(), which pops jobs and
 *                           calls ecx_SDOread()/ecx_SDOwrite() (these
 *                           never touch the socket once a slave is in
 *                           cyclic mode -- they push/pop SOEM's own
 *                           PI-mutex protected queue and block THIS
 *                           thread until the RT thread has pumped it).
 *                           This module does NOT spawn its own thread.
 *   caller thread(s)      : ecm_mailbox_sdo_read/write() -- build a
 *                           job, push it to our own queue, block on a
 *                           per-job condvar until the worker signals
 *                           completion.
 *
 * The job queue below (caller thread -> worker thread) is entirely
 * non-RT/non-RT: the RT thread never touches it. A plain mutex+cond
 * here does not violate master_plan_v2 Sec.2.5 ("no mutex between RT
 * and non-RT") -- that rule is specifically about the RT thread.
 */
#include "ecm_mailbox.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---- internal job type: never exposed in the public header ---- */

typedef enum {
    JOB_SDO_READ = 0,
    JOB_SDO_WRITE,
} job_op_t;

typedef struct ecm_mbx_job {
    job_op_t op;
    uint16_t slave;
    uint16_t index;
    uint8_t  subindex;
    bool     complete_access;
    int      timeout_us;

    uint8_t  data[ECM_MBX_MAX_DATA];
    int      size; /* in: buffer size for read / bytes to write for write
                     * out: bytes actually read (read only) */

    int      wkc;
    bool     failed; /* wkc <= 0 after the SOEM call */

    bool            done;
    pthread_mutex_t done_mtx;
    pthread_cond_t  done_cv;
} ecm_mbx_job_t;

/* ---- bounded job queue: caller thread(s) -> worker thread ---- */

typedef struct {
    ecm_mbx_job_t *slots[ECM_MBX_JOB_QUEUE_LEN];
    int             head;
    int             tail;
    int             count;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
} ecm_mbx_queue_t;

struct ecm_mailbox {
    ecx_contextt    *ctx;
    ecm_mbx_queue_t  queue;
    /* No thread handle and no running-flag of our own here: the thread
     * this runs on, and its stop flag, belong to ecm_run.c (see
     * ecm_mailbox_run()). */
};

/* ---------------- queue helpers ---------------- */

static void queue_init(ecm_mbx_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void queue_destroy(ecm_mbx_queue_t *q)
{
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
}

/* Returns 0 on success, -1 if the queue is full. */
static int queue_push(ecm_mbx_queue_t *q, ecm_mbx_job_t *job)
{
    int rc = 0;
    pthread_mutex_lock(&q->mtx);
    if (q->count >= ECM_MBX_JOB_QUEUE_LEN) {
        rc = -1;
    } else {
        q->slots[q->tail] = job;
        q->tail = (q->tail + 1) % ECM_MBX_JOB_QUEUE_LEN;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mtx);
    return rc;
}

/* Blocking pop used only by the mailbox thread. Wakes up at least
 * every 200ms even with nothing queued, so *stop_flag is checked
 * promptly and shutdown is timely without needing a dedicated
 * wake-on-shutdown signal. Returns NULL on a poll timeout (caller
 * re-checks *stop_flag and loops) or when told to stop.
 *
 * stop_flag is read here the same way ecm_run.c's own four threads
 * already read their g_*_stop flags (plain volatile sig_atomic_t,
 * written once from main() at shutdown, never written back) -- this
 * keeps the same convention as the rest of that file instead of
 * introducing a second, different synchronization style for just this
 * one thread. */
static ecm_mbx_job_t *queue_pop(ecm_mbx_queue_t *q, volatile sig_atomic_t *stop_flag)
{
    ecm_mbx_job_t *job = NULL;

    pthread_mutex_lock(&q->mtx);
    if (q->count == 0 && *stop_flag == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200L * 1000L * 1000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&q->not_empty, &q->mtx, &ts);
    }
    if (q->count > 0) {
        job = q->slots[q->head];
        q->head = (q->head + 1) % ECM_MBX_JOB_QUEUE_LEN;
        q->count--;
    }
    pthread_mutex_unlock(&q->mtx);
    return job;
}

/* ---------------- job execution (runs on the worker thread) ---------------- */

static void job_signal_done(ecm_mbx_job_t *job)
{
    pthread_mutex_lock(&job->done_mtx);
    job->done = true;
    pthread_cond_signal(&job->done_cv);
    pthread_mutex_unlock(&job->done_mtx);
}

static void execute_job(ecx_contextt *ctx, ecm_mbx_job_t *job)
{
    if (job->op == JOB_SDO_READ) {
        int size = job->size;
        job->wkc = ecx_SDOread(ctx, job->slave, job->index, job->subindex,
                                job->complete_access, &size, job->data,
                                job->timeout_us);
        job->size = size;
    } else {
        job->wkc = ecx_SDOwrite(ctx, job->slave, job->index, job->subindex,
                                 job->complete_access, job->size, job->data,
                                 job->timeout_us);
    }

    job->failed = (job->wkc <= 0);

    /* Deliberately NOT calling ecx_poperror()/ecx_iserror() here.
     * ecx_SDOread/write() push the decoded Abort code into
     * context->elist via ecx_SDOerror() on THIS (worker) thread, while
     * ecx_mbxhandler() running on the RT thread can push into the same
     * unlocked elist via ecx_mbxerror()/ecx_mbxemergencyerror(). Adding
     * a second concurrent reader/drainer here would not fix that
     * existing race and would only add a third party touching elist.
     * Draining elist is left to exactly one thread (the monitor /
     * giam-sat thread) by design -- see integration notes. */
}

void ecm_mailbox_run(ecm_mailbox_t *mbx, volatile sig_atomic_t *stop_flag)
{
    /* Deliberately NOT calling set_non_rt_thread() / touching scheduling
     * policy here: this function runs on ecm_run.c's own mailbox
     * thread, which already calls set_non_rt_thread() itself right
     * before invoking this function. Doing it a second time here would
     * duplicate, not strengthen, that guarantee. */
    while (*stop_flag == 0) {
        ecm_mbx_job_t *job = queue_pop(&mbx->queue, stop_flag);
        if (!job) {
            continue; /* poll timeout, re-check *stop_flag */
        }
        execute_job(mbx->ctx, job);
        job_signal_done(job);
    }
}

/* ---------------- public API: Part 1 -- enable cyclic mailbox ---------------- */

int ecm_mailbox_enable_cyclic(ecx_contextt *ctx)
{
    int enabled = 0;

    for (int slave = 1; slave <= ctx->slavecount; slave++) {
        if (ctx->slavelist[slave].state < EC_STATE_PRE_OP) {
            fprintf(stderr,
                "[ecm_mailbox] slave %d: state < PRE_OP, skipping "
                "cyclic mailbox enable (call this again once it is up)\n",
                slave);
            continue;
        }
        if (ecx_slavembxcyclic(ctx, slave)) {
            enabled++;
        } else {
            /* mbxstatus == 0 for this slave: no mailbox support
             * advertised in SII, or CoE bit not set. Not an error by
             * itself, but this slave must NEVER receive an SDO call
             * from ecm_mailbox_sdo_read/write() -- it would fall back
             * to SOEM's direct-socket mailbox path in ecx_mbxsend/
             * receive and race the RT thread for context->port. */
            fprintf(stderr,
                "[ecm_mailbox] slave %d: cyclic mailbox NOT enabled "
                "(mbxstatus=0) -- do not send SDO to this slave from "
                "the mailbox worker\n", slave);
        }
    }
    return enabled;
}

/* ---------------- public API: lifecycle ---------------- */

ecm_mailbox_t *ecm_mailbox_create(ecx_contextt *ctx)
{
    ecm_mailbox_t *mbx = calloc(1, sizeof(*mbx));
    if (!mbx) {
        return NULL;
    }
    mbx->ctx = ctx;
    queue_init(&mbx->queue);
    return mbx;
}

void ecm_mailbox_destroy(ecm_mailbox_t *mbx)
{
    if (!mbx) {
        return;
    }
    queue_destroy(&mbx->queue);
    free(mbx);
}

/* ---------------- public API: Part 3 (caller side) -- blocking SDO calls ---------------- */

static int submit_and_wait(ecm_mailbox_t *mbx, ecm_mbx_job_t *job)
{
    pthread_mutex_init(&job->done_mtx, NULL);
    pthread_cond_init(&job->done_cv, NULL);
    job->done = false;

    if (queue_push(&mbx->queue, job) != 0) {
        pthread_mutex_destroy(&job->done_mtx);
        pthread_cond_destroy(&job->done_cv);
        return -1; /* queue full */
    }

    /* Wait up to the SOEM-level timeout plus a generous safety margin.
     * This wait is ONLY a safety net against a stuck/dead worker -- in
     * the normal case ecx_SDOread/write() itself already returns by
     * timeout_us and the worker signals us right after. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long extra_ns = (long)job->timeout_us * 1000L + 500L * 1000L * 1000L;
    ts.tv_sec += extra_ns / 1000000000L;
    ts.tv_nsec += extra_ns % 1000000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    int rc = 0;
    pthread_mutex_lock(&job->done_mtx);
    while (!job->done && rc == 0) {
        rc = pthread_cond_timedwait(&job->done_cv, &job->done_mtx, &ts);
    }
    pthread_mutex_unlock(&job->done_mtx);

    pthread_mutex_destroy(&job->done_mtx);
    pthread_cond_destroy(&job->done_cv);

    return (rc == 0) ? 0 : -2;
}

int ecm_mailbox_sdo_read(ecm_mailbox_t *mbx, uint16_t slave, uint16_t index,
                          uint8_t subindex, bool complete_access,
                          uint8_t *out, int *inout_size, int timeout_us)
{
    if (*inout_size > ECM_MBX_MAX_DATA) {
        return -3;
    }

    ecm_mbx_job_t job;
    memset(&job, 0, sizeof(job));
    job.op              = JOB_SDO_READ;
    job.slave           = slave;
    job.index           = index;
    job.subindex        = subindex;
    job.complete_access = complete_access;
    job.timeout_us      = timeout_us;
    job.size            = *inout_size;

    int rc = submit_and_wait(mbx, &job);
    if (rc != 0) {
        return rc;
    }
    if (job.failed) {
        return -4;
    }

    *inout_size = job.size;
    memcpy(out, job.data, (size_t)job.size);
    return 0;
}

int ecm_mailbox_sdo_write(ecm_mailbox_t *mbx, uint16_t slave, uint16_t index,
                           uint8_t subindex, bool complete_access,
                           const uint8_t *data, int size, int timeout_us)
{
    if (size > ECM_MBX_MAX_DATA) {
        return -3;
    }

    ecm_mbx_job_t job;
    memset(&job, 0, sizeof(job));
    job.op              = JOB_SDO_WRITE;
    job.slave           = slave;
    job.index           = index;
    job.subindex        = subindex;
    job.complete_access = complete_access;
    job.timeout_us      = timeout_us;
    job.size            = size;
    memcpy(job.data, data, (size_t)size);

    int rc = submit_and_wait(mbx, &job);
    if (rc != 0) {
        return rc;
    }
    if (job.failed) {
        return -4;
    }
    return 0;
}