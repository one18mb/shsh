#include "common.h"
#include "server.h"
#include "shsh_shell.h"

static void usage(FILE *f)
{
    fprintf(f,
            "用法: shshd <会话名>\n"
            "\n"
            "启动并持有共享会话 <会话名>（SIGTERM 结束会话）。shshd 是会话\n"
            "首领、自建 PTY 即其控制终端；从交互 shell 启动时会自动 fork\n"
            "守护化、转入后台运行。\n"
            "\n"
            "选项:\n"
            "  -h, --help    显示本帮助\n");
}

/* 把 shell 层的 fd0/1/2 指向共享 pty 的 slave：shell 自己 fork 出的外部
 * 命令据此继承终端，输出经 master 由会话层广播；日志另存原始 stderr，
 * 不混入共享流。 */
static void route_shell_stdio(int slave)
{
    shsh_log_fd = dup(STDERR_FILENO);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
}

int main(int argc, char **argv)
{
    const char *session = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (a[0] == '-') {
            fprintf(stderr, "未知选项: %s\n", a);
            usage(stderr);
            return 2;
        }
        if (session) {
            usage(stderr);
            return 2;
        }
        session = a;
    }
    if (!session) {
        usage(stderr);
        return 2;
    }
    if (!sh_name_valid(session)) {
        fprintf(stderr, "非法的会话名: %s\n", session);
        return 2;
    }

    session_t *s = NULL;
    if (session_start(&s, session) != 0) {
        fprintf(stderr, "会话 %s 已存在\n", session);
        return 1;
    }
    log_info("会话 %s 已启动 (socket=%s)", session_name(s), session_sockpath(s));

    /* 单进程内整合：会话层已就绪并注册好同步钩子；此处初始化 shell 层，
     * 把其 stdio 接上共享 pty，然后由 shell 层从命令管道读命令、逐条执行。
     * 会话事件循环不再由本进程显式 run，而是被 shell 层在"等命令 / 等前台
     * 作业"时经钩子同步驱动（单线程，不异步）。 */
    shsh_shell_init();
    route_shell_stdio(session_pty_slave(s));
    int want_exit = 0;
    shsh_shell_run_fd(session_cmd_fd(s), &want_exit);

    log_info("会话 %s 已结束", session_name(s));
    session_free(s);
    return 0;
}