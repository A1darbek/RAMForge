// src/persistence.c  –  RDB + AOF with CRC-32C footer verification
#define _POSIX_C_SOURCE 200809L
#include "persistence.h"
#include "aof_batch.h"
#include "storage.h"
#include "crc32c.h"              /* NEW */

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static char      *g_rdb_path;
static Storage   *g_storage;
static uv_timer_t g_snapshot_timer;

/* ──────────────────────────────────────────────────────────── */
/* 1.   Streaming iterator that updates CRC while dumping      */
static void rdb_iter_crc_cb(int id,
                            const void *data,
                            size_t      size,
                            void       *ud)
{
    struct { FILE *f; uint32_t *crc; } *ctx = ud;

    fwrite(&id,   sizeof id,   1, ctx->f);
    fwrite(&size, sizeof size, 1, ctx->f);
    fwrite(data,  size,        1, ctx->f);

    *ctx->crc = crc32c(*ctx->crc, &id,   sizeof id);
    *ctx->crc = crc32c(*ctx->crc, &size, sizeof size);
    *ctx->crc = crc32c(*ctx->crc,  data, size);
}

static void storage_iterate_crc(Storage *st, FILE *out, uint32_t *crc)
{
    *crc = 0;
    struct { FILE *f; uint32_t *crc; } ctx = { out, crc };
    storage_iterate(st, rdb_iter_crc_cb, &ctx);
}

/* ──────────────────────────────────────────────────────────── */
/* 2.   Load RDB on startup – verify footer CRC                */
static void load_rdb(Storage *st)
{
    FILE *rdb = fopen(g_rdb_path, "rb");
    if (!rdb) return;

    /* read footer */
    fseek(rdb, 0, SEEK_END);
    long fsz = ftell(rdb);
    if (fsz < 4) { fclose(rdb); return; }

    fseek(rdb, fsz - 4, SEEK_SET);
    uint32_t crc_file; fread(&crc_file, 4, 1, rdb);
    rewind(rdb);

    uint32_t crc = 0;
    while (ftell(rdb) < fsz - 4) {
        int    id;
        size_t sz;

        if (fread(&id, sizeof id, 1, rdb) != 1) goto corrupt;
        if (fread(&sz, sizeof sz, 1, rdb) != 1) goto corrupt;

        void *buf = malloc(sz);
        if (fread(buf, sz, 1, rdb) != 1) { free(buf); goto corrupt; }

        crc = crc32c(crc, &id, sizeof id);
        crc = crc32c(crc, &sz, sizeof sz);
        crc = crc32c(crc,  buf, sz);

        storage_save(st, id, buf, sz);
        free(buf);
    }

    if (crc != crc_file) goto corrupt;
    fclose(rdb);
    return;

    corrupt:
    fprintf(stderr,
            "❌ RDB checksum mismatch, aborting startup (computed %#x ≠ %#x)\n",
            crc, crc_file);
    exit(2);
}

/* ──────────────────────────────────────────────────────────── */
/* 3.   Periodic forked snapshot – dump + CRC footer           */
static void snapshot_cb(uv_timer_t *t)
{
    (void)t;
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }

    if (pid == 0) {                              /* child */
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_rdb_path);

        FILE *out = fopen(tmp, "wb");
        if (!out) _exit(1);

        uint32_t crc;
        storage_iterate_crc(g_storage, out, &crc);
        fwrite(&crc, 4, 1, out);                 /* footer */

        fflush(out); fsync(fileno(out));
        fclose(out);

        rename(tmp, g_rdb_path);
        _exit(0);
    }
    /* parent: reap immediately (non-blocking) */
    waitpid(pid, NULL, WNOHANG);
}

/* ──────────────────────────────────────────────────────────── */
void Persistence_init(const char *rdb_path,
                      const char *aof_path,
                      Storage    *storage,
                      unsigned    snapshot_interval_sec,
                      unsigned    aof_flush_ms)
{
    g_rdb_path = strdup(rdb_path);
    g_storage  = storage;

    /* 1) Restore from snapshot */
    load_rdb(storage);

    /* 2) Start AOF engine & replay */
    AOF_init(aof_path, 1 << 16, aof_flush_ms);
    AOF_load(storage);

    /* 3) Periodic snapshot timer (in each worker) */
    uv_timer_init(uv_default_loop(), &g_snapshot_timer);
    uv_timer_start(&g_snapshot_timer,
                   snapshot_cb,
                   snapshot_interval_sec * 1000,
                   snapshot_interval_sec * 1000);
}

/* ──────────────────────────────────────────────────────────── */
void Persistence_shutdown(void)
{
    AOF_shutdown();
}

/* compact invoked via /admin/compact handler */
void Persistence_compact(void)
{
    /* 1) synchronous RDB rewrite */
    char tmp_rdb[512];
    snprintf(tmp_rdb, sizeof tmp_rdb, "%s.tmp", g_rdb_path);

    FILE *out = fopen(tmp_rdb, "wb");
    if (out) {
        uint32_t crc;
        storage_iterate_crc(g_storage, out, &crc);
        fwrite(&crc, 4, 1, out);
        fflush(out); fsync(fileno(out));
        fclose(out);
        rename(tmp_rdb, g_rdb_path);
    }

    /* 2) AOF rewrite */
    AOF_rewrite(g_storage);
}
