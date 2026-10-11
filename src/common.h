#ifndef SH_COMMON_H
#define SH_COMMON_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 会话/用户名的最大长度 */
#define SH_NAME_MAX 64
/* 单帧 payload 上限（1 MiB） */
#define SH_MSG_MAX (1u << 20)
/* socket 目录 */
#define SH_SOCK_DIR "/tmp/shsh"

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
/* 会话名/用户名合法性：1..64 字节，仅 [A-Za-z0-9._-] */
bool sh_name_valid(const char *s);

/* 日志输出 fd。默认 stderr；shshd 在把 fd2 改指共享 pty 前会保存原始
 * stderr 到这里，使会话层日志不混入广播给用户的共享流。 */
extern int shsh_log_fd;

void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
