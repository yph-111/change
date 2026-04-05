#ifndef LOGGER_H
#define LOGGER_H

#include <pthread.h>
#include <stdio.h>

typedef enum {
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR
} LogLevel;

int log_init(const char *filename);
void log_close(void);
void log_write(LogLevel level, const char *format, ...);

#define LOG_I(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOG_W(...) log_write(LOG_WARNING, __VA_ARGS__)
#define LOG_E(...) log_write(LOG_ERROR, __VA_ARGS__)

#endif
