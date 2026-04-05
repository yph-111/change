#include "logger.h"
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>

static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

int log_init(const char *filename) {
    log_file = fopen(filename, "a");
    if (log_file == NULL) {
        return -1;
    }
    return 0;
}

void log_close(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_destroy(&log_mutex);
}

void log_write(LogLevel level, const char *format, ...) {
    if (!log_file) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", t);

    const char *level_str = "INFO";
    if (level == LOG_WARNING) level_str = "WARN";
    else if (level == LOG_ERROR) level_str = "ERROR";

    pthread_t tid = pthread_self();

    pthread_mutex_lock(&log_mutex);

    fprintf(log_file, "[%s] [%s] [Thread: %lu] ", time_buf, level_str, (unsigned long)tid);

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file);

    pthread_mutex_unlock(&log_mutex);
}
