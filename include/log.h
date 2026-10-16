/*
 * OS concept: Mutual exclusion (pthread mutex protecting a shared output stream) — Unit 3
 *
 * Thread-safe logger. Each line carries a timestamp, level, component and pid:
 *   2026-10-16T05:31:00.123 INFO  reliefd[1234] message
 */
#ifndef RELIEF_LOG_H
#define RELIEF_LOG_H

typedef enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR
} log_level_t;

/* Set the component name shown in every line (e.g. "reliefd", "camp"). */
void log_init(const char *component);

/* Lines below this level are dropped. Default: LOG_LVL_INFO. */
void log_set_level(log_level_t level);

void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_msg(LOG_LVL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_LVL_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)

#endif /* RELIEF_LOG_H */
