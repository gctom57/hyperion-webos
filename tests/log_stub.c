// Replaces src/log.c so tests don't need PmLog; warnings go to stderr.
#include "log.h"

void log_init(void)
{
}

void log_set_level(LogLevel level)
{
    (void)level;
}

void log_printf(LogLevel level, const char* module, const char* fmt, ...)
{
    if (level > Warning) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s] ", module);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}
