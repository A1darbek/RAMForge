// main.c – parent process (no threads, no libuv, just forks workers)
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "cluster.h"

// ────────────────────────────────────────────────────────────────
// global configuration visible inside workers
unsigned g_aof_flush_ms = 10;           // 0  → appendfsync always
// ────────────────────────────────────────────────────────────────

// graceful shutdown flag (parent only)
static volatile int shutdown_requested = 0;

/* ──────────  signal handling  ────────── */
static void signal_handler(int sig)
{
    printf("\n🛑 Parent received signal %d, forwarding to cluster …\n", sig);
    shutdown_requested = 1;
}

static void setup_signal_handlers(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);      // Ctrl-C
    sigaction(SIGTERM, &sa, NULL);      // kill/terminate
}

/* ──────────  CLI parsing  ────────── */
static void parse_arguments(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--aof") == 0 && i + 1 < argc) {
            if (strcmp(argv[i + 1], "always") == 0) {
                g_aof_flush_ms = 0;
                printf("📝 AOF flush mode: ALWAYS (sync-every-write)\n");
            } else {
                printf("📝 Unknown --aof option “%s”, using default 10 ms\n",
                       argv[i + 1]);
            }
            i++;                        // skip value
        }
    }
}

/* ──────────  entry point  ────────── */
int main(int argc, char **argv)
{
    /*   force line-buffered stdout even when redirected               */
    setvbuf(stdout, NULL, _IOLBF, 0);

    parse_arguments(argc, argv);
    setup_signal_handlers();

    printf("🚀 RamForge parent – starting cluster only (heavy init in workers)\n");
    printf("   AOF flush interval: %s\n",
           g_aof_flush_ms == 0 ? "always" : "10 ms (default)");
    printf("   Port: 1109\n\n");

    /* forks workers & monitors them */
    int rc = start_cluster_with_args(1109, argc, argv);

    printf("👋 Parent exiting (cluster stopped) – status %d\n", rc);
    return rc;
}
