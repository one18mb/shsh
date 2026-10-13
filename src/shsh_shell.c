/*
 * shsh shell 层的会话驱动入口（SHSH_EMBED）。
 *
 * dash 蓝本自带 main()/cmdloop() 主循环；整合进 shshd 后，主循环归会话层
 * （epoll）所有，shell 层只提供"初始化一次 + 逐行执行"的库式接口：
 *
 *   shsh_shell_init()          进程启动时调用一次
 *   shsh_shell_run_line(line)  会话层每收到一条 canonical 行调用一次
 *
 * 一行内仍走 dash 原生的 parsecmd/evaltree（经 evalstring），cwd、变量、
 * $?、作业在多次调用间天然持久——它们就是同一进程内的全局状态。
 *
 * 异常落点沿用 dash 原生的 main_handler：forkreset（main.c 的 MKINIT
 * FORKRESET 块）会把命令子进程的 handler 重置为 &main_handler，子进程
 * EV_EXIT 路径 exraise 后 longjmp 回这里并 exitshell；根 shell 则 reset
 * 后把控制权交回会话层主循环。命名遵循 dash 风格。
 */

#include <stdlib.h>
#include <unistd.h>

#include "init.h"
#include "main.h"
#include "shell.h"
#include "options.h"
#include "eval.h"
#include "parser.h"
#include "input.h"
#include "var.h"
#include "trap.h"
#include "output.h"
#include "jobs.h"
#include "memalloc.h"
#include "error.h"

/* main_handler 定义在 main.c（MKINIT）；embed 下没有 main()，由本文件
 * 在每次逐行执行前为其建立 setjmp 落点。shsh_errno 同理在 embed 下由本
 * 文件提供（main.c 条件排除）；rootpid/shlvl 仍在 main.c。 */
extern struct jmploc main_handler;
#ifdef __GLIBC__
int *shsh_errno;
#endif

/* evalstring 在 eval.c（dash 原生）：setinputstring + parsecmd + evaltree */
int evalstring(char *s, int flags);

/* 提示符：执行者名 + cwd 基名。命令边界/执行者/退出码由 setprompt 打印
 * PS1 前发出的 OSC 633 C/D 标记承载（见 parser.c）。 */
#define SHSH_PS1 "$SHSH_USER:${PWD##*/}$ "

static int sh_line_exc;

/* 内建 read 的让出钩子与放弃标志（见 shsh_shell.h）：会话层在 session_start
 * 注册/清除；read 无数据可读时经此驱动会话事件循环，避免会话冻结。 */
void (*shsh_read_wait)(void) = NULL;
volatile int shsh_read_abort = 0;

/* 命令相位标志（见 shsh_shell.h）：会话层交付命令置位，parser 的 setprompt
 * 在 PS1 处消费（先 C 后 D）。 */
volatile int shsh_phase_run = 0;

/* 排空钩子（会话层注册，见 server.c）：shell 层据此判定是否追加 P。 */
int (*shsh_cmd_drained)(void) = NULL;

void
shsh_shell_phase_begin(void)
{
	shsh_phase_run = 1;
}

/* 异常落点：fork 出的命令子进程继承/被重置到 main_handler，其 EV_EXIT
 * 路径会 exraise(EXEND)：子进程必须 exitshell 永不返回；只有根 shell 才
 * reset 并把控制权交回会话层。 */
static void
sh_line_catch(void)
{
	if (getpid() != rootpid)
		exitshell();			/* 子进程：不返回 */

	exitreset();
	sh_line_exc = exception;
	if (sh_line_exc == EXINT)
		out2c('\n');
	reset();
}

void
shsh_shell_init(void)
{
#ifdef __GLIBC__
	shsh_errno = __errno_location();
#endif
	rootpid = getpid();
	shlvl = 0;

	/* mkinit 聚合的全局初始化（变量表、信号默认处置、内建表等） */
	init();

	/* 以"读标准输入的非交互 shell"为底，但提示符/输入由会话层接管：
	 * 不打开任何脚本文件、不读 profile。mflag=1 表示开启作业控制（下）；
	 * 信号/行编辑仍按非交互处理，故 iflag 保持 0。 */
	arg0 = "shsh";
	minusc = NULL;
	argptr = NULL;
	sflag = 1;
	iflag = 0;
	mflag = 1;
	shellparam.p = NULL;
	shellparam.nparam = 0;
	shellparam.optind = 1;
	shellparam.optoff = 0;
	shellparam.malloc = 0;

	/* 固定 PS1 为 shsh 形态；dash 随后每次打印它即提示符。 */
	setvar("PS1", SHSH_PS1, 0);

	/* 信号策略（见 trap.c SHSH_EMBED 分支）：SIGINT 捕获为 EXINT 回提示符；
	 * SIGTERM/SIGQUIT 保持默认，交由 shshd 经 signalfd 收编（不忽略）。 */
	setsignal(SIGINT);

	/* 开启 shell 层作业控制：shell 自成进程组（rootpid）并持有 pty 前台组，
	 * 前台命令执行期间把控制终端交给该组（jobs.c 的 tcsetpgrp），使交互式
	 * 子 shell/TUI（如 bash）取得作业控制，TOSTOP 与 ^C(VINTR) 也按标准终端
	 * 语义生效。控制终端由会话层在 session_start 里取得。 */
	setjobctl(1);
}

/* 执行一条 canonical 行（不含结尾换行）。
 * 返回该条命令的退出状态；*want_exit 置位表示遇到 exit（EXEXIT/EXEND），
 * 由会话层决定是断开当前用户还是关停——shell 层自身永不 _exit。 */
int
shsh_shell_run_line(const char *line, int *want_exit)
{
	struct jmploc *saved_handler;
	int status;

	if (want_exit)
		*want_exit = 0;
	if (line == NULL)
		line = "";

	saved_handler = handler;
	handler = &main_handler;
	sh_line_exc = -1;
	if (unlikely(setjmp(main_handler.loc))) {
		sh_line_catch();
		handler = saved_handler;
		if (sh_line_exc == EXEXIT || sh_line_exc == EXEND) {
			if (want_exit)
				*want_exit = 1;
			return exitstatus;
		}
		/* EXERROR / EXINT：本行失败，会话继续 */
		return exitstatus ? exitstatus : (sh_line_exc == EXINT ? 130 : 2);
	}

	/* evalstring 内部会 sstrdup 输入串，line 可直接来自会话层易变缓冲 */
	status = evalstring((char *)line, 0);
	handler = saved_handler;

	return status;
}

/* 放弃当前未完成的多行命令输入：复用 dash 的中断恢复路径（onint →
 * EXINT → 回到 run_fd 的 main_handler 落点并 reset），丢弃半成品解析。
 * 由会话层在执行者中途离开（shell 停在 PS2）时调用。 */
void
shsh_shell_abort(void)
{
	if (getpid() != rootpid)
		return;
	onint();			/* 声明为 noreturn：exraise(EXINT) */
}

/* 直接设置 SHSH_USER（见 shsh_shell.h）。setvar 会复制、导出该变量。 */
void
shsh_shell_set_user(const char *name)
{
	setvar("SHSH_USER", name, VEXPORT);
}

/* 从持久 fd 源驱动：无提示符版 cmdloop。EOF（NEOF）结束本次驱动但不
 * exitshell——会话命令 pipe 的 EOF 只在 shshd 关停时出现。每条命令的
 * 解析/执行异常仍经 main_handler：子进程 exitshell，根 shell reset
 * 后继续下一条。 */
int
shsh_shell_run_fd(int fd, int *want_exit)
{
	union node *n;
	struct stackmark smark;
	int status = 0;

	if (want_exit)
		*want_exit = 0;

	INTOFF;
	/* 作为 basepf 本体（push=0），而非压栈层：dash 的 RESET 会
	 * popallfiles() 弹回 basepf，若命令源是压栈层会在异常时被弹掉
	 * （甚至二次释放）。放到 basepf 后，异常 reset 只清压栈层，
	 * 命令源在多次异常/多条命令间保持。 */
	shsh_setinputfd(fd, 0);
	INTON;
	setstackmark(&smark);

	for (;;) {
		handler = &main_handler;
		sh_line_exc = -1;
		if (unlikely(setjmp(main_handler.loc))) {
			if (getpid() != rootpid)
				exitshell();
			exitreset();
			sh_line_exc = exception;
			if (sh_line_exc == EXINT)
				out2c('\n');
			reset();
			if (sh_line_exc == EXEXIT || sh_line_exc == EXEND) {
				if (want_exit)
					*want_exit = 1;
				status = exitstatus;
				break;
			}
			continue;			/* 本行出错，继续读下一条 */
		}

		/* doprompt=1：dash 在每条命令/续行前打印 PS1/PS2；setprompt 同时
		 * 发出 OSC 633 C/D 标记，随后可见提示符经 master 广播全员。 */
		n = parsecmd(1);
		if (n == NEOF)
			break;
		status = evaltree(n, 0);
	}

	popstackmark(&smark);
	/* 命令源是 basepf，不 popfile（异常时 reset 已弹回 basepf） */
	return status;
}
