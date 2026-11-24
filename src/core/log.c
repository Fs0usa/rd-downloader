#define _POSIX_C_SOURCE 200809L
#include "rd/log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef RD_HOST_BUILD
#define RD_LOG_PATH "/tmp/rd-downloader-host-downloads/downloads/rd-downloader.log"
#else
#define RD_LOG_PATH "/data/homebrew/downloads/rd-downloader.log"
#endif

static pthread_mutex_t rd_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void rd_log_write(const char *level, const char *format, ...) {
    char message[1400], line[1600];
    struct timespec now;
    struct tm calendar;
    va_list arguments;
    size_t used, i;
    int fd, count, saved_errno;

    memset(&now, 0, sizeof(now));
    if (!level) level = "ERROR";
    va_start(arguments, format);
    count = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (count < 0) return;
    message[sizeof(message) - 1U] = '\0';
    for (i = 0; message[i]; ++i)
        if (message[i] == '\r' || message[i] == '\n' ||
            (unsigned char)message[i] < 0x20U) message[i] = ' ';
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        !localtime_r(&now.tv_sec, &calendar)) {
        memset(&calendar, 0, sizeof(calendar));
        calendar.tm_year = 70;
        calendar.tm_mday = 1;
    }
    count = snprintf(line, sizeof(line),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ld [%s] %s\n",
        calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday,
        calendar.tm_hour, calendar.tm_min, calendar.tm_sec,
        now.tv_nsec / 1000000L, level, message);
    if (count < 0 || (size_t)count >= sizeof(line)) return;

    pthread_mutex_lock(&rd_log_mutex);
    fd = open(RD_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        saved_errno = errno;
        fprintf(stderr, "RD Downloader: cannot append log (%s): errno=%d\n",
                RD_LOG_PATH, saved_errno);
        pthread_mutex_unlock(&rd_log_mutex);
        return;
    }
    used = 0;
    while (used < (size_t)count) {
        ssize_t written = write(fd, line + used, (size_t)count - used);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        used += (size_t)written;
    }
    saved_errno = errno;
    close(fd);
    if (used != (size_t)count)
        fprintf(stderr, "RD Downloader: incomplete log write (%s): errno=%d\n",
                RD_LOG_PATH, saved_errno);
    pthread_mutex_unlock(&rd_log_mutex);
}
