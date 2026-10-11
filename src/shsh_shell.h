#ifndef SHSH_SHELL_H
#define SHSH_SHELL_H

/* shell 层会话驱动入口（SHSH_EMBED） */
void shsh_shell_init(void);

/* 执行一条 canonical 行。want_exit 非 NULL 时，遇 exit 返回 1
 * （shell 不自行退出进程，交由会话层处理）。返回命令退出状态。 */
int shsh_shell_run_line(const char *line, int *want_exit);

/* 放弃当前未完成的多行命令输入（如 if/heredoc 续行中途执行者离开）：
 * 走 dash 原生 EXINT 恢复路径回到 PS1，丢弃半成品解析与已缓冲的续行。
 * 仅在 shell 处于 PS2（whichprompt==2）且可 longjmp 的上下文中调用。 */
void shsh_shell_abort(void);

/* 直接设置 shell 的 SHSH_USER 变量（当前执行者名）。执行权变更时由会话层
 * 立即调用，使 shell 随后打印的 PS1 就带新名字——避免"先按旧名打一次 PS1、
 * 再注入改名"产生的过渡提示符。 */
void shsh_shell_set_user(const char *name);

/* 把一个已打开的命令源 fd（通常是会话 pipe 的读端，调用方应放到 >=10
 * 以免与 shell 内部 fd 冲突）压为持久输入层，并驱动解析执行直到该 fd
 * EOF。未闭合引号/heredoc/行尾续行期间 parser 阻塞在 fd 上等后续写入，
 * 因此同一条逻辑命令可由多次写入（多条 canonical 行）拼成。
 * 不打印提示符；EOF 只结束本次驱动，不终止进程。fd 由调用方关闭。
 * 返回最后一条命令状态；*want_exit 同 run_line 语义。 */
int shsh_shell_run_fd(int fd, int *want_exit);

#endif
