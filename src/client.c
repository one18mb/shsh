#include "client.h"
#include "buf.h"
#include "common.h"
#include "osc633.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>

/*
 * shsh 客户端——统一文本协议（OSC 633）。
 *
 * 与 shshd 之间只有一条双向文本字节流（AF_UNIX 承载）：
 *
 *   上行（stdin）：连接建立后先发一行 "name\n" 文本握手，随后所有键入字节
 *     （含 ^C/^D）原样透传；终端尺寸用夹带在流里的
 *     ESC ] 633 ; W ; rows ; x cols ST 标记上报，shshd 剥离后转 TIOCSWINSZ。
 *
 *   下行（stdout）：终端文本 + OSC 633 带内标记。客户端扫描这些标记：
 *       D;s=<退出码>;u=<执行者>;w=<目录>  提示符边界/当前执行者
 *       A / B / C;<退出码>                 命令相位（无需客户端动作）
 *       E;reason=<原因>                    会话即将关闭（随后即 EOF）
 *     执行者身份以最新 D 标记里的 u= 与命令行给定的自身名字比较。
 *     TTY 模式标记原样透传给终端（不可见/被理解）；
 *     --plain 模式剥除标记，只留可见文本。
 *
 * 本地终端模式按角色切换：执行者 raw（^C/^D 作为字节上送，由 shshd
 * 按相位裁决）；跟随者 cooked 去回显，保留 ISIG/ICANON，让本地系统处理
 * ^C/^D——shshd 无响应时也能正常退出。
 */

#define OSC633     "\033]633;"
#define OSC_ST      "\033\\"

static struct termios g_saved;
static bool g_saved_ok;
static bool g_tty;
static bool g_plain;
static bool g_background; /* 后台端点：不读终端、不因 stdin EOF 离场 */

/* 自身名字（命令行给定）与最新 D 标记里的执行者名字 */
static char g_self[SH_NAME_MAX + 1];
static char g_exec[SH_NAME_MAX + 1];
static bool g_have_role;

static bool is_follower(void)
{
    return g_have_role && strcmp(g_exec, g_self) != 0;
}

static void restore_tio(void)
{
    if (g_saved_ok && g_tty)
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_saved);
}

static void apply_stdio_mode(bool follower)
{
    if (!g_tty || !g_saved_ok)
        return;
    struct termios t = g_saved;
    if (follower) {
        t.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHOCTL);
        t.c_lflag |= ICANON | ISIG;
        t.c_oflag |= (OPOST | ONLCR);
    } else {
        cfmakeraw(&t);
        t.c_oflag |= (OPOST | ONLCR);
    }
    tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
}

/* SIGINT/SIGTERM/SIGHUP：恢复终端后退出（真实 handler，即使主循环卡住也生效） */
static void channel_unlink(void);

static void on_signal(int sig)
{
    (void)sig;
    channel_unlink(); /* unlink 异步信号安全：清掉端点文件（stale .pid 无害） */
    restore_tio();
    _exit(0);
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

/* ---- 上行缓冲 ----
 *
 * socket 设为非阻塞，所有上行（握手/尺寸/键入）先进入 g_up，再随 poll 的
 * POLLOUT 补齐。这样"写 socket"永不阻塞，client 才能在注入大输入的同时
 * 持续读下行——否则写阻塞会让服务端输出积压、触发断连（旧 bug）。
 * g_up 达高水位时暂停读 stdin/FIFO，把背压经内核管道/FIFO 反向传给生产者
 * （终端或 send），端到端限流，支持任意大输入平滑推进。 */
static buf_t g_up;
static int g_sock = -1;
#define UP_HIGH_WATER (1u << 20)

static void up_flush(void)
{
    while (g_up.len > 0) {
        ssize_t w = write(g_sock, g_up.data, g_up.len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return; /* EAGAIN（等 POLLOUT）或 EPIPE（交读侧 EOF 处理） */
        }
        buf_consume(&g_up, (size_t)w);
    }
}

static void up_append(const void *p, size_t n)
{
    if (!n)
        return;
    buf_append(&g_up, p, n);
    up_flush();
}

/* ---- 连接 ---- */

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

/* ---- 上行：文本握手与尺寸标记 ---- */

static void send_handshake(const char *name)
{
    up_append(name, strlen(name));
    up_append("\n", 1);
}

static void send_winch(unsigned rows, unsigned cols)
{
    char buf[48];
    int n = snprintf(buf, sizeof buf,
                     OSC633 "W;%u;x%u" OSC_ST, rows, cols);
    if (n > 0)
        up_append(buf, (size_t)n);
}

/* 转发一批 stdin 字节：普通字节直接透传；^C/^D 单独切分。
 *  - 执行者：^C/^D 一律上送，由 shshd 按相位裁决（运行态可能转交前台任务）
 *  - 跟随者：其前台即 shsh 自身——^C/^D 直接本地结束客户端，不等 EOF
 *    （tty 场景下系统已先行处理，此处是无 tty 管道时的兜底） */
static void forward_input(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && p[i] != 0x03 && p[i] != 0x04)
            i++;
        if (i > start)
            up_append(p + start, i - start);
        if (i == n)
            break;
        uint8_t ctl = p[i++];
        if (is_follower()) {
            channel_unlink();
            restore_tio();
            _exit(0);
        }
        up_append(&ctl, 1);
    }
}

/* ---- 下行：OSC 633 扫描器 ----
 *
 * 扫描 "\x1b]633;...ST/BEL"：标记本身在普通模式原样写给 stdout（终端忽略
 * 或原生理解），--plain 模式剥除；同时解析 D 的 u= 切换终端模式、E 的
 * reason= 作离开原因。其它转义序列（CSI、其它 OSC）一律原样放行。
 * 切分与跨读拼齐由共享扫描器 osc633.c 负责。 */

/* 下行 OSC 633 扫描复用共享扫描器（osc633.c）：本文件只提供角色/相位回调。 */
typedef struct {
    osc633_scan_t scan;
} rx_t;

static bool g_joined; /* 已收到服务器任意字节（握手通过/进入会话） */
static bool g_quit;
static char g_bye_reason[256];

/* 无 tty（管道/重定向）场景的 EOF 相位跟踪：
 * stdin 一次性灌入并立即 EOF 时，不能立刻把 EOF 变 ^D——命令可能还在
 * canonical 缓冲里没交付。改为：EOF 后等最后一条命令回归提示符（D），
 * 输出静默一小段，再发 ^D 离场。tty 下由人按键，不走这套。 */
static bool g_stdin_eof;       /* stdin 已读到 EOF（管道写尽） */
static bool g_cmd_running;     /* 已见 A、尚未见命令结束的 D */
static bool g_eof_seen_d;      /* stdin EOF 之后已见过至少一枚 D */
static long g_eof_at_ms;       /* stdin EOF 到达时刻（ms） */
static long g_last_recv_ms;    /* 最近一次从 socket 收到字节的时刻（ms） */
static bool g_leaving;         /* 已发出离场 ^D，等服务端 E/EOF */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 无 tty 且 stdin 已 EOF 时，离场需同时满足：
 *   身份已确认（见过 D）+ 当前无在跑命令 + EOF 后见过 D + 输出静默
 * 返回 true 表示此刻该发 ^D（执行者）或直接退出（跟随者）。 */
#define EOF_QUIET_MS 150   /* 回归 D 后再等的静默窗口，排空尾随输出 */
#define EOF_MAX_WAIT_MS 30000 /* 交互程序（如漏发 ^D 的 cat）兜底，避免永久挂 */

static void emit_bytes(const uint8_t *p, size_t n);
static void emit_live(const uint8_t *p, size_t n);

/* 解析 D;s=..;u=<执行者>;w=..：只需 u= 决定本地终端模式 */
static void on_mark_d(const uint8_t *q, size_t n)
{
    char u[SH_NAME_MAX + 1] = "";
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && q[e] != ';')
            e++;
        if (e - i >= 2 && q[i + 1] == '=' && q[i] == 'u') {
            size_t vlen = e - i - 2;
            if (vlen >= sizeof u)
                vlen = sizeof u - 1;
            memcpy(u, q + i + 2, vlen);
            u[vlen] = '\0';
        }
        i = (e < n) ? e + 1 : e;
    }
    if (u[0]) {
        snprintf(g_exec, sizeof g_exec, "%s", u);
        g_have_role = true;
        apply_stdio_mode(is_follower());
    }
}

/* 解析 E;reason=<原因>：记录原因、安排退出（字节排空后进程结束） */
static void on_mark_e(const uint8_t *q, size_t n)
{
    const char key[] = "reason=";
    size_t klen = sizeof key - 1;
    char reason[sizeof g_bye_reason] = "";
    if (n >= klen && memcmp(q, key, klen) == 0) {
        size_t rlen = n - klen;
        if (rlen >= sizeof reason)
            rlen = sizeof reason - 1;
        memcpy(reason, q + klen, rlen);
        reason[rlen] = '\0';
    }
    snprintf(g_bye_reason, sizeof g_bye_reason, "%s", reason);
    g_quit = true;
}

/* 共享扫描器回调：可见字节（含非 633 的其它 OSC）一律作真实终端内容放行。 */
static void scan_on_raw(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    emit_live(p, n);
}

/* 共享扫描器回调：处理一条完整 OSC；633 之外（如窗口标题）原样放行。 */
static void scan_on_mark(void *ctx, int is633, char code,
                         const uint8_t *args, size_t args_len,
                         const uint8_t *raw, size_t rawlen)
{
    (void)ctx;
    if (!is633) {
        emit_live(raw, rawlen);
        return;
    }
    if (code == 'A') {
        g_cmd_running = true; /* 命令交付：进入运行相位 */
        g_joined = true;
    } else if (code == 'D') {
        on_mark_d(args, args_len);
        g_joined = true;       /* 收到提示符：握手已通过 */
        g_cmd_running = false; /* 提示符回归：上一条命令已结束 */
        if (g_stdin_eof)
            g_eof_seen_d = true;
    } else if (code == 'E') {
        /* 握手前的 E = 服务器拒绝（名字非法/重名等），故不置 joined */
        on_mark_e(args, args_len);
    } else {
        /* B/C/P 等：无需客户端动作，但同样证明会话活跃 */
        g_joined = true;
    }
    /* 标记本身在普通模式透传给终端/agent（不可见）；--plain 全部剥除 */
    if (!g_plain)
        emit_bytes(raw, rawlen);
}

/* 发出可见字节（plain 模式下剥除标记后，其余输出都经此）。 */
static void emit_bytes(const uint8_t *p, size_t n)
{
    if (n)
        write_all(STDOUT_FILENO, p, n);
}

/* 真实终端内容字节（非 633 标记）：同时确认会话已握手通过 */
static void emit_live(const uint8_t *p, size_t n)
{
    emit_bytes(p, n);
    g_joined = true;
}

/* ---- 本地 send 通道：FIFO 注入 + 输出 tee ----
 *
 * shsh 是唯一的 shshd 客户端，同时充当 send 的本地端点：send 往 FIFO 写命令，
 * shsh 把它与终端输入合并送往 shshd；shshd 的输出除写终端外，再原样 tee 到
 * .out，供 send 读增量。FIFO 以 O_RDWR 自持写端（永不 EOF）。全部 best-effort：
 * 建不出（如无 HOME）就退化为纯交互，不影响主流程。 */

static int g_fifo_fd = -1;
static int g_out_fd = -1;
static bool g_channel_tried;   /* 已尝试建端点（无论成败） */
static bool g_channel_live;    /* 端点文件已就绪 */
static bool g_paths_ok;        /* 路径已算出（供退出时 unlink） */
static char g_in_path[PATH_MAX];
static char g_out_path[PATH_MAX];
static char g_pid_path[PATH_MAX];
static char g_cursor_path[PATH_MAX];
static char g_log_path[PATH_MAX];
static buf_t g_pre_out;        /* 建端点前已收到的服务器字节，就绪后补写 */
#define CHANNEL_PRE_MAX (1u << 20)

static bool build_paths(const char *session, const char *name)
{
    const char *home = getenv("HOME");
    char base[PATH_MAX - 16];
    if (!home || !*home)
        return false;
    int r = snprintf(base, sizeof base, "%s/.shsh/%s__%s", home, session,
                     name);
    if (r < 0 || (size_t)r >= sizeof base)
        return false;
    snprintf(g_in_path, sizeof g_in_path, "%s.in", base);
    snprintf(g_out_path, sizeof g_out_path, "%s.out", base);
    snprintf(g_pid_path, sizeof g_pid_path, "%s.pid", base);
    snprintf(g_cursor_path, sizeof g_cursor_path, "%s.cursor", base);
    snprintf(g_log_path, sizeof g_log_path, "%s.log", base);
    g_paths_ok = true;
    return true;
}

static void channel_open(const char *session, const char *name)
{
    g_channel_tried = true;
    if (!build_paths(session, name))
        return;
    const char *home = getenv("HOME");
    char dir[PATH_MAX - 16];
    snprintf(dir, sizeof dir, "%s/.shsh", home);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return;
    if (mkfifo(g_in_path, 0600) != 0 && errno != EEXIST)
        return;
    g_fifo_fd = open(g_in_path, O_RDWR | O_NONBLOCK);
    if (g_fifo_fd < 0)
        return;
    g_out_fd = open(g_out_path,
                    O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (g_out_fd < 0) {
        close(g_fifo_fd);
        g_fifo_fd = -1;
        return;
    }
    int pf = open(g_pid_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pf >= 0) {
        char b[32];
        int n = snprintf(b, sizeof b, "%d\n", (int)getpid());
        write_all(pf, b, (size_t)n);
        close(pf);
    }
    int cf = open(g_cursor_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cf >= 0) {
        write_all(cf, "0\n", 2); /* .out 已截断，游标归零 */
        close(cf);
    }
    if (g_pre_out.len)
        write_all(g_out_fd, g_pre_out.data, g_pre_out.len);
    buf_clear(&g_pre_out);
    g_channel_live = true;
}

/* 仅 unlink（异步信号安全）：供信号/快速退出路径清理端点文件 */
static void channel_unlink(void)
{
    if (!g_paths_ok)
        return;
    unlink(g_in_path);
    unlink(g_out_path);
    unlink(g_pid_path);
    unlink(g_cursor_path);
    unlink(g_log_path);
}

static void channel_close(void)
{
    if (g_fifo_fd >= 0) {
        close(g_fifo_fd);
        g_fifo_fd = -1;
    }
    if (g_out_fd >= 0) {
        close(g_out_fd);
        g_out_fd = -1;
    }
    channel_unlink();
}

/* ---- 加入会话 ---- */

int client_join(const char *session, const char *name, bool plain,
                bool background)
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

    snprintf(g_self, sizeof g_self, "%s", name);
    g_plain = plain;
    g_background = background;

    /* socket 非阻塞：写走 g_up 缓冲，永不阻塞写，避免与读下行互锁 */
    g_sock = fd;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    buf_init(&g_up);

    g_tty = isatty(STDIN_FILENO) == 1 && !background;
    if (g_tty && tcgetattr(STDIN_FILENO, &g_saved) == 0) {
        g_saved_ok = true;
        atexit(restore_tio);
        /* 首枚 D 标记到达前先按执行者 raw；收到 D 后按 u= 重设 */
        apply_stdio_mode(false);
    }

    /* 文本握手：首行 name；随后上报初始终端尺寸 */
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    ws.ws_row = 24;
    ws.ws_col = 80;
    if (g_tty)
        ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    send_handshake(name);
    send_winch(ws.ws_row, ws.ws_col);

    /* ^C/^D 语义由本地终端模式决定：SIGINT/SIGTERM/SIGHUP 用真实 handler
     * （跟随者 ^C 由系统发 SIGINT、外部 kill 亦然），即使主循环卡住也能恢复
     * 终端并退出；SIGWINCH 走 signalfd 以便读尺寸。 */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGWINCH);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sigfd < 0) {
        fprintf(stderr, "signalfd 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    rx_t rx;
    memset(&rx, 0, sizeof rx);
    osc633_scan_init(&rx.scan);
    buf_init(&g_pre_out);
    bool quit = false;
    int rc = 0;
    g_last_recv_ms = now_ms();

    while (!quit) {
        bool up_full = g_up.len >= UP_HIGH_WATER;
        struct pollfd pfds[4];
        pfds[0].fd = fd;
        pfds[0].events = POLLIN | (g_up.len ? POLLOUT : 0);
        pfds[0].revents = 0;
        pfds[1].fd = sigfd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        /* 收到首枚 D（加入提示符）前不读 stdin：D 是"已进入会话、shell
         * 就绪"的同步点。管道/重定向下 OS 缓冲里积压的命令因此严格在转正
         * 之后才放行，避免整批键入抢在提示符前被 ^D 带走。数据留在 OS
         * 管道/tty 缓冲里不会丢（tty 下人此刻也还没按键）。
         * 上行缓冲满时也暂停读 stdin/FIFO，把背压反向传给生产者。 */
        pfds[2].fd = (!g_background && g_have_role && !g_stdin_eof &&
                      !up_full) ? STDIN_FILENO : -1;
        pfds[2].events = POLLIN;
        pfds[2].revents = 0;
        /* send 通道 FIFO：就绪后才挂；-1 时 poll 忽略。其字节与终端输入
         * 合并送往 shshd，是"用户 + send 共同作用于输入"的输入边。 */
        pfds[3].fd = up_full ? -1 : g_fifo_fd;
        pfds[3].events = POLLIN;
        pfds[3].revents = 0;
        /* 无 tty 且 stdin 已 EOF：即便 socket 暂时无事件也要周期醒来，
         * 检查"命令回归 D + 输出静默"的离场条件；其余情况永久等 */
        int wait_ms = g_joined ? -1 : 10000;
        if (!g_tty && g_stdin_eof && !g_leaving)
            wait_ms = 50;
        int pr = poll(pfds, 4, wait_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            if (!g_joined) {
                fprintf(stderr, "连接会话 %s 失败: 服务器无响应\n", session);
                rc = 1;
                break;
            }
            /* 仅无 tty EOF 离场轮询会走到这（超时）；下方统一判定离场 */
        }

        if (pfds[0].revents & POLLOUT)
            up_flush();

        if (pfds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sigfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGWINCH && g_tty && g_joined) {
                    struct winsize nw;
                    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &nw) == 0)
                        send_winch(nw.ws_row, nw.ws_col);
                }
            }
        }

        if (pfds[2].revents & (POLLIN | POLLHUP)) {
            uint8_t chunk[4096];
            ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
            if (n == 0) {
                /* stdin 写尽。tty：人按的 ^D，立即按身份裁决；无 tty（管道/
                 * 重定向）：命令和 EOF 常一起涌来，先标记，等命令回归 D 再
                 * 离场（见循环末尾的 eof_should_leave），避免命令还没交付
                 * 就被 ^D 带走。 */
                g_stdin_eof = true;
                g_eof_at_ms = now_ms();
                if (g_tty && is_follower()) {
                    restore_tio();
                    _exit(0);
                }
                if (g_tty) {
                    /* 执行者 tty EOF：发 ^D 交服务端裁决，停止监听 stdin */
                    uint8_t eot = 0x04;
                    up_append(&eot, 1);
                }
            } else if (n > 0) {
                forward_input(chunk, (size_t)n);
            }
        }

        /* send 通道 FIFO：注入字节与终端输入同路送往 shshd */
        if (g_fifo_fd >= 0 && (pfds[3].revents & (POLLIN | POLLERR))) {
            for (;;) {
                uint8_t b[4096];
                ssize_t n = read(g_fifo_fd, b, sizeof b);
                if (n > 0) {
                    up_append(b, (size_t)n);
                    continue;
                }
                break; /* EAGAIN/EINTR：本批排空 */
            }
        }

        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            uint8_t chunk[65536];
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n <= 0) {
                if (g_joined)
                    fprintf(stderr, "连接已断开\n");
                rc = g_joined ? 0 : 1;
                break;
            }
            g_last_recv_ms = now_ms();
            /* tee 到 .out（端点就绪前先缓存，就绪后补写） */
            if (g_channel_live)
                write_all(g_out_fd, chunk, (size_t)n);
            else if (!g_channel_tried)
                buf_append(&g_pre_out, chunk, (size_t)n);
            osc633_scan_feed(&rx.scan, chunk, (size_t)n, NULL,
                             scan_on_raw, scan_on_mark);
            /* 确认加入后再建端点，避免重名被拒时覆盖他人端点文件 */
            if (!g_channel_tried) {
                if (g_pre_out.len > CHANNEL_PRE_MAX)
                    g_channel_tried = true; /* 放弃建端点，避免无界缓存 */
                else if (g_joined)
                    channel_open(session, name);
            }
            if (g_quit) {
                if (g_joined) {
                    char shown[sizeof g_bye_reason + 8];
                    snprintf(shown, sizeof shown, "\n[%s]\n", g_bye_reason);
                    write_all(STDOUT_FILENO, shown, strlen(shown));
                    rc = 0;
                } else {
                    fprintf(stderr, "%s\n", g_bye_reason);
                    rc = 1;
                }
                quit = true;
            }
        }

        /* 无 tty 管道/重定向：stdin 写尽后，等命令跑完再优雅离场。
         *   follower        → 身份确认即可立即退出（不影响执行者）
         *   执行者          → 等最后一条命令回归 D 且输出静默，发 ^D 离场，
         *                     随后等服务端 E 标记（g_quit）/EOF
         *   超时兜底        → 漏发 ^D 的交互程序（cat 等）会一直占运行相位，
         *                     超过 EOF_MAX_WAIT_MS 直接断开（前台组随之 SIGHUP） */
        if (!g_background && !g_tty && g_stdin_eof && !g_leaving &&
            !g_quit && g_have_role) {
            long since_eof = now_ms() - g_eof_at_ms;
            long quiet = now_ms() - g_last_recv_ms;
            bool settled = !g_cmd_running && g_eof_seen_d &&
                           quiet >= EOF_QUIET_MS;
            if (is_follower() || settled) {
                if (!is_follower()) {
                    uint8_t eot = 0x04;
                    up_append(&eot, 1);
                    g_leaving = true;
                } else {
                    quit = true; /* follower 直接结束，不惊动会话 */
                }
            } else if (since_eof > EOF_MAX_WAIT_MS) {
                quit = true; /* 兜底：前台程序不退出，放弃等待，rc=0 */
                rc = 0;
            }
        }
    }
    channel_close();
    buf_free(&g_pre_out);
    buf_free(&g_up);
    restore_tio();
    close(sigfd);
    close(fd);
    return rc;
}

/* ---- 停止端点（logout） ---- */

int client_logout(const char *session, const char *name)
{
    if (!build_paths(session, name)) {
        fprintf(stderr, "无法定位端点文件（HOME 未设置）\n");
        return 1;
    }
    int pid = 0;
    FILE *f = fopen(g_pid_path, "r");
    if (f) {
        if (fscanf(f, "%d", &pid) != 1)
            pid = 0;
        fclose(f);
    }
    if (pid > 0 && kill(pid, 0) == 0) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 30 && kill(pid, 0) == 0; i++)
            usleep(100 * 1000);
        if (kill(pid, 0) == 0)
            kill(pid, SIGKILL);
    }
    channel_unlink();
    fprintf(stderr, "已停止端点 %s（%s）\n", name, session);
    return 0;
}

/* ---- 列出会话（独立纯文本短连接：发 list 行，读到 EOF） ---- */

static int list_one(const char *session, bool leading_blank)
{
    char err[128];
    int fd = connect_session(session, err, sizeof err);
    if (fd < 0)
        return -1;
    if (write_all(fd, "list\n", 5) != 0) {
        close(fd);
        return -1;
    }
    bool got = false;
    if (leading_blank)
        write_all(STDOUT_FILENO, "\n", 1);
    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 3000);
        if (pr <= 0)
            break;
        char chunk[8192];
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r <= 0)
            break;
        write_all(STDOUT_FILENO, chunk, (size_t)r);
        got = true;
    }
    close(fd);
    return got ? 0 : -1;
}

int client_list(const char *session)
{
    if (session) {
        if (list_one(session, false) != 0) {
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
        if (list_one(sess, found > 0) == 0)
            found++;
        else
            fprintf(stderr, "注意: %s 已失效\n", names[i]);
        free(names[i]);
    }
    if (!found)
        printf("无活动会话\n");
    return 0;
}
