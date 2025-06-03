/* aof_batch.c – append-only log with batching + CRC32C + rewrite */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdio.h>
#include <errno.h>

#include "aof_batch.h"
#include "storage.h"
#include "crc32c.h"

/* ─── configuration ───────────────────────────── */
#define DEFAULT_RING_CAP (1 << 15)            /* 32 k entries */

/* ─── types / globals ─────────────────────────── */
typedef struct { int id; uint32_t sz; void *data; } aof_cmd_t;

static aof_cmd_t     *ring;
static size_t         cap, mask, head, tail;
static pthread_mutex_t lock;
static pthread_cond_t  cond;

static int            fd = -1;
static char          *g_path = NULL;          /* remember for rewrite */
static unsigned       flush_ms = 10;
static int            mode_always = 0;
static pthread_t      writer;
static int            running = 0;

/* ─── CRC helper ──────────────────────────────── */
static int safe_write(int fd, const void *buf, size_t len)
{
    ssize_t n = write(fd, buf, len);
    return (n == (ssize_t)len) ? 0 : -1;        /* errno already set */
}

int aof_write_record(int fd, int id,
                     const void *data, uint32_t size)
{
    if (safe_write(fd,&id,4)         ||
        safe_write(fd,&size,4)       ||
        safe_write(fd,data,size))
        return -1;

    uint32_t crc = crc32c(0,&id,4);
    crc = crc32c(crc,&size,4);
    crc = crc32c(crc,data,size);
    return safe_write(fd,&crc,4);
}


/* ─── background writer (batch mode) ─────────── */
static void *writer_thread(void *arg)
{
    (void)arg;
    while (running) {
        pthread_mutex_lock(&lock);
        while (head == tail && running)
            pthread_cond_wait(&cond, &lock);

        while (head != tail) {
            aof_cmd_t *c = &ring[tail];
            aof_write_record(fd, c->id, c->data, c->sz);
            free(c->data);
            tail = (tail + 1) & mask;
        }
        fsync(fd);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&lock);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += flush_ms * 1000000ULL;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }

        pthread_mutex_lock(&lock);
        pthread_cond_timedwait(&cond, &lock, &ts);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* ─── public API ─────────────────────────────── */
void AOF_init(const char *path,
              size_t      ring_capacity,
              unsigned    interval_ms)
{
    mode_always = (interval_ms == 0);
    flush_ms    = mode_always ? 1000 : interval_ms;
    g_path      = strdup(path);

    if (!mode_always) {
        cap = 1; while (cap < ring_capacity) cap <<= 1;
        mask = cap - 1;
        ring = calloc(cap, sizeof *ring);
        pthread_mutex_init(&lock, NULL);
        pthread_cond_init(&cond, NULL);
    }

    fd = open(path, O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) { perror("AOF_init/open"); exit(1); }

    if (!mode_always) {
        running = 1;
        if (pthread_create(&writer, NULL, writer_thread, NULL)) {
            perror("pthread_create"); exit(1);
        }
    }
}

int AOF_append(int id, const void *data, size_t size) {
    if (mode_always) {
        if (aof_write_record(fd, id, data, (uint32_t) size) == -1)
            return -1;
        fsync(fd);
        return 0;
    }

    void *copy = malloc(size);
    memcpy(copy, data, size);

    pthread_mutex_lock(&lock);
    size_t nxt = (head + 1) & mask;
    while (nxt == tail) {
        pthread_cond_wait(&cond, &lock);
        nxt = (head + 1) & mask;
    }
    ring[head] = (aof_cmd_t) {id, (uint32_t) size, copy};
    head = nxt;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    return 0;
}

/* replay - FIXED: open separate read fd */
void AOF_load(Storage *st)
{
    if (!g_path) return;  // No path set

    // Open separate read-only fd for loading
    int read_fd = open(g_path, O_RDONLY | O_CLOEXEC);
    if (read_fd < 0) {
        if (errno == ENOENT) return;  // File doesn't exist yet, that's OK
        perror("AOF_load/open");
        return;
    }

    int id; uint32_t size, crc_file;
    while (read(read_fd, &id, 4) == 4) {
        if (read(read_fd, &size, 4) != 4) goto corrupt;

        void *buf = malloc(size);
        if (read(read_fd, buf, size) != (ssize_t)size) goto corrupt;

        if (read(read_fd, &crc_file, 4) != 4) goto corrupt;

        uint32_t crc = crc32c(0, &id, 4);
        crc = crc32c(crc, &size, 4);
        crc = crc32c(crc, buf, size);
        if (crc != crc_file) goto corrupt;

        storage_save(st, id, buf, size);
        free(buf);
    }
    close(read_fd);
    return;

    corrupt:
    fprintf(stderr, "❌ AOF corrupt at offset %#lx – aborting\n",
            (unsigned long)lseek(read_fd, 0, SEEK_CUR));
    close(read_fd);
    exit(2);
}

static void dump_record_cb(int id, const void *data, size_t sz, void *ud)
{
    int fd = (int)(intptr_t)ud;
    aof_write_record(fd, id, data, (uint32_t)sz);
}

/* ─── AOF rewrite (compaction) ──────────────── */
void AOF_rewrite(Storage *st)
{
    printf("🔄 Compacting AOF …\n");

    /* 1) dump current state into tmp file */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    int fd_tmp = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd_tmp < 0) { perror("open tmp"); return; }

    if (mode_always) {
        /* In always mode, reload from AOF to get complete state */
        Storage temp_st;
        storage_init(&temp_st);
        AOF_load(&temp_st);
        storage_iterate(&temp_st, dump_record_cb, (void*)(intptr_t)fd_tmp);
        storage_destroy(&temp_st);
    } else {
        /* In batch mode, memory state is authoritative */
        storage_iterate(st, dump_record_cb, (void*)(intptr_t)fd_tmp);
    }

    fsync(fd_tmp);
    close(fd_tmp);

    /* 2) pause writer (batch mode) / close fd (always mode) */
    if (!mode_always) {
        pthread_mutex_lock(&lock);
        while (head != tail) {                    /* flush queue first */
            aof_cmd_t *c = &ring[tail];
            aof_write_record(fd, c->id, c->data, c->sz);
            free(c->data); tail = (tail + 1) & mask;
        }
        fsync(fd);
    }
    close(fd);

    /* 3) atomically replace */
    if (rename(tmp, g_path) != 0) perror("rename");

    /* 4) reopen append fd */
    fd = open(g_path, O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) { perror("re-open AOF"); exit(1); }

    if (!mode_always) {
        pthread_mutex_unlock(&lock);
    }
    printf("✓ AOF rewrite complete\n");
}

/* ─── shutdown ─────────────────────────────── */
void AOF_shutdown(void)
{
    if (mode_always) { if (fd!=-1) close(fd); return; }

    running = 0;
    pthread_cond_signal(&cond);
    pthread_join(writer, NULL);

    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&cond);
    free(ring);

    if (fd!=-1) close(fd);
}