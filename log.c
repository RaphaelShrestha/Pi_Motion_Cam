/* log.c - timestamped logging to stderr */
#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int verbose = 0;

void log_set_verbose(int on) { verbose = on; }
int  log_is_verbose(void)    { return verbose; }

static void stamp(void)
{
    char buf[16];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", buf);
}

static void emit(const char *prefix, const char *fmt, va_list ap)
{
    stamp();
    if (prefix)
        fputs(prefix, stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(NULL, fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("warning: ", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("error: ", fmt, ap);
    va_end(ap);
}

void log_errno(const char *what)
{
    log_error("%s: %s", what, strerror(errno));
}

void log_verbose(const char *fmt, ...)
{
    va_list ap;

    if (!verbose)
        return;
    va_start(ap, fmt);
    emit(NULL, fmt, ap);
    va_end(ap);
}
