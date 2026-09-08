/* log.h - timestamped logging to stderr */
#ifndef PMC_LOG_H
#define PMC_LOG_H

#if defined(__GNUC__)
#define PMC_PRINTF(fmt_idx, first_arg) \
    __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define PMC_PRINTF(fmt_idx, first_arg)
#endif

void log_set_verbose(int on);
int  log_is_verbose(void);

void log_info(const char *fmt, ...)    PMC_PRINTF(1, 2);
void log_warn(const char *fmt, ...)    PMC_PRINTF(1, 2);
void log_error(const char *fmt, ...)   PMC_PRINTF(1, 2);
void log_verbose(const char *fmt, ...) PMC_PRINTF(1, 2);
void log_errno(const char *what);

#endif /* PMC_LOG_H */
