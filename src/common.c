#include "common.h"

#include <ctype.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        fatal("内存不足");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        fatal("内存不足");
    return q;
}

char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d)
        fatal("内存不足");
    return d;
}

bool sh_name_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > SH_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

int shsh_log_fd = STDERR_FILENO;

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dprintf(shsh_log_fd, "[shsh] ");
    vdprintf(shsh_log_fd, fmt, ap);
    dprintf(shsh_log_fd, "\n");
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dprintf(shsh_log_fd, "[shsh] 错误: ");
    vdprintf(shsh_log_fd, fmt, ap);
    dprintf(shsh_log_fd, "\n");
    va_end(ap);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dprintf(shsh_log_fd, "[shsh] 致命: ");
    vdprintf(shsh_log_fd, fmt, ap);
    dprintf(shsh_log_fd, "\n");
    va_end(ap);
    exit(1);
}
