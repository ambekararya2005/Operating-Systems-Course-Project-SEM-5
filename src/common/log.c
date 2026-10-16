/*
 * OS concept: Mutual exclusion (pthread mutex protecting shared output streams) — Unit 3
 */
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX 1024

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *g_component = "relief";
static log_level_t g_min_level = LOG_LVL_INFO;
static FILE *g_file = NULL;

static const char *const g_level_upper[] = { "DEBUG", "INFO", "WARN", "ERROR" };
static const char *const g_level_lower[] = { "debug", "info", "warn", "error" };

void log_init(const char *component)
{
    pthread_mutex_lock(&g_log_lock);
    g_component = component;
    pthread_mutex_unlock(&g_log_lock);
}

void log_set_level(log_level_t level)
{
    pthread_mutex_lock(&g_log_lock);
    g_min_level = level;
    pthread_mutex_unlock(&g_log_lock);
}

int log_open_file(const char *path)
{
    FILE *f = fopen(path, "ae");    /* "e" = O_CLOEXEC: not inherited by exec'd camps */
    if (!f)
        return -1;
    pthread_mutex_lock(&g_log_lock);
    if (g_file)
        fclose(g_file);
    g_file = f;
    pthread_mutex_unlock(&g_log_lock);
    return 0;
}

void log_close_file(void)
{
    pthread_mutex_lock(&g_log_lock);
    if (g_file)
        fclose(g_file);
    g_file = NULL;
    pthread_mutex_unlock(&g_log_lock);
}

static void timestamp(char *buf, size_t len)
{
    struct timespec ts;
    struct tm tm;
    char sec[24];

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(sec, sizeof sec, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf, len, "%s.%03d", sec, (int)(ts.tv_nsec / 1000000L) % 1000);
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

/* Write one line to both sinks. Caller holds g_log_lock.
 * The stderr line is formatted first and emitted with a single write(2): reliefd and all
 * camps share one O_APPEND log file, and one write per line keeps their lines from
 * interleaving (stdio may split an unbuffered fprintf into several writes). */
static void emit_locked(log_level_t level, const char *stamp, const char *human, const char *kv)
{
    char line[LOG_LINE_MAX + 128];
    int n = snprintf(line, sizeof line, "%s %-5s %s[%d] %s\n", stamp, g_level_upper[level],
                     g_component, (int)getpid(), human);

    if (n < 0)
        return;
    if ((size_t)n >= sizeof line) {     /* truncated: keep the newline */
        n = (int)sizeof line - 1;
        line[n - 1] = '\n';
    }
    write_all(STDERR_FILENO, line, (size_t)n);
    if (g_file) {
        fprintf(g_file, "ts=%s level=%s comp=%s pid=%d %s\n", stamp, g_level_lower[level],
                g_component, (int)getpid(), kv);
        fflush(g_file);
    }
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    char stamp[32], text[LOG_LINE_MAX], kv[LOG_LINE_MAX + 32];
    size_t o = 0;
    va_list ap;

    if (level < LOG_LVL_DEBUG || level > LOG_LVL_ERROR)
        level = LOG_LVL_ERROR;
    timestamp(stamp, sizeof stamp);
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    /* event=log msg="..." with quotes/backslashes escaped and newlines flattened. */
    o = (size_t)snprintf(kv, sizeof kv, "event=log msg=\"");
    for (const char *p = text; *p && o + 3 < sizeof kv; p++) {
        if (*p == '"' || *p == '\\')
            kv[o++] = '\\';
        kv[o++] = (*p == '\n') ? ' ' : *p;
    }
    kv[o++] = '"';
    kv[o] = '\0';

    pthread_mutex_lock(&g_log_lock);
    if (level >= g_min_level)
        emit_locked(level, stamp, text, kv);
    pthread_mutex_unlock(&g_log_lock);
}

void log_event(log_level_t level, const char *event, const char *fmt, ...)
{
    char stamp[32], fields[LOG_LINE_MAX], line[LOG_LINE_MAX + 64];
    va_list ap;

    if (level < LOG_LVL_DEBUG || level > LOG_LVL_ERROR)
        level = LOG_LVL_ERROR;
    timestamp(stamp, sizeof stamp);
    va_start(ap, fmt);
    vsnprintf(fields, sizeof fields, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "event=%s %s", event, fields);

    pthread_mutex_lock(&g_log_lock);
    if (level >= g_min_level)
        emit_locked(level, stamp, line, line);
    pthread_mutex_unlock(&g_log_lock);
}
