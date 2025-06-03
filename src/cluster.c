// cluster.c – forks, monitors and restarts worker processes
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

#include "cluster.h"
#include "slab_alloc.h"
#include "storage.h"
#include "persistence.h"
#include "app.h"
#include "app_routes.h"

/* configuration coming from main.c */
extern unsigned g_aof_flush_ms;

/* cluster-wide state (parent only) */
static volatile int  cluster_shutdown = 0;
static pid_t        *worker_pids      = NULL;
static int           worker_count     = 0;

static int detect_worker_target(int cli_argc, char **cli_argv)
{
    /* 1) CLI flag has top priority */
    for (int i = 1; i < cli_argc; i++) {
        if (strcmp(cli_argv[i], "--workers") == 0 && i + 1 < cli_argc) {
            return atoi(cli_argv[i + 1]);
        }
    }
    /* 2) environment variable */
    const char *env = getenv("RAMFORGE_WORKERS");
    if (env) return atoi(env);

    /* 3) default: number of CPUs */
    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    return cores < 1 ? 1 : cores;
}


/* ────────── helpers ────────── */

static void cluster_signal_handler(int sig)
{
    printf("🛑 Cluster manager caught signal %d – shutting down workers …\n", sig);
    cluster_shutdown = 1;

    for (int i = 0; i < worker_count; i++) {
        if (worker_pids[i] > 0) {
            printf("📤 SIGTERM → worker %d (PID %d)\n", i, worker_pids[i]);
            kill(worker_pids[i], SIGTERM);
        }
    }
}

static int setup_cpu_affinity(int worker_id)
{
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(worker_id, &cpus);

    if (sched_setaffinity(0, sizeof(cpus), &cpus) < 0) {
        perror("sched_setaffinity");
        return -1;
    }
    printf("⚙ Worker %d pinned to CPU core %d\n", worker_id, worker_id);
    return 0;
}

/* ────────── worker bootstrap ────────── */

static App *init_worker_systems(int worker_id)
{
    /* 1) allocator */
    slab_init();

    /* 2) storage */
    static Storage storage;
    storage_init(&storage);

    /* 3) persistence */
    const char *aof_file = "./append.aof";
    const char *dump_file = "./dump.rdb";

    if (worker_id == 0)
        printf("🔧 Using shared AOF: %s (all workers)\n", aof_file);

    Persistence_init(dump_file, aof_file, &storage, 60, g_aof_flush_ms);

    /* 4) application & routes */
    App *app = app_create(&storage);
    if (!app) {
        fprintf(stderr, "❌ Worker %d: app_create failed\n", worker_id);
        return NULL;
    }
    register_application_routes(app);

    printf("📋 Routes registered (worker %d)\n", worker_id);
    return app;
}

static void run_worker(int worker_id, int port)
{
    printf("🏃 Worker %d starting on port %d\n", worker_id, port);

    setup_cpu_affinity(worker_id);

    App *app = init_worker_systems(worker_id);
    if (!app) exit(1);

    /* workers handle SIGTERM gracefully via default handler */
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    printf("🚀 Worker %d ready – starting HTTP server …\n", worker_id);
    app->start(app, port);                 // will run uv_loop forever

    /* Cleanup (will run if start() returns or SIGTERM caught) */
    printf("🛑 Worker %d shutting down …\n", worker_id);
    if (app->shutdown) app->shutdown();
    printf("👋 Worker %d exit\n", worker_id);
    exit(0);
}

/* ────────── parent side helpers ────────── */

static void wait_for_workers(void)
{
    int remaining = worker_count;

    while (remaining > 0) {
        int   status;
        pid_t pid = wait(&status);

        if (pid > 0) {
            int wid = -1;
            for (int i = 0; i < worker_count; i++)
                if (worker_pids[i] == pid) { wid = i; break; }

            if (WIFEXITED(status))
                printf("✓ Worker %d (PID %d) exited code %d\n",
                       wid, pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                printf("⚠ Worker %d (PID %d) killed by signal %d\n",
                       wid, pid, WTERMSIG(status));
            remaining--;
        } else if (errno == EINTR) {
            continue;                      // interrupted by signal
        } else {
            perror("wait");
            break;
        }
    }
    printf("✓ All workers exited\n");
}

/* ────────── public API ────────── */

int start_cluster_with_args(int port, int argc, char **argv)
{
    worker_count = detect_worker_target(argc, argv);
    if (worker_count < 1) worker_count = 1;

    worker_pids = calloc(worker_count, sizeof *worker_pids);

    printf("🚀 Starting RamForge cluster with %d worker%s on port %d\n",
           worker_count, worker_count==1?"":"s", port);

    /* parent signal handler */
    struct sigaction sa = {0};
    sa.sa_handler = cluster_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* fork workers */
    for (int i = 0; i < worker_count; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) { run_worker(i, port); }
        worker_pids[i] = pid;
        printf("✓ Worker %d started (PID %d)\n", i, pid);
    }

    printf("\n🌟 All workers live – monitoring … (Ctrl-C to stop)\n\n");

    /* monitor & restart if not shutting down */
    while (!cluster_shutdown) {
        int   status;
        pid_t dead = waitpid(-1, &status, WNOHANG);

        if (dead > 0) {
            int wid = -1;
            for (int i = 0; i < worker_count; i++)
                if (worker_pids[i] == dead) { wid = i; break; }

            if (!cluster_shutdown) {
                printf("⚠ Worker %d (PID %d) died – restarting …\n", wid, dead);
                pid_t npid = fork();
                if (npid == 0) {
                    run_worker(wid, port);
                } else if (npid > 0) {
                    worker_pids[wid] = npid;
                    printf("✓ Worker %d restarted (PID %d)\n", wid, npid);
                } else {
                    perror("fork (restart)");
                }
            }
        } else if (dead == 0) {
            usleep(100000);               // 100 ms idle
        } else if (errno != ECHILD) {
            perror("waitpid");
        }
    }

    printf("🛑 Cluster shutting down – waiting for workers …\n");
    wait_for_workers();
    free(worker_pids);
    printf("✓ Cluster shutdown complete\n");
    return 0;
}
/* backward-compat wrapper: old callers without argv */
int start_cluster(int port) {
    return start_cluster_with_args(port, 0, NULL);
}
