#ifndef SH_TERMCODE_H
#define SH_TERMCODE_H

#include "output.h"

/*
 * OSC 633 shell-integration 标记（统一文本协议）。
 *
 * shsh 不使用私有的带外二进制帧传递命令边界/退出码/执行权，而是把这些控制
 * 信息编码进同一条终端字节流里的 OSC 633 序列（人眼不可见；VS Code/iTerm2/
 * kitty 等终端能原生理解，其它终端忽略未知 OSC）。机器客户端（管道）解析
 * 这些序列即可精确切分命令边界，无需猜测提示符文本。
 *
 * 序列形式（终止符用 ST = ESC \\）：
 *   ESC ] 633 ; A ST                     命令行提交、即将执行
 *   ESC ] 633 ; B ST                     命令输出开始
 *   ESC ] 633 ; C ; <退出码> ST          命令结束（携带退出码）
 *   ESC ] 633 ; D ; s=<码>;u=<执行者>;w=<cwd基名> ST   提示符边界（回到空闲）
 *   ESC ] 633 ; E ; reason=<原因> ST     会话即将关闭（随后 EOF）
 *   ESC ] 633 ; W ; <rows>;x<cols> ST   （客户端→服务端）终端尺寸
 */

#define SH_OSC    "\033]633;"
#define SH_ST     "\033\\"

/* 输出一条无参数 633 标记（如 A、B）。 */
static inline void
term_mark(const char *code)
{
	out2str(SH_OSC);
	out2str(code);
	out2str(SH_ST);
}

/* 输出 "633;C;<status>"：命令结束，带退出码。 */
static inline void
term_mark_c(int status)
{
	outfmt(out2, SH_OSC "C;%d" SH_ST, status);
}

/* 输出提示符边界 D；s=退出码 u=执行者 w=工作目录基名。 */
static inline void
term_mark_d(int status, const char *user, const char *wbase)
{
	outfmt(out2, SH_OSC "D;s=%d;u=%s;w=%s" SH_ST,
	       status, user ? user : "", wbase ? wbase : "");
}

/* 输出会话关闭 E。 */
static inline void
term_mark_e(const char *reason)
{
	outfmt(out2, SH_OSC "E;reason=%s" SH_ST, reason ? reason : "");
}

#endif
