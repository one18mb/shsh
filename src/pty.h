#ifndef SH_PTY_H
#define SH_PTY_H

#include <stdint.h>

/* 打开一对 pty。master 为非阻塞 + CLOEXEC，slave 为 CLOEXEC。
 * 成功返回 0，失败返回 -1。 */
int pty_open(int *master_out, int *slave_out);

/* 设置终端窗口大小（对 master 调用即可影响 slave 侧） */
int pty_set_size(int fd, uint16_t rows, uint16_t cols);

#endif