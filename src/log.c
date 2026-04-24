#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

void huv_log(huv_server_t *s, huv_log_level_t level, const char *fmt, ...)
{
    if (!s->config.log_cb)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s->config.log_cb(s->config.log_user, level, buf);
}

void huv_log_stderr(void *user, huv_log_level_t level, const char *msg)
{
    (void)user;
    const char *tag;
    switch (level) {
    case HUV_LOG_DEBUG:
        tag = "debug";
        break;
    case HUV_LOG_INFO:
        tag = "info";
        break;
    case HUV_LOG_WARN:
        tag = "warn";
        break;
    case HUV_LOG_ERROR:
        tag = "error";
        break;
    default:
        tag = "?";
        break;
    }
    /* One fprintf call → one write() on unbuffered stderr, so master and
     * worker lines interleave cleanly without tearing. The pid prefix lets
     * you tell them apart in multi-worker mode. */
    fprintf(stderr, "[pid=%d] http[%s] %s\n", (int)getpid(), tag, msg);
}
