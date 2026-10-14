// Leveled logging to stdout, a log file and the in-game console ring buffer.
#ifndef DG_LOG_H
#define DG_LOG_H

#include "base.h"

typedef enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL } LogLevel;

#define LOG_RING_LINES 512
#define LOG_LINE_MAX 256

typedef struct LogLine { LogLevel level; char text[LOG_LINE_MAX]; } LogLine;

void log_init(const char *file_path);
void log_shutdown(void);
void log_msg(LogLevel level, const char *fmt, ...);
void log_set_min_level(LogLevel level);

// Console access: lines are indexed oldest..newest.
int log_line_count(void);
const LogLine *log_get_line(int index);
u32 log_revision(void);

#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define log_fatal(...) log_msg(LOG_FATAL, __VA_ARGS__)

#endif
