#ifndef SH_SERVER_H
#define SH_SERVER_H

typedef struct session session_t;

/* 启动会话（mkdir/socket/bind/listen/epoll/signalfd）。
 * 成功返回 0 并写出 *out；会话已存在返回 -1。 */
const char *session_name(session_t *s);
const char *session_sockpath(session_t *s);

int session_start(session_t **out, const char *name);
/* 主循环，阻塞直到收到 SIGINT/SIGTERM。 */
int session_run(session_t *s);
void session_free(session_t *s);

#endif
