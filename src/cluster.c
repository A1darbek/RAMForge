// cluster.c – forks, monitors and (optionally) restarts worker processes
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

/* configuration exported by main.c */
extern unsigned g_aof_flush_ms;

/* parent-only state */
static volatile int  cluster_shutdown = 0;
static pid_t        *worker_pids      = NULL;
static int           worker_count     = 0;

/* ────────── CLI / ENV helpers ────────── */
static int detect_worker_target(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i],"--workers") && i+1<argc)
            return atoi(argv[i+1]);

    const char *env = getenv("RAMFORGE_WORKERS");
    if (env) return atoi(env);

    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    return cores < 1 ? 1 : cores;
}

/* ────────── parent signal handler ────────── */
static void cluster_signal_handler(int sig)
{
    printf("🛑 Cluster manager caught signal %d – shutting down workers …\n", sig);
    cluster_shutdown = 1;
    for (int i=0;i<worker_count;i++)
        if (worker_pids[i] > 0) {
            printf("📤 SIGTERM → worker %d (PID %d)\n", i, worker_pids[i]);
            kill(worker_pids[i], SIGTERM);
        }
}

/* ────────── CPU pin helper (worker) ────────── */
static void setup_cpu_affinity(int wid)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(wid, &set);
    if (sched_setaffinity(0,sizeof set,&set))
        perror("sched_setaffinity");
    else
        printf("⚙ Worker %d pinned to CPU core %d\n", wid, wid);
}

/* ────────── worker bootstrap ────────── */
static App* init_worker_systems(int wid)
{
    slab_init();
    static Storage storage;
    storage_init(&storage);

    const char *aof  = "./append.aof";
    const char *dump = "./dump.rdb";
    if (wid==0) printf("🔧 Using shared AOF: %s (all workers)\n", aof);
    Persistence_init(dump, aof, &storage, 60, g_aof_flush_ms);

    App *app = app_create(&storage);
    if (!app) { fprintf(stderr,"❌ app_create failed\n"); return NULL; }
    register_application_routes(app);

    printf("📋 Routes registered (worker %d)\n", wid);
    return app;
}

static void run_worker(int wid,int port)
{
    printf("🏃 Worker %d starting on port %d\n", wid, port);
    setup_cpu_affinity(wid);

    App *app = init_worker_systems(wid);
    if (!app) exit(1);

    struct sigaction sa={0}; sa.sa_handler=SIG_DFL; sigaction(SIGTERM,&sa,NULL);

    printf("🚀 Worker %d ready – starting HTTP server …\n", wid);
    app->start(app, port);                /* blocks until exit */

    /* graceful path (rare) */
    if (app->shutdown) app->shutdown();
    exit(0);
}

/* ────────── parent wait-helper ────────── */
static void wait_for_workers(void)
{
    int left = worker_count;
    while (left > 0) {
        int st; pid_t pid = wait(&st);
        if (pid > 0) {
            int wid=-1; for(int i=0;i<worker_count;i++) if(worker_pids[i]==pid) {wid=i;break;}
            if (WIFEXITED(st))
                printf("✓ Worker %d (PID %d) exited code %d\n", wid,pid,WEXITSTATUS(st));
            else if (WIFSIGNALED(st))
                printf("⚠ Worker %d (PID %d) killed by signal %d\n", wid,pid,WTERMSIG(st));
            left--;
        } else if (errno==EINTR) continue; else break;
    }
    printf("✓ All workers exited\n");
}

/* ────────── PUBLIC API ────────── */

int start_cluster_with_args(int port,int argc,char **argv)
{
    worker_count = detect_worker_target(argc, argv);
    /* NEW — if caller requests 0 workers, run a single worker in-process */
    if (worker_count == 0) {
        printf("🚀 Single-process mode (no cluster manager)\n");
        run_worker(0, port);          /* run_worker never returns */
        return 0;                     /* not reached, but keeps compiler happy */
    }
    worker_pids = calloc(worker_count,sizeof *worker_pids);

    printf("🚀 Starting RamForge cluster with %d worker%s on port %d\n",
           worker_count, worker_count==1?"":"s", port);

    struct sigaction sa={0}; sa.sa_handler=cluster_signal_handler;
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    /* fork initial workers */
    for(int i=0;i<worker_count;i++){
        pid_t pid=fork();
        if(pid<0){perror("fork");exit(1);}
        if(pid==0) run_worker(i,port);
        worker_pids[i]=pid;
        printf("✓ Worker %d started (PID %d)\n",i,pid);
    }

    printf("\n🌟 All workers live – monitoring … (Ctrl-C to stop)\n\n");

    /* monitor loop */
    while(!cluster_shutdown){
        int st; pid_t dead=waitpid(-1,&st,WNOHANG);

        if(dead>0){
            int wid=-1; for(int i=0;i<worker_count;i++) if(worker_pids[i]==dead){wid=i;break;}

            /* decide whether to restart or stop */
            int fatal = (WIFEXITED(st) && WEXITSTATUS(st)!=0) ||
                        WIFSIGNALED(st);

            if (fatal) {
                printf("‼︎ Worker %d (PID %d) exited abnormally – shutting cluster down\n",
                       wid, dead);
                cluster_shutdown = 1;
            } else {
                printf("✓ Worker %d exited normally – stopping cluster\n", wid);
                cluster_shutdown = 1;
            }
        } else if (dead==0) {
            usleep(100000); /* idle 100 ms */
        } else if(errno!=ECHILD){
            perror("waitpid"); break;
        }
    }

    printf("🛑 Cluster shutting down – waiting for workers …\n");
    wait_for_workers();
    free(worker_pids);
    printf("✓ Cluster shutdown complete\n");
    return 0;
}

int start_cluster(int port){ return start_cluster_with_args(port,0,NULL); }
