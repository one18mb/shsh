#ifndef SH_EXEC_H
#define SH_EXEC_H

#include "parse.h"

#include <sys/types.h>

/* 子进程内置命令回调：返回退出码 */
typedef int (*sh_child_fn)(void *ctx, const sh_command_t *cmd);

/* 启动一个子进程：
 *   - setpgid(0, pgroup)：pgroup=0 表示自成进程组（整条管道用同一个 pgroup，
 *     便于 Ctrl+C / 挂断时一次打全组）；不改变会话，故 pty 不产生信号，
 *     信号由服务端显式 kill(-pgid) 完成
 *   - 依次应用 cmd->redirs（从左到右），再把 0/1/2 接到 in_fd/out_fd/err_fd
 *     （<0 表示保持不变，即沿用 pty slave）
 *   - close_fds：需要显式关闭的 fd（管道两端）；exec 路径也安全，内置路径必需
 *   - child_fn 非空时在子进程内执行内置命令（不 exec）
 * 成功返回 pid，失败返回 -1。 */
pid_t exec_spawn(const sh_command_t *cmd, pid_t pgroup, const char *cwd,
                 int in_fd, int out_fd, int err_fd, const int *close_fds,
                 size_t nclose, sh_child_fn child_fn, void *child_ctx);

#endif