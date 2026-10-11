#ifndef SH_SERVER_H
#define SH_SERVER_H

typedef struct session session_t;

/* 启动会话（mkdir/socket/bind/listen/epoll/signalfd、pty、命令管道）。
 * 成功返回 0 并写出 *out；会话已存在返回 -1。
 * 会话层握有命令管道写端，并把自己注册为 shell 层的两个同步钩子
 * （shsh_cmd_wait / shsh_job_wait），由 shsh_shell_run_fd 驱动。 */
int session_start(session_t **out, const char *name);
void session_free(session_t *s);

const char *session_name(session_t *s);
const char *session_sockpath(session_t *s);
/* shell 层命令源：命令管道读端（已移到 fd>=10，供 shsh_shell_run_fd） */
int session_cmd_fd(session_t *s);
/* 共享 pty 的 slave 端（shell 层 fd0/1/2 指向它，输出经 master 广播） */
int session_pty_slave(session_t *s);

#endif