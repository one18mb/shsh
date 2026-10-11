#ifndef SH_PTY_H
#define SH_PTY_H

#include <stdint.h>
#include <termios.h>

/* 打开一对 pty。master 为非阻塞 + CLOEXEC，slave 为 CLOEXEC。
 * 成功返回 0，失败返回 -1。 */
int pty_open(int *master_out, int *slave_out);

/* 设置终端窗口大小（对 master 调用即可影响 slave 侧） */
int pty_set_size(int fd, uint16_t rows, uint16_t cols);

/* 配置会话共享的规范终端状态：
 *   canonical + ECHO（输入过程由行规程回显并广播）
 *   TOSTOP（后台进程组写终端即收 SIGTTOU 停止，保证单流不交错）
 *   关闭 ISIG（^C/^D 一律由会话层在写入前拦截解释，不产生行规程信号）
 * 成功返回 0，并把配置后的 termios 写入 *canon（供命令结束后恢复）。 */
int pty_config_shared(int slave_fd, struct termios *canon);

#endif