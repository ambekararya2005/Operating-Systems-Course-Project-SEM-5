/*
 * OS concept: Mutual exclusion (pthread mutex protecting a shared output stream) — Unit 3
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *g_component = "relief";
static log_level_t g_min_level = LOG_LVL_INFO;

static const char *const g_level_names[] = { "DEBUG", "INFO", "WARN", "ERROR" };

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

void log_msg(log_level_t level, const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    char stamp[32];

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);

    pthread_mutex_lock(&g_log_lock);
    if (level >= g_min_level) {
        va_list ap;
        fprintf(stderr, "%s.%03ld %-5s %s[%d] ", stamp, ts.tv_nsec / 1000000L,
                g_level_names[level], g_component, (int)getpid());
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
        fflush(stderr);
    }
    pthread_mutex_unlock(&g_log_lock);
}
