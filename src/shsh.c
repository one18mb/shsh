#include "client.h"
#include "common.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

/*
 * shsh —— 交互客户端，同时也是 send 的本地端点。
 *
 * 命令：
 *   shsh -h
 *   shsh -l [-s 会话名]                     列出会话/在线用户
 *   shsh login -n 用户名 [-s 会话名] [--plain] [-d]
 *                                           加入会话；-d 为后台端点（供 send）
 *   shsh logout -n 用户名 [-s 会话名]       停止该端点并清理其文件
 *
 * 无论前台交互还是后台端点，shsh 都会在 ~/.shsh/ 下建立 <会话>__<用户名>.{in,out}
 * 通道（FIFO 注入 + 输出 tee），send 据此与同一 shsh 通信 —— 用户与 send 共同
 * 作用于输入，shshd 输出作用于输出。shsh 是唯一的 shshd 客户端。
 */

static void usage(FILE *f)
{
    fprintf(f,
            "用法:\n"
            "  shsh -h\n"
            "  shsh -l [-s 会话名]\n"
            "  shsh login -n 用户名 [-s 会话名] [--plain] [-d]\n"
            "  shsh logout -n 用户名 [-s 会话名]\n"
            "\n"
            "选项:\n"
            "  -n 用户名   目标用户（login/logout 必填）\n"
            "  -s 会话名   会话名（省略时为默认 \"%s\"）\n"
            "  --plain     剥除输出中的 OSC 633 标记，只留可见文本\n"
            "  -d          后台端点：脱离终端常驻，供 send 使用\n",
            SH_DEFAULT_SESSION);
}

/* 建 ~/.shsh 并返回 <会话>__<用户名>.log 路径（供 -d 诊断） */
static int ensure_rundir(char *logpath, size_t sz, const char *session,
                         const char *name)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        snprintf(logpath, sz, "/dev/null");
        return -1;
    }
    char dir[PATH_MAX - 16];
    snprintf(dir, sizeof dir, "%s/.shsh", home);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        snprintf(logpath, sz, "/dev/null");
        return -1;
    }
    snprintf(logpath, sz, "%s/%s__%s.log", dir, session, name);
    return 0;
}

/* 后台端点：fork+setsid 脱离终端，stdin/stdout 丢弃、stderr 落 log，
 * 子进程以 background=true 运行 client_join（建立端点通道并常驻）。 */
static int start_background(const char *session, const char *name, bool plain)
{
    char logpath[PATH_MAX];
    ensure_rundir(logpath, sizeof logpath, session, name);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
        }
        int lf = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lf >= 0)
            dup2(lf, STDERR_FILENO);
        client_join(session, name, plain, true);
        _exit(0);
    }
    usleep(700 * 1000);
    if (kill(pid, 0) == 0) {
        fprintf(stderr, "已启动后台端点 %s（%s），pid=%d\n", name, session,
                (int)pid);
        return 0;
    }
    fprintf(stderr, "启动后台端点失败，见 %s\n", logpath);
    return 1;
}

int main(int argc, char **argv)
{
    const char *sub = NULL;
    const char *name = NULL;
    const char *session = NULL;
    bool plain = false, background = false, list = false, help = false;

    int i = 1;
    if (argc >= 2 &&
        (strcmp(argv[1], "login") == 0 || strcmp(argv[1], "logout") == 0)) {
        sub = argv[1];
        i = 2;
    }
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            help = true;
        } else if (strcmp(a, "-l") == 0) {
            list = true;
        } else if (strcmp(a, "--plain") == 0) {
            plain = true;
        } else if (strcmp(a, "-d") == 0) {
            background = true;
        } else if (strcmp(a, "-n") == 0) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            name = argv[i];
        } else if (strcmp(a, "-s") == 0) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            session = argv[i];
        } else {
            fprintf(stderr, "未知选项: %s\n", a);
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
        /* -l 时 -n/-d 无意义 */
        if (name || background) {
            usage(stderr);
            return 2;
        }
        return client_list(session);
    }
    if (!sub) {
        usage(stderr);
        return 2;
    }
    if (!name || !sh_name_valid(name)) {
        fprintf(stderr, "非法的用户名: %s\n", name ? name : "(未指定)");
        return 2;
    }
    const char *sess = session ? session : SH_DEFAULT_SESSION;
    if (strcmp(sub, "logout") == 0) {
        if (plain || background) {
            usage(stderr);
            return 2;
        }
        return client_logout(sess, name);
    }
    /* login */
    if (background)
        return start_background(sess, name, plain);
    return client_join(sess, name, plain, false);
}