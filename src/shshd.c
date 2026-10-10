#include "common.h"
#include "server.h"

static void usage(FILE *f)
{
    fprintf(f,
            "用法: shshd <会话名>\n"
            "\n"
            "启动并持有共享会话 <会话名>（前台运行，Ctrl+C / SIGTERM 结束会话）。\n"
            "其他用户用 shsh -s <会话名> -n <用户名> 加入。\n"
            "\n"
            "选项:\n"
            "  -h, --help    显示本帮助\n");
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
    session_run(s);
    log_info("会话 %s 已结束", session_name(s));
    session_free(s);
    return 0;
}