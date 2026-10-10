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
#include <time.h>

/* 终端模式 */
typedef enum {
    M_UNKNOWN = 0,
    M_FOLLOWER,  /* 无执行权：规范模式、关闭回显，输入被丢弃 */
    M_EXEC_IDLE, /* 执行者空闲：规范模式 + 回显（命令执行前不共享） */
    M_RUN,       /* 命令运行中：原始模式，按键直通 pty */
} cmode_t;

static struct termios g_saved;
static bool g_saved_ok;
static bool g_tty;
static cmode_t g_mode = M_UNKNOWN;

static buf_t ed_line;
static size_t ed_pos;
static bool ed_on;      /* 本地行编辑中（tty + 执行者空闲） */
static bool ed_ready;   /* 行已按回车，等待提交 */
static bool ed_want;    /* 请求补全 */
static bool ed_await;   /* 已发出补全请求，等待应答 */
static bool ed_quit;    /* 需要离开会话 */
static bool ed_inited;
static long long ed_await_t0;    /* 补全请求发出时刻（ms） */

static void ed_reset(void);
static int write_all(int fd, const void *p, size_t n);
static void ed_feed(const uint8_t *p, size_t n);
static void ed_apply_complete(const uint8_t *p, size_t n);

static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void restore_tio(void)
{
    if (g_saved_ok && g_tty)
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_saved);
}

/* ---- 执行者空闲时的本地行编辑与 Tab 补全 ----
 *
 * 执行者空闲时客户端把本地终端设为原始模式，自己回显与编辑；这样 Tab 才能
 * 作为按键被截获。补全候选由服务端计算（会话 cwd / PATH 在服务端），应答只
 * 发给请求者，因此清单不会打扰其他用户。 */

static void ed_write(const char *p, size_t n)
{
    if (n)
        write_all(STDOUT_FILENO, p, n);
}

/* 重画光标处到行尾，并把光标移回原位 */
static void ed_tail(void)
{
    if (ed_pos >= ed_line.len)
        return;
    ed_write((const char *)ed_line.data + ed_pos, ed_line.len - ed_pos);
    for (size_t i = ed_pos; i < ed_line.len; i++)
        ed_write("\b", 1);
}

static void ed_move(int delta)
{
    if (delta < 0) {
        size_t n = (size_t)(-delta) > ed_pos ? ed_pos : (size_t)(-delta);
        for (size_t i = 0; i < n; i++)
            ed_write("\b", 1);
        ed_pos -= n;
    } else {
        size_t n = (size_t)delta;
        if (ed_pos + n > ed_line.len)
            n = ed_line.len - ed_pos;
        for (size_t i = 0; i < n; i++) {
            char c = (char)ed_line.data[ed_pos + i];
            ed_write(&c, 1);
        }
        ed_pos += n;
    }
}

static void ed_insert(unsigned char c)
{
    buf_reserve(&ed_line, 1);
    memmove(ed_line.data + ed_pos + 1, ed_line.data + ed_pos,
            ed_line.len - ed_pos);
    ed_line.data[ed_pos] = c;
    ed_line.len++;
    ed_write((const char *)&c, 1);
    ed_pos++;
    ed_tail();
}

static void ed_backspace(void)
{
    if (ed_pos == 0)
        return;
    memmove(ed_line.data + ed_pos - 1, ed_line.data + ed_pos,
            ed_line.len - ed_pos);
    ed_line.len--;
    ed_pos--;
    ed_write("\b", 1);
    ed_tail();
    ed_write(" ", 1);
    for (size_t i = ed_pos; i < ed_line.len + 1; i++)
        ed_write("\b", 1);
}

static void ed_kill_line(void)
{
    ed_move(-(int)ed_pos);
    ed_write("\x1b[K", 3);
    buf_clear(&ed_line);
    ed_pos = 0;
}

static void ed_reset(void)
{
    buf_clear(&ed_line);
    ed_pos = 0;
    ed_ready = false;
    ed_want = false;
    ed_await = false;
}

/* 处理原始按键 */
static void ed_feed(const uint8_t *p, size_t n)
{
    static int esc = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (esc == 1) {
            esc = (c == '[') ? 2 : 0;
            continue;
        }
        if (esc == 2) {
            esc = 0;
            if (c == 'D')
                ed_move(-1);
            else if (c == 'C')
                ed_move(1);
            continue;
        }
        if (c == 0x1b) {
            esc = 1;
            continue;
        }
        if (c == '\r' || c == '\n') {
            ed_write("\n", 1);
            ed_ready = true;
            return;
        }
        if (c == '\t') {
            ed_want = true;
            return;
        }
        if (c == 0x7f || c == 0x08) {
            ed_backspace();
            continue;
        }
        if (c == 0x03) {
            ed_quit = true;
            return;
        }
        if (c == 0x04) {
            if (ed_line.len == 0)
                ed_quit = true;
            continue;
        }
        if (c == 0x01) {
            ed_move(-(int)ed_pos);
            continue;
        }
        if (c == 0x05) {
            ed_move((int)(ed_line.len - ed_pos));
            continue;
        }
        if (c == 0x15) {
            ed_kill_line();
            continue;
        }
        if (c < 0x20)
            continue;
        ed_insert(c);
    }
}

/* 应用服务端补全应答：prompt / replace_from / replace_text / listing */
static void ed_apply_complete(const uint8_t *p, size_t n)
{
    size_t off = 0;
    const uint8_t *prompt = NULL, *repl = NULL, *listing = NULL;
    size_t plen = 0, rlen = 0, llen = 0;
    uint32_t from = 0;
    if (proto_get_blob(p, n, &off, &prompt, &plen) ||
        proto_get_u32(p, n, &off, &from) ||
        proto_get_blob(p, n, &off, &repl, &rlen) ||
        proto_get_blob(p, n, &off, &listing, &llen)) {
        ed_await = false;
        return;
    }

    if (from == 0xFFFFFFFFu) { /* 服务端限速：忽略本次，不改动输入 */
        ed_await = false;
        return;
    }

    /* 被替换区间是 [from, cursor)：尾部要从 cursor 起，否则会把原词重复一遍 */
    size_t old_pos = ed_pos;
    if (from > old_pos)
        from = (uint32_t)old_pos;
    size_t tail_off = old_pos;

    buf_t nb;
    buf_init(&nb);
    buf_append(&nb, ed_line.data, from);
    buf_append(&nb, repl, rlen);
    buf_append(&nb, ed_line.data + tail_off, ed_line.len - tail_off);
    buf_free(&ed_line);
    ed_line = nb;
    ed_pos = from + rlen;

    /* 只用物理回退（不能调 ed_move：它会一并改写 ed_pos，覆盖刚算好的逻辑光标） */
    for (size_t i = 0; i < old_pos; i++)
        ed_write("\b", 1);
    ed_write("\x1b[K", 3);
    if (llen) {
        /* 清单必须先换行输出，然后重画提示符与整行 */
        ed_write("\r\n", 2);
        ed_write((const char *)listing, llen);
        ed_write((const char *)prompt, plen);
    }
    /* 提示符已在屏幕上（未换行时）或刚写出（换行后），只画行内容 */
    ed_write((const char *)ed_line.data, ed_line.len);
    for (size_t i = ed_pos; i < ed_line.len; i++)
        ed_write("\b", 1);
    ed_await = false;
}

static void apply_mode(cmode_t m)
{
    if (m == g_mode)
        return;
    g_mode = m;
    /* 角色切换时丢弃终端输入队列中的残留按键：
     * 失去/获得执行权都不应让旧输入被当成命令行执行 */
    if (g_tty && (m == M_EXEC_IDLE || m == M_FOLLOWER))
        tcflush(STDIN_FILENO, TCIFLUSH);
    if (!g_tty)
        return;
    struct termios t = g_saved;
    if (m == M_FOLLOWER) {
        t.c_lflag &= ~ECHO;
        ed_on = false;
    } else if (m == M_RUN) {
        cfmakeraw(&t);
        t.c_oflag |= (OPOST | ONLCR); /* 保留输出换行处理，避免阶梯显示 */
        ed_on = false;
    } else {
        /* 执行者空闲：原始模式 + 自己回显，才能截获 Tab 做补全 */
        cfmakeraw(&t);
        t.c_oflag |= (OPOST | ONLCR);
        ed_on = true;
        if (!ed_inited) {
            buf_init(&ed_line);
            ed_inited = true;
        }
        ed_reset();
    }
    tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
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
    size_t off = 0;
    while (off < f.len) {
        ssize_t n = write(fd, f.data + off, f.len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            buf_free(&f);
            return -1;
        }
        off += (size_t)n;
    }
    buf_free(&f);
    return 0;
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

/* ---- 加入会话 ---- */

int client_join(const char *session, const char *name)
{
    char err[128];
    int fd = connect_session(session, err, sizeof err);
    if (fd < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED)
            fprintf(stderr, "会话 %s 不存在，请先运行: shshd %s\n", session,
                    session);
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

    buf_t in, linebuf;
    buf_init(&in);
    buf_init(&linebuf);
    uint32_t self_id = 0, exec_id = 0;
    bool welcome = false, quit_sent = false, pending_line = false;
    int rc = 0;

    for (;;) {
        /* 一次只提交一条命令行：服务端回应（RUN_BEGIN / 提示符 / ROLE…）后
         * 才提交缓冲中的下一条，避免运行期提交被丢弃 */
        if (g_mode == M_EXEC_IDLE && !pending_line && linebuf.len > 0) {
            uint8_t *nl = memchr(linebuf.data, '\n', linebuf.len);
            if (nl) {
                size_t ll = (size_t)(nl - linebuf.data);
                if (ll > 0 && linebuf.data[ll - 1] == '\r')
                    ll--;
                msg_t lm;
                msg_make_line(&lm, linebuf.data, ll);
                send_frame(fd, &lm);
                buf_free(&lm.payload);
                pending_line = true;
                buf_consume(&linebuf, (size_t)(nl - linebuf.data) + 1);
            }
        }

        /* 已提交命令行但服务端尚未回应前，暂停读取 stdin；
         * 运行相位中不门控（按键需直通 pty，Ctrl+C 才能中断命令） */
        bool gate_stdin = (pending_line && g_mode != M_RUN) || ed_await;

        /* 行编辑器就绪：提交；Tab：请求补全 */
        if (ed_on && ed_ready && !pending_line) {
            msg_t lm;
            msg_make_line(&lm, ed_line.data, ed_line.len);
            send_frame(fd, &lm);
            buf_free(&lm.payload);
            pending_line = true;
            ed_ready = false;
            buf_clear(&ed_line);
            ed_pos = 0;
        }
        if (ed_on && ed_want && !pending_line && !ed_await) {
            ed_want = false;
            /* 串行化：同一时刻只允许一个补全请求在飞（连按 Tab 只在回来后再发一次） */
            ed_await = true;
            ed_await_t0 = mono_ms();
            buf_t req;
            buf_init(&req);
            proto_put_blob(&req, ed_line.data, ed_line.len);
            proto_put_u32(&req, (uint32_t)ed_pos);
            msg_t cm;
            cm.type = C2S_COMPLETE;
            cm.payload = req;
            send_frame(fd, &cm);
            buf_free(&cm.payload);
        }
        if (ed_quit && !quit_sent) {
            ed_quit = false;
            msg_t q;
            msg_make_quit(&q);
            send_frame(fd, &q);
            buf_free(&q.payload);
            quit_sent = true;
        }

        struct pollfd pfds[3];
        int sidx = -1;
        pfds[0].fd = fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = sigfd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        int np = 2;
        if (!quit_sent && !gate_stdin) {
            pfds[2].fd = STDIN_FILENO;
            pfds[2].events = POLLIN;
            pfds[2].revents = 0;
            sidx = 2;
            np = 3;
        }

        /* 等待补全应答时周期性醒来，避免应答丢失后输入被永久门控 */
        long long now_ms = mono_ms();
        int wait_ms = welcome ? -1 : 10000;
        if (ed_await) {
            long long left = 2000 - (now_ms - ed_await_t0);
            wait_ms = left > 0 ? (int)left : 1;
        }
        int pr = poll(pfds, (nfds_t)np, wait_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            if (!welcome) {
                fprintf(stderr, "连接会话 %s 失败: 服务器无响应\n", session);
                rc = 1;
                break;
            }
            if (ed_await && mono_ms() - ed_await_t0 >= 2000)
                ed_await = false; /* 应答超时：解除门控，避免输入卡死 */
            continue;
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
                } else if (!quit_sent) {
                    msg_t q;
                    msg_make_quit(&q);
                    send_frame(fd, &q);
                    buf_free(&q.payload);
                    quit_sent = true;
                }
            }
        }

        if (sidx >= 0 && (pfds[sidx].revents & (POLLIN | POLLHUP))) {
            char chunk[4096];
            ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
            if (n == 0) { /* EOF（ctrl+d）：离开会话 */
                if (!quit_sent) {
                    msg_t q;
                    msg_make_quit(&q);
                    send_frame(fd, &q);
                    buf_free(&q.payload);
                    quit_sent = true;
                }
            } else if (n > 0) {
                if (ed_on) {
                    ed_feed((const uint8_t *)chunk, (size_t)n);
                } else if (g_mode == M_RUN) {
                    msg_t rm;
                    msg_make_raw(&rm, chunk, (size_t)n);
                    send_frame(fd, &rm);
                    buf_free(&rm.payload);
                } else if (g_mode == M_EXEC_IDLE) {
                    buf_append(&linebuf, chunk, (size_t)n);
                    if (linebuf.len > SH_MSG_MAX)
                        buf_clear(&linebuf);
                }
                /* 其余相位（无执行权 / 角色未知）：直接丢弃，不发送任何内容 */
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
                    goto done;
                }
                if (r == 0)
                    break;
                const uint8_t *data = NULL;
                size_t len = 0;
                switch (m.type) {
                case S2C_WELCOME: {
                    char ename[SH_NAME_MAX + 1];
                    if (msg_parse_welcome(&m, &self_id, &exec_id, ename,
                                          sizeof ename) == 0) {
                        /* 不做额外提示：提示符本身已说明当前执行者 */
                        welcome = true;
                        apply_mode(exec_id == self_id ? M_EXEC_IDLE
                                                     : M_FOLLOWER);
                    }
                    break;
                }
                case S2C_OUT:
                    msg_parse_bytes(&m, &data, &len);
                    if (len)
                        write_all(STDOUT_FILENO, data, len);
                    break;
                case S2C_CMD_ECHO:
                    msg_parse_bytes(&m, &data, &len);
                    if (len)
                        write_all(STDOUT_FILENO, data, len);
                    break;
                case S2C_RUN_BEGIN:
                    apply_mode(M_RUN);
                    break;
                case S2C_RUN_END:
                    /* 一条命令结束：相位门控复位，按当前执行权回到空闲态 */
                    pending_line = false;
                    apply_mode(exec_id == self_id ? M_EXEC_IDLE : M_FOLLOWER);
                    if (g_mode != M_EXEC_IDLE)
                        buf_clear(&linebuf);
                    break;
                case S2C_ROLE: {
                    uint32_t eid = 0;
                    if (msg_parse_role(&m, &eid) == 0) {
                        exec_id = eid;
                        apply_mode(eid == self_id ? M_EXEC_IDLE : M_FOLLOWER);
                        if (g_mode != M_EXEC_IDLE)
                            buf_clear(&linebuf);
                    }
                    break;
                }
                case S2C_COMPLETE:
                    msg_parse_bytes(&m, &data, &len);
                    ed_apply_complete(data, len);
                    break;
                case S2C_BYE: {
                    char reason[128] = "";
                    msg_parse_text(&m, reason, sizeof reason);
                    if (welcome) {
                        printf("\n[%s]\n", reason);
                        fflush(stdout);
                        buf_free(&m.payload);
                        rc = 0;
                        goto done;
                    }
                    fprintf(stderr, "%s\n", reason);
                    buf_free(&m.payload);
                    rc = 1;
                    goto done;
                }
                default:
                    break;
                }
                buf_free(&m.payload);
            }
        }
    }
done:
    buf_free(&in);
    buf_free(&linebuf);
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