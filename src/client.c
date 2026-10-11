#include "client.h"
#include "buf.h"
#include "common.h"
#include "proto.h"

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>

/*
 * 客户端始终工作在原始模式：本地不做行编辑、不做回显、不解释控制字符，
 * 也**不自行响应 ^C/^D**（哑管道）。所有字节（含 ^C/^D）一律通过 RAW 帧
 * 交给 shshd，由会话层按"执行者/非执行者 + 相位"两模式裁决：
 *   - 普通字节：仅执行者写入共享 PTY，行规程回显后广播全员（实时可见）
 *   - 执行者：其前台是共享命令——^C/^D 交回 shshd（运行态转发给前台任务，
 *     空闲态由会话级处理：^C 无效果、^D 离开会话）
 *   - 非执行者：其前台即 shsh 自身——^C/^D 均结束本客户端（离开会话）
 * 是否退出由服务端回 BYE 决定；follower 与执行者的终端模式完全相同。
 */

static struct termios g_saved;
static bool g_saved_ok;
static bool g_tty;

static void restore_tio(void)
{
    if (g_saved_ok && g_tty)
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_saved);
}

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* ---- 连接与帧收发 ---- */

static void sock_path(char *out, size_t sz, const char *session)
{
    snprintf(out, sz, "%s/%s.sock", SH_SOCK_DIR, session);
}

static void set_sun_path(struct sockaddr_un *a, const char *path)
{
    snprintf(a->sun_path, sizeof a->sun_path, "%s", path);
}

static int connect_session(const char *session, char *errbuf, size_t errsz)
{
    char path[108];
    sock_path(path, sizeof path, session);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(errbuf, errsz, "socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    set_sun_path(&a, path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        int e = errno;
        snprintf(errbuf, errsz, "%s", strerror(e));
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static int send_frame(int fd, const msg_t *m)
{
    buf_t f;
    buf_init(&f);
    if (msg_encode(m, &f) != 0) {
        buf_free(&f);
        return -1;
    }
    int rc = write_all(fd, f.data, f.len);
    buf_free(&f);
    return rc;
}

static void send_raw(int fd, const void *p, size_t n)
{
    msg_t m;
    msg_make_raw(&m, p, n);
    send_frame(fd, &m);
    buf_free(&m.payload);
}

/* 转发一批 stdin 字节：普通字节成批上送；^C/^D 各自单独成帧。语义完全由
 * 服务端裁决（客户端是哑管道：^C/^D 不再本地退出）——执行者的 ^C/^D 可能
 * 被转交给其前台任务，follower 的则按 shsh 自身处理（离开）。 */
static void forward_input(int sock, const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && p[i] != 0x03 && p[i] != 0x04)
            i++;
        if (i > start)
            send_raw(sock, p + start, i - start);
        if (i == n)
            break;
        uint8_t ctl = p[i++];
        send_raw(sock, &ctl, 1);
    }
}

/* ---- 加入会话 ---- */

int client_join(const char *session, const char *name)
{
    char err[128];
    int fd = connect_session(session, err, sizeof err);
    if (fd < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED)
            fprintf(stderr, "会话 %s 不存在，请先运行: shshd %s\n",
                    session, session);
        else if (errno == EACCES)
            fprintf(stderr, "无法访问会话 %s: 权限不足\n", session);
        else
            fprintf(stderr, "连接会话 %s 失败: %s\n", session, err);
        return 1;
    }

    g_tty = isatty(STDIN_FILENO) == 1;
    if (g_tty && tcgetattr(STDIN_FILENO, &g_saved) == 0) {
        g_saved_ok = true;
        atexit(restore_tio);
        /* 全程原始模式（任何角色、任何相位）；保留输出后处理，避免阶梯显示 */
        struct termios t = g_saved;
        cfmakeraw(&t);
        t.c_oflag |= (OPOST | ONLCR);
        tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
    }

    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = 24;
    ws.ws_col = 80;
    if (g_tty)
        ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    msg_t hello;
    msg_make_hello(&hello, name, ws.ws_row, ws.ws_col);
    int hr = send_frame(fd, &hello);
    buf_free(&hello.payload);
    if (hr != 0) {
        fprintf(stderr, "发送 HELLO 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGWINCH);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sigfd < 0) {
        fprintf(stderr, "signalfd 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    buf_t in;
    buf_init(&in);
    bool welcome = false, quit = false, stdin_eof = false;
    int rc = 0;

    while (!quit) {
        struct pollfd pfds[3];
        pfds[0].fd = fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = sigfd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        pfds[2].fd = stdin_eof ? -1 : STDIN_FILENO; /* EOF 后不再监听 stdin */
        pfds[2].events = POLLIN;
        pfds[2].revents = 0;
        int pr = poll(pfds, 3, welcome ? -1 : 10000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            if (!welcome) {
                fprintf(stderr, "连接会话 %s 失败: 服务器无响应\n", session);
                rc = 1;
            }
            break;
        }

        if (pfds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sigfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGWINCH) {
                    if (g_tty && welcome) {
                        struct winsize nw;
                        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &nw) == 0) {
                            msg_t wm;
                            msg_make_winch(&wm, nw.ws_row, nw.ws_col);
                            send_frame(fd, &wm);
                            buf_free(&wm.payload);
                        }
                    }
                } else {
                    /* 外部信号（kill）：按掉线离开，不由 ^C/^D 路径处理 */
                    quit = true;
                    rc = 0;
                }
            }
        }

        if (pfds[2].revents & (POLLIN | POLLHUP)) {
            uint8_t chunk[4096];
            ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
            if (n == 0) {
                /* stdin EOF：发一个 ^D 后停止监听 stdin，自身不退出——
                 * 是否离开由服务端裁决（运行态会把它交给前台任务） */
                uint8_t eot = 0x04;
                send_raw(fd, &eot, 1);
                stdin_eof = true;
            } else if (n > 0) {
                forward_input(fd, chunk, (size_t)n);
            }
        }

        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char chunk[65536];
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n <= 0) {
                if (welcome)
                    fprintf(stderr, "连接已断开\n");
                rc = welcome ? 0 : 1;
                break;
            }
            buf_append(&in, chunk, (size_t)n);
            for (;;) {
                msg_t m;
                int r = msg_try_extract(&in, &m);
                if (r < 0) {
                    fprintf(stderr, "协议错误\n");
                    rc = 1;
                    quit = true;
                    break;
                }
                if (r == 0)
                    break;
                const uint8_t *data = NULL;
                size_t len = 0;
                switch (m.type) {
                case S2C_WELCOME:
                    welcome = true;
                    break;
                case S2C_OUT:
                    msg_parse_bytes(&m, &data, &len);
                    if (len)
                        write_all(STDOUT_FILENO, data, len);
                    break;
                case S2C_BYE: {
                    char reason[128] = "";
                    msg_parse_text(&m, reason, sizeof reason);
                    if (welcome) {
                        printf("\n[%s]\n", reason);
                        fflush(stdout);
                        rc = 0;
                        quit = true;
                    } else {
                        fprintf(stderr, "%s\n", reason);
                        rc = 1;
                        quit = true;
                    }
                    break;
                }
                /* RUN_BEGIN/RUN_END/ROLE/NOTICE/LISTRESP：
                 * 终端模式不再随相位切换，无需客户端动作 */
                default:
                    break;
                }
                buf_free(&m.payload);
            }
        }
    }
    buf_free(&in);
    close(sigfd);
    close(fd);
    return rc;
}

/* ---- 列出会话 ---- */

static void print_listing(const char *sess, const user_info_t *users, size_t n)
{
    const char *exec = "无";
    for (size_t i = 0; i < n; i++)
        if (users[i].is_exec) {
            exec = users[i].name;
            break;
        }
    printf("%s (执行者: %s)\n", sess, exec);
    for (size_t i = 0; i < n; i++) {
        bool is_sys = strcmp(users[i].name, "shshd") == 0;
        if (is_sys)
            printf("  %-20s [系统]%s\n", users[i].name,
                   users[i].is_exec ? " [执行者]" : "");
        else
            printf("  %-20s%s\n", users[i].name,
                   users[i].is_exec ? " [执行者]" : "");
    }
    fflush(stdout);
}

static int list_one(const char *session)
{
    char err[128];
    int fd = connect_session(session, err, sizeof err);
    if (fd < 0)
        return -1;
    msg_t m;
    msg_make_list(&m);
    int rc = send_frame(fd, &m);
    buf_free(&m.payload);
    if (rc != 0) {
        close(fd);
        return -1;
    }
    buf_t in;
    buf_init(&in);
    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 3000);
        if (pr <= 0)
            break;
        char chunk[8192];
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r <= 0)
            break;
        buf_append(&in, chunk, (size_t)r);
        msg_t fr;
        int ex = msg_try_extract(&in, &fr);
        if (ex == 0)
            continue;
        if (ex < 0)
            break;
        if (fr.type == S2C_LISTRESP) {
            char sess[SH_NAME_MAX + 1];
            user_info_t users[256];
            size_t n = 0;
            if (msg_parse_listresp(&fr, sess, sizeof sess, users, 256, &n) == 0)
                print_listing(sess, users, n);
            buf_free(&fr.payload);
            buf_free(&in);
            close(fd);
            return 0;
        }
        buf_free(&fr.payload);
    }
    buf_free(&in);
    close(fd);
    return -1;
}

int client_list(const char *session)
{
    if (session) {
        if (list_one(session) != 0) {
            fprintf(stderr, "会话 %s 不存在或已失效\n", session);
            return 1;
        }
        return 0;
    }
    DIR *d = opendir(SH_SOCK_DIR);
    if (!d) {
        printf("无活动会话\n");
        return 0;
    }
    char *names[256];
    size_t cnt = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 5, ".sock") != 0)
            continue;
        if (cnt < 256)
            names[cnt++] = xstrdup(e->d_name);
    }
    closedir(d);
    for (size_t i = 0; i < cnt; i++)
        for (size_t j = i + 1; j < cnt; j++)
            if (strcmp(names[j], names[i]) < 0) {
                char *t = names[i];
                names[i] = names[j];
                names[j] = t;
            }
    int found = 0;
    for (size_t i = 0; i < cnt; i++) {
        char sess[SH_NAME_MAX + 1];
        size_t l = strlen(names[i]);
        memcpy(sess, names[i], l - 5);
        sess[l - 5] = '\0';
        if (list_one(sess) == 0)
            found++;
        else
            fprintf(stderr, "注意: %s 已失效\n", names[i]);
        free(names[i]);
    }
    if (!found)
        printf("无活动会话\n");
    return 0;
}
