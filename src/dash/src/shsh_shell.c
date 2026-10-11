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

static int sh_line_exc;

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
	 * 不打开任何脚本文件、不读 profile、不做 setjobctl（终端归会话层）。 */
	arg0 = "shsh";
	minusc = NULL;
	argptr = NULL;
	sflag = 1;
	iflag = 0;
	mflag = 0;
	shellparam.p = NULL;
	shellparam.nparam = 0;
	shellparam.optind = 1;
	shellparam.optoff = 0;
	shellparam.malloc = 0;
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

		n = parsecmd(0);			/* doprompt=0：无 PS1/PS2 输出 */
		if (n == NEOF)
			break;
		status = evaltree(n, 0);
	}

	popstackmark(&smark);
	/* 命令源是 basepf，不 popfile（异常时 reset 已弹回 basepf） */
	return status;
}
