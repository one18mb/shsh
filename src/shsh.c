#include "client.h"
#include "common.h"

static void usage(FILE *f)
{
    fprintf(f,
            "用法:\n"
            "  shsh <用户名> [会话名]   加入共享会话（省略会话名时为 \"%s\"）\n"
            "  shsh -l [会话名]         列出所有活动会话，或指定会话的在线用户\n"
            "  shsh -h                  显示本帮助\n",
            SH_DEFAULT_SESSION);
}

int main(int argc, char **argv)
{
    const char *session = NULL;
    const char *name = NULL;
    bool list = false;
    bool help = false;
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            help = true;
        } else if (strcmp(a, "-l") == 0) {
            list = true;
        } else if (a[0] == '-') {
            fprintf(stderr, "未知选项: %s\n", a);
            usage(stderr);
            return 2;
        } else if (list) {
            /* shsh -l [会话名]：至多一个位置参数 */
            if (session) {
                usage(stderr);
                return 2;
            }
            session = a;
            npos++;
        } else {
            /* shsh <用户名> [会话名]：第一个位置参数是用户名，第二个是会话名 */
            if (npos == 0)
                name = a;
            else if (npos == 1)
                session = a;
            else {
                usage(stderr);
                return 2;
            }
            npos++;
        }
    }

    if (help) {
        usage(stdout);
        return 0;
    }
    if (session && !sh_name_valid(session)) {
        fprintf(stderr, "非法的会话名: %s\n", session);
        return 2;
    }
    if (list) {
        if (name) {
            usage(stderr);
            return 2;
        }
        return client_list(session);
    }
    if (!name) {
        usage(stderr);
        return 2;
    }
    if (!sh_name_valid(name)) {
        fprintf(stderr, "非法的用户名: %s\n", name);
        return 2;
    }
    return client_join(session ? session : SH_DEFAULT_SESSION, name);
}
