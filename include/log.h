/*
 * OS concept: Mutual exclusion (pthread mutex protecting shared output streams) — Unit 3
 *
 * Thread-safe logger with two sinks:
 *   stderr (human):        2026-10-16T05:31:00.123 INFO  reliefd[1234] event=camp_exit camp=1 status=0
 *   file   (key=value):    ts=2026-10-16T05:31:00.123 level=info comp=reliefd pid=1234 event=camp_exit camp=1 status=0
 * The file sink is optional (log_open_file) and is meant to be parsed with awk.
 * Values in key=value fields never contain spaces, except msg="..." on free-form lines.
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

/* Also append structured key=value lines to path (close-on-exec). Returns 0 or -1. */
int log_open_file(const char *path);
void log_close_file(void);

/* Free-form message. In the file sink it becomes: event=log msg="..." */
void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Structured event. fmt produces the remaining "key=value key=value" fields. */
void log_event(log_level_t level, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_DEBUG(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  log_msg(LOG_LVL_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  log_msg(LOG_LVL_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)

#define EV_INFO(event, ...)  log_event(LOG_LVL_INFO,  event, __VA_ARGS__)
#define EV_WARN(event, ...)  log_event(LOG_LVL_WARN,  event, __VA_ARGS__)
#define EV_ERROR(event, ...) log_event(LOG_LVL_ERROR, event, __VA_ARGS__)

#endif /* RELIEF_LOG_H */
