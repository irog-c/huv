#include "internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void drain_force_close(uv_timer_t *timer)
{
    huv_server_t *s = timer->data;
    huv_log(s, HUV_LOG_WARN,
            "shutdown drain timeout, force-closing %u remaining connection(s)",
            s->num_conns);
    huv_conn_t *c = s->conns_head;
    while (c) {
        huv_conn_t *next = c->next;
        huv_conn_close(c);
        c = next;
    }
    uv_timer_stop(&s->drain_timer);
    if (!uv_is_closing((uv_handle_t *)&s->drain_timer))
        uv_close((uv_handle_t *)&s->drain_timer, NULL);
}

static void start_drain(huv_server_t *s)
{
    if (s->shutting_down)
        return;
    s->shutting_down = true;
    huv_log(s, HUV_LOG_INFO, "shutting down (draining %u connection(s))",
            s->num_conns);

    if (s->listening && !uv_is_closing((uv_handle_t *)&s->listener)) {
        uv_close((uv_handle_t *)&s->listener, NULL);
        s->listening = false;
    }
    if (s->tls_listening && !uv_is_closing((uv_handle_t *)&s->tls_listener)) {
        uv_close((uv_handle_t *)&s->tls_listener, NULL);
        s->tls_listening = false;
    }
    if (s->signals_installed) {
        uv_signal_stop(&s->sigint);
        uv_signal_stop(&s->sigterm);
        if (!uv_is_closing((uv_handle_t *)&s->sigint))
            uv_close((uv_handle_t *)&s->sigint, NULL);
        if (!uv_is_closing((uv_handle_t *)&s->sigterm))
            uv_close((uv_handle_t *)&s->sigterm, NULL);
        s->signals_installed = false;
    }

    huv_conn_t *c = s->conns_head;
    while (c) {
        huv_conn_t *next = c->next;
        c->keep_alive = false;
        if (c->phase == PHASE_IDLE)
            huv_conn_close(c);
        c = next;
    }

    if (s->num_conns == 0)
        return;

    unsigned drain_ms = s->config.shutdown_timeout_ms;
    if (drain_ms) {
        if (!s->drain_timer_initialized) {
            uv_timer_init(&s->loop, &s->drain_timer);
            s->drain_timer.data = s;
            s->drain_timer_initialized = true;
        }
        uv_timer_start(&s->drain_timer, drain_force_close, drain_ms, 0);
    }
}

static void on_signal(uv_signal_t *handle, int signum)
{
    (void)signum;
    huv_server_t *s = handle->data;
    start_drain(s);
}

huv_server_t *huv_server_new(const huv_server_config_t *config)
{
    huv_server_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->config = config ? *config : HUV_SERVER_CONFIG_DEFAULT;
    if (!s->config.bind_addr)
        s->config.bind_addr = "0.0.0.0";
    if (s->config.max_body_bytes == 0)
        s->config.max_body_bytes = 1u << 20;
    /* Note: uv_loop_init is deferred to run_worker() so that fork() in
     * huv_server_run() can happen before any libuv state exists. */
    return s;
}

void huv_server_use(huv_server_t *s, huv_handler_fn middleware)
{
    if (s->num_middlewares < HUV_MAX_MIDDLEWARES)
        s->middlewares[s->num_middlewares++] = middleware;
}

/* Create a listening socket with SO_REUSEPORT + SO_REUSEADDR, then hand it
 * to libuv via uv_tcp_open. SO_REUSEPORT lets each worker bind its own
 * socket on the same port; the kernel load-balances incoming connections
 * across them. Harmless with a single worker. */
static int bind_listener(huv_server_t *s, uv_tcp_t *tcp, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        huv_log(s, HUV_LOG_ERROR, "socket(): %s", strerror(errno));
        return -1;
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) < 0) {
        huv_log(s, HUV_LOG_ERROR, "setsockopt: %s", strerror(errno));
        close(fd);
        return -1;
    }
    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, s->config.bind_addr, &sin.sin_addr) != 1) {
        huv_log(s, HUV_LOG_ERROR, "bad bind_addr: %s", s->config.bind_addr);
        close(fd);
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)&sin, sizeof sin) < 0) {
        huv_log(s, HUV_LOG_ERROR, "bind %s:%d: %s", s->config.bind_addr, port,
                strerror(errno));
        close(fd);
        return -1;
    }
    if (uv_tcp_init(&s->loop, tcp) != 0 || uv_tcp_open(tcp, fd) != 0) {
        huv_log(s, HUV_LOG_ERROR, "uv_tcp_init/open failed");
        close(fd);
        return -1;
    }
    return 0;
}

/* Single-process entry point. Initializes the loop, binds listener(s), runs
 * uv_run until shutdown. Called either directly (workers<=1) or in each
 * forked child (workers>1). */
static int run_worker(huv_server_t *s)
{
    if (uv_loop_init(&s->loop) != 0)
        return -1;
    s->loop_initialized = true;
    s->loop.data = s;

    bool tls_configured = s->config.tls_cert_path && s->config.tls_key_path;
    if (s->config.port == 0 && !tls_configured) {
        huv_log(s, HUV_LOG_ERROR,
                "no listeners: port=0 and TLS not configured");
        return -1;
    }

    if (s->config.port > 0) {
        if (bind_listener(s, &s->listener, s->config.port) != 0)
            return -1;
        s->listener.data = s;
        if (uv_listen((uv_stream_t *)&s->listener, 128, huv_conn_on_accept) !=
            0)
            return -1;
        s->listening = true;
    }

    if (tls_configured) {
        if (huv_tls_ctx_init(s) < 0) {
            huv_log(s, HUV_LOG_ERROR, "tls init failed; aborting");
            return -1;
        }
        if (bind_listener(s, &s->tls_listener, s->config.tls_port) != 0)
            return -1;
        s->tls_listener.data = s;
        if (uv_listen((uv_stream_t *)&s->tls_listener, 128,
                      huv_conn_on_accept_tls) != 0)
            return -1;
        s->tls_listening = true;
        huv_log(s, HUV_LOG_INFO, "tls listening on %s:%d", s->config.bind_addr,
                s->config.tls_port);
    }

    uv_signal_init(&s->loop, &s->sigint);
    s->sigint.data = s;
    uv_signal_start(&s->sigint, on_signal, SIGINT);
    uv_signal_init(&s->loop, &s->sigterm);
    s->sigterm.data = s;
    uv_signal_start(&s->sigterm, on_signal, SIGTERM);
    s->signals_installed = true;

    if (s->listening)
        huv_log(s, HUV_LOG_INFO,
                "worker pid=%d listening on %s:%d (max_connections=%u)",
                (int)getpid(), s->config.bind_addr, s->config.port,
                s->config.max_connections);
    uv_run(&s->loop, UV_RUN_DEFAULT);
    huv_log(s, HUV_LOG_INFO, "worker pid=%d stopped", (int)getpid());
    return 0;
}

/* Signal-safe child tracking for the master process. Only touched from
 * master context (single-threaded) and from the master's signal handler,
 * which only calls kill(2). */
#define MAX_WORKERS 128

/* Crash-loop guard: if a worker slot has exceeded this many abnormal exits
 * within RESPAWN_WINDOW_MS of the window start, the master stops respawning
 * it. The slot is retired — other workers keep serving, capacity just drops
 * for that slot. Values are deliberately conservative; a handler that
 * crashes this often is broken, not flappy. */
#define RESPAWN_MAX_IN_WINDOW 10u
#define RESPAWN_WINDOW_MS 60000u

typedef struct
{
    pid_t pid; /* current worker pid, -1 once the slot is done */
    unsigned restart_count;
    uint64_t window_start_ms;
    bool retired;
} worker_slot_t;

static volatile sig_atomic_t g_master_signaled = 0;
static worker_slot_t g_slots[MAX_WORKERS];
static unsigned g_num_slots = 0;

static void master_forward_signal(int sig)
{
    g_master_signaled = 1;
    for (unsigned i = 0; i < g_num_slots; i++) {
        if (g_slots[i].pid > 0)
            kill(g_slots[i].pid, sig);
    }
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Fork one worker. Child runs run_worker and never returns. Parent returns
 * the child's pid, or -1 on fork failure. */
static pid_t spawn_worker(huv_server_t *s)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int rc = run_worker(s);
        huv_server_free(s);
        _exit(rc == 0 ? 0 : 1);
    }
    return pid;
}

/* Master process: fork N workers, wait for all, forward SIGINT/SIGTERM,
 * optionally respawn workers that exit abnormally. */
static int run_master(huv_server_t *s, unsigned workers)
{
    if (workers > MAX_WORKERS)
        workers = MAX_WORKERS;
    g_num_slots = 0;

    /* HUV_RESPAWN=0 disables worker respawn regardless of cfg. Intended
     * for sanitizer / CI runs: when a worker crashes (SIGABRT from ASan,
     * exit code 23 from LSan, etc.) we want the master to propagate the
     * abnormal exit rather than silently respawn and return 0. */
    const char *respawn_env = getenv("HUV_RESPAWN");
    if (respawn_env &&
        (respawn_env[0] == '0' || strcasecmp(respawn_env, "false") == 0 ||
         strcasecmp(respawn_env, "off") == 0 ||
         strcasecmp(respawn_env, "no") == 0)) {
        s->config.respawn_workers = false;
    }

    uint64_t t0 = now_ms();
    for (unsigned i = 0; i < workers; i++) {
        pid_t pid = spawn_worker(s);
        if (pid < 0) {
            huv_log(s, HUV_LOG_ERROR, "fork: %s", strerror(errno));
            for (unsigned j = 0; j < g_num_slots; j++)
                if (g_slots[j].pid > 0)
                    kill(g_slots[j].pid, SIGTERM);
            for (unsigned j = 0; j < g_num_slots; j++)
                if (g_slots[j].pid > 0)
                    waitpid(g_slots[j].pid, NULL, 0);
            return -1;
        }
        g_slots[i].pid = pid;
        g_slots[i].restart_count = 0;
        g_slots[i].window_start_ms = t0;
        g_slots[i].retired = false;
        g_num_slots++;
    }

    huv_log(s, HUV_LOG_INFO, "master pid=%d spawned %u workers (respawn=%s)",
            (int)getpid(), workers, s->config.respawn_workers ? "on" : "off");

    struct sigaction sa = {0};
    sa.sa_handler = master_forward_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int any_fail = 0;
    unsigned alive = g_num_slots;
    while (alive > 0) {
        int status = 0;
        pid_t gone = waitpid(-1, &status, 0);
        if (gone < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        unsigned slot = g_num_slots;
        for (unsigned i = 0; i < g_num_slots; i++) {
            if (g_slots[i].pid == gone) {
                slot = i;
                break;
            }
        }
        if (slot == g_num_slots)
            continue; /* unknown pid, shouldn't happen */

        bool abnormal = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        bool should_respawn = abnormal && s->config.respawn_workers &&
                              !g_master_signaled && !g_slots[slot].retired;

        if (should_respawn) {
            uint64_t t = now_ms();
            if (t - g_slots[slot].window_start_ms > RESPAWN_WINDOW_MS) {
                g_slots[slot].window_start_ms = t;
                g_slots[slot].restart_count = 0;
            }
            g_slots[slot].restart_count++;
            if (g_slots[slot].restart_count > RESPAWN_MAX_IN_WINDOW) {
                huv_log(s, HUV_LOG_ERROR,
                        "worker slot %u: %u abnormal exits in %ums, "
                        "retiring slot",
                        slot, g_slots[slot].restart_count, RESPAWN_WINDOW_MS);
                g_slots[slot].pid = -1;
                g_slots[slot].retired = true;
                any_fail = 1;
                alive--;
                continue;
            }
            pid_t np = spawn_worker(s);
            if (np < 0) {
                huv_log(s, HUV_LOG_ERROR, "respawn fork failed for slot %u: %s",
                        slot, strerror(errno));
                g_slots[slot].pid = -1;
                any_fail = 1;
                alive--;
            } else {
                int info = WIFSIGNALED(status) ? WTERMSIG(status)
                                               : WEXITSTATUS(status);
                huv_log(s, HUV_LOG_WARN,
                        "worker slot %u: pid=%d exited (%s=%d), "
                        "respawned pid=%d (%u/%u in window)",
                        slot, (int)gone,
                        WIFSIGNALED(status) ? "signal" : "status", info,
                        (int)np, g_slots[slot].restart_count,
                        RESPAWN_MAX_IN_WINDOW);
                g_slots[slot].pid = np;
                /* alive unchanged: replacement took the slot. A successful
                 * respawn counts as recovered, so any_fail stays clear. */
            }
        } else {
            if (abnormal)
                any_fail = 1;
            g_slots[slot].pid = -1;
            alive--;
        }
    }
    huv_log(s, HUV_LOG_INFO, "master pid=%d: all workers exited",
            (int)getpid());
    return any_fail ? -1 : 0;
}

int huv_server_run(huv_server_t *s)
{
    unsigned workers = s->config.workers;
    if (workers <= 1)
        return run_worker(s);
    return run_master(s, workers);
}

static void walk_close_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle))
        uv_close(handle, NULL);
}

void huv_server_free(huv_server_t *s)
{
    if (!s)
        return;

    if (s->loop_initialized) {
        if (s->listening && !uv_is_closing((uv_handle_t *)&s->listener))
            uv_close((uv_handle_t *)&s->listener, NULL);
        if (s->tls_listening && !uv_is_closing((uv_handle_t *)&s->tls_listener))
            uv_close((uv_handle_t *)&s->tls_listener, NULL);
        if (s->signals_installed) {
            uv_signal_stop(&s->sigint);
            uv_signal_stop(&s->sigterm);
            if (!uv_is_closing((uv_handle_t *)&s->sigint))
                uv_close((uv_handle_t *)&s->sigint, NULL);
            if (!uv_is_closing((uv_handle_t *)&s->sigterm))
                uv_close((uv_handle_t *)&s->sigterm, NULL);
        }
        if (s->drain_timer_initialized &&
            !uv_is_closing((uv_handle_t *)&s->drain_timer))
            uv_close((uv_handle_t *)&s->drain_timer, NULL);

        uv_walk(&s->loop, walk_close_cb, NULL);
        uv_run(&s->loop, UV_RUN_DEFAULT);
        uv_loop_close(&s->loop);
    }

    huv_tls_ctx_free(s);
    free(s);
}
