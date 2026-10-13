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

/* 命令相位标志：会话层每交付一条要执行的 canonical 行置 1（发 OSC 633 A）；
 * shell 层在下一次 PS1（setprompt）处先发 C;<退出码> 再发 D，保证字节流里
 * 标记顺序为 A → 输出 → C → D+PS1。to/exit/help 等被会话层拦截、不交给
 * shell 执行的行不置位，故不会有无前导 A 的 C。执行者掉线 abort 回 PS1 时
 * 标志仍在，C 携带 EXINT 的退出状态（通常 130）。 */
extern volatile int shsh_phase_run;
void shsh_shell_phase_begin(void);

/* 排空钩子（会话层注册，见 server.c）：shell 层在 PS1 发 C 后询问"命令输入
 * 是否已全部消费"；为真则在同一路输出里追加一枚 OSC 633 P（send 的批次结束
 * 信号）。同路输出保证 P 排在 C 之后，无跨 fd 时序问题。 */
extern int (*shsh_cmd_drained)(void);

/* 把一个已打开的命令源 fd（通常是会话 pipe 的读端，调用方应放到 >=10
 * 以免与 shell 内部 fd 冲突）压为持久输入层，并驱动解析执行直到该 fd
 * EOF。未闭合引号/heredoc/行尾续行期间 parser 阻塞在 fd 上等后续写入，
 * 因此同一条逻辑命令可由多次写入（多条 canonical 行）拼成。
 * 不打印提示符；EOF 只结束本次驱动，不终止进程。fd 由调用方关闭。
 * 返回最后一条命令状态；*want_exit 同 run_line 语义。 */
int shsh_shell_run_fd(int fd, int *want_exit);

/* ---- 内建 read 的让出钩子 ----
 * 内建命令（如 read）在 fd0 上阻塞读取时不会回调会话层钩子，事件循环随之
 * 停摆 → 会话冻结（收不到键入、也无回显）。故 read 在无数据可读时调用
 * shsh_read_wait 让出，由会话层驱动一轮事件循环（转发键入、广播回显），
 * 再重试；shsh_read_abort 置位表示会话层要求 read 放弃（执行者掉线/会话
 * 停止），read 应立即返回失败。二者由会话层在 session_start 注册。 */
extern void (*shsh_read_wait)(void);
extern volatile int shsh_read_abort;

#endif
