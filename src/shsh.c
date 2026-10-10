#include "client.h"
#include "common.h"

static void usage(FILE *f)
{
    fprintf(f,
            "用法:\n"
            "  shsh -l                          列出所有活动会话\n"
            "  shsh -l -s <会话>                列出指定会话的用户\n"
            "  shsh -s <会话> -n <用户名>       加入共享会话\n"
            "  shsh -h                          显示本帮助\n");
}

int main(int argc, char **argv)
{
    const char *session = NULL;
    const char *name = NULL;
    bool list = false;
    bool help = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            help = true;
        } else if (strcmp(a, "-l") == 0) {
            list = true;
        } else if (strcmp(a, "-s") == 0) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            session = argv[i];
        } else if (strcmp(a, "-n") == 0) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            name = argv[i];
        } else {
            usage(stderr);
            return 2;
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
            fprintf(stderr, "-l 模式下无需 -n\n");
            return 2;
        }
        return client_list(session);
    }
    if (!session || !name) {
        usage(stderr);
        return 2;
    }
    if (!sh_name_valid(name)) {
        fprintf(stderr, "非法的用户名: %s\n", name);
        return 2;
    }
    return client_join(session, name);
}
