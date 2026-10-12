#ifndef SH_PTY_H
#define SH_PTY_H

#include <stdint.h>
#include <termios.h>

/* 打开一对 pty。master 为非阻塞 + CLOEXEC，slave 为 CLOEXEC。
 * 成功返回 0，失败返回 -1。 */
int pty_open(int *master_out, int *slave_out);

/* 设置终端窗口大小（对 master 调用即可影响 slave 侧） */
int pty_set_size(int fd, uint16_t rows, uint16_t cols);

/* 把已打开的 pty slave 设为当前会话的控制终端（TIOCSCTTY）。调用者必须
 * 已经是会话首领（setsid），否则返回 -1。取得控制终端后，作业控制
 * （tcsetpgrp）、TOSTOP、^C(VINTR) 等标准终端语义才会生效。 */
int pty_acquire_ctty(int slave_fd);

/* 配置会话共享的规范终端状态：
 *   canonical + ECHO（输入过程由行规程回显并广播）
 *   ISIG（^C/VINTR 等按标准终端语义交前台进程组，不再由会话层代发信号）
 *   TOSTOP（后台进程组写终端即收 SIGTTOU 停止，保证单流不交错）
 * 成功返回 0，并把配置后的 termios 写入 *canon（供命令结束后恢复）。 */
int pty_config_shared(int slave_fd, struct termios *canon);

#endif