#include "server.h"
#include "buf.h"
#include "common.h"
#include "proto.h"
#include "pty.h"
#include "shsh_shell.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <sys/wait.h>
#include <termios.h>

#define EPOLL_MAX_EVENTS 64
#define CLIENT_OUT_LIMIT (1u << 20) /* 单客户端输出积压上限 */
#define CLIENT_IN_LIMIT (SH_MSG_MAX * 4)
/* 待写入 pty master 的用户输入上限（高水位）：达此值即暂停读客户端 socket，
 * 由 AF_UNIX 反向施加背压（见下方读取循环与 pty_flush_input）。 */
#define PTY_IN_HIGH_WATER (1u << 20)
/* pty master 的 POLLOUT 恒为真（不反映 slave 侧输入队列余量），无法用作
 * 续写唤醒；改以该周期的 epoll 超时轮询续写积压输入。 */
#define PTY_IN_POLL_MS 10
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

/* executor_id == 0 表示由 shshd（系统虚拟用户）持有执行权 */
#define SH_EXEC_SHSHD 0u
#define SHSHD_NAME "shshd"

/* shell 层同步钩子（定义在 dash 层 input.c / jobs.c，见 dash_decision.md 6）。
 * 会话层在 session_start 里把自己注册进去，把 dash 的"等命令 / 等前台作业"
 * 变成对会话事件循环的同步驱动，单线程、不异步。 */
extern int (*shsh_cmd_wait)(int);
extern void (*shsh_job_wait)(void);
/* 当前前台作业进程组（定义在 dash 层 jobs.c，见 jobs.h）：执行者掉线 / 会话
 * 关停时整组 SIGHUP */
extern pid_t shsh_fg_pgid;
/* 上一条命令的退出状态（dash 层 error.c 的 exitstatus）：RUN_END 携带 */
extern int exitstatus;
/* 最近一次提示符：1=PS1（新命令边界），2=PS2（多行命令续行，见 parser.h）。
 * 会话层据此只在 PS1 拦截 to/exit/help、只在 PS1 注入 SHSH_USER，
 * 避免把多行命令中路过的 exit/续行当作会话级指令处理。 */
extern int whichprompt;

typedef struct client {
    int fd;
    uint32_t id;
    char name[SH_NAME_MAX + 1];
    bool has_name;
    bool want_close; /* 输出排空后关闭 */
    bool out_armed;  /* epoll 已注册 EPOLLOUT */
    bool dead;
    uint16_t rows, cols;
    buf_t in, out;
    struct client *next;
} client_t;

struct session {
    char name[SH_NAME_MAX + 1];
    char sockpath[108];
    int listen_fd;
    int epoll_fd;
    int sig_fd;
    int pty_master;
    int pty_slave;
    bool pty_tio_ok;
    struct termios pty_tio;
    /* 命令管道：[0]=shell 层命令源读端（>=10），[1]=会话层写端 */
    int cmd_pipe[2];
    bool slave_armed;   /* pty slave 是否已注册进 epoll */
    bool running;       /* 前台命令执行中（两个钩子切换，供 ^C/相位判断） */
    bool phase_run;     /* 已注入命令、尚未回空闲（相位） */
    bool abort_cmd;     /* 需放弃未完成的多行命令（执行者中途离开，见 srv_cmd_wait） */
    client_t *clients;  /* 按连接顺序，头部 = 最早加入 */
    uint32_t next_id;
    uint32_t executor_id;  /* 0=shshd；否则为持有执行权的 client id（用于 fg pgid） */
    bool stopping;
    /* 最近的提示符片段（含 OSC 633 D 标记），供新加入者单播看到当前提示符 */
    char ps_cache[512];
    size_t ps_cache_len;
    /* 空闲态从 pty slave 读到的 canonical 行缓冲（可能一次读到多行） */
    char cmd_buf[4096];
    size_t cmd_len;
    /* 待写入 pty master 的用户输入：master 为非阻塞、其输入队列容量有限
     * （约 11KB），单次写入会短写/无进展，故积压于此、在会话循环中续写，
     * 确保大块输入不丢字节 */
    buf_t pty_in;
};

/* 钩子无 context，经此全局取得当前会话（单会话单进程） */
static session_t *g_session;

static void client_disconnect(session_t *s, client_t *c, bool unused_err);
static bool flush_out(session_t *s, client_t *c);
static void kill_foreground_job(session_t *s);
static void cmd_to(session_t *s, client_t *c, const char *target);
static void request_leave(session_t *s, client_t *c);
static int srv_cmd_wait(int fd);
static void srv_job_wait(void);
static void srv_read_wait(void);

/* ---- 基础工具 ---- */

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_sun_path(struct sockaddr_un *a, const char *path)
{
    snprintf(a->sun_path, sizeof a->sun_path, "%s", path);
}

/* 丢弃当前执行者"未交付"的键入：pty 行规程里尚未成行的半行（如 heredoc/
 * 续行中输入到一半的字节）与已注入命令管道、shell 尚未读走的字节，以及
 * 会话层累加缓冲。执行者离开时调用——否则这些残留会被下一位执行者的
 * 回车"补全"成命令执行（归属错乱）。 */
static void discard_pending_input(session_t *s)
{
    if (s->pty_tio_ok)
        tcflush(s->pty_slave, TCIFLUSH); /* 仅清输入半行，不丢已产生的输出 */
    buf_clear(&s->pty_in);               /* 丢弃尚未写入 master 的输入 */
    s->cmd_len = 0;
    struct pollfd p = {.fd = s->cmd_pipe[0], .events = POLLIN};
    char buf[256];
    while (s->cmd_pipe[0] >= 0 && poll(&p, 1, 0) == 1 &&
           (p.revents & POLLIN)) {
        if (read(s->cmd_pipe[0], buf, sizeof buf) <= 0)
            break;
    }
}

static client_t *client_find(session_t *s, int fd)
{
    for (client_t *c = s->clients; c; c = c->next)
        if (c->fd == fd)
            return c;
    return NULL;
}

static client_t *client_by_id(session_t *s, uint32_t id)
{
    for (client_t *c = s->clients; c; c = c->next)
        if (c->has_name && c->id == id)
            return c;
    return NULL;
}

/* 最早加入的在线用户（id != skip_id） */
static client_t *first_named_client(session_t *s, uint32_t skip_id)
{
    for (client_t *c = s->clients; c; c = c->next)
        if (c->has_name && c->id != skip_id)
            return c;
    return NULL;
}

static client_t *client_by_name(session_t *s, const char *name)
{
    for (client_t *c = s->clients; c; c = c->next)
        if (c->has_name && strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

static void client_add(session_t *s, int fd)
{
    client_t *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->fd = fd;
    buf_init(&c->in);
    buf_init(&c->out);
    if (!s->clients) {
        s->clients = c;
    } else {
        client_t *t = s->clients;
        while (t->next)
            t = t->next;
        t->next = c;
    }
}

static int client_update_events(session_t *s, client_t *c)
{
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP | (c->out_armed ? EPOLLOUT : 0);
    ev.data.fd = c->fd;
    return epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* ---- 发送 ---- */

static void client_queue(session_t *s, client_t *c, const buf_t *frame,
                         bool force)
{
    if (c->want_close)
        return;
    if (!c->has_name && !force)
        return;
    buf_append(&c->out, frame->data, frame->len);
    if (c->out.len > CLIENT_OUT_LIMIT) {
        log_error("客户端 %s 输出积压超限，断开", c->name);
        c->want_close = true;
    }
    if (!c->out_armed) {
        c->out_armed = true;
        client_update_events(s, c);
    }
}

static void broadcast_frame(session_t *s, const buf_t *frame)
{
    for (client_t *c = s->clients; c; c = c->next)
        client_queue(s, c, frame, false);
}

static void send_msg(session_t *s, client_t *c, const msg_t *m, bool force)
{
    buf_t f;
    buf_init(&f);
    if (msg_encode(m, &f) == 0)
        client_queue(s, c, &f, force);
    buf_free(&f);
}

static void broadcast_msg(session_t *s, const msg_t *m)
{
    buf_t f;
    buf_init(&f);
    if (msg_encode(m, &f) == 0)
        broadcast_frame(s, &f);
    buf_free(&f);
}

/* 广播一段文本（所有输出统一走 S2C_OUT） */
static void out_text(session_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void out_text(session_t *s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    msg_t m;
    msg_make_out(&m, buf, strlen(buf));
    broadcast_msg(s, &m);
    buf_free(&m.payload);
}

static const char *executor_name(session_t *s)
{
    if (s->executor_id == SH_EXEC_SHSHD)
        return SHSHD_NAME;
    client_t *c = client_by_id(s, s->executor_id);
    return c ? c->name : SHSHD_NAME;
}

/* 当前工作目录基名。cwd 由 shell 层维护（dash cd 即 chdir 本进程），
 * 会话层直接用 getcwd 读取。 */
static void cwd_basename(char *out, size_t sz)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        snprintf(out, sz, "?");
        return;
    }
    const char *b = strrchr(cwd, '/');
    const char *src = b ? (b[1] ? b + 1 : "/") : cwd;
    snprintf(out, sz, "%s", src);
}

/* 合成执行者提示符（"<name>:<cwd>$ "）。仅在运行态发生执行权变更时使用：
 * shell 正忙于前台作业、不会立刻打印 PS1，会话层先广播一枚带新执行者名的
 * 提示符让移交可见；空闲时则改由注入 SHSH_USER 让 shell 自己打印。 */
static void compose_prompt(session_t *s, char *out, size_t sz)
{
    char w[300];
    cwd_basename(w, sizeof w);
    snprintf(out, sz, "%s:%s$ ", executor_name(s), w);
}

/* shshd 执行 to：唯一的系统广播。显示为新行上的 shshd 命令行 */
static void sys_to_line(session_t *s, const char *target)
{
    char w[300];
    cwd_basename(w, sizeof w);
    out_text(s, "\n%s:%s$ to %s\n", SHSHD_NAME, w, target);
}

static void broadcast_role(session_t *s)
{
    msg_t m;
    msg_make_role(&m, s->executor_id); /* 0 = shshd，客户端据此进入只读态 */
    broadcast_msg(s, &m);
    buf_free(&m.payload);
}

/* RUN_BEGIN/RUN_END 相位（见 dash_decision.md 3.6）：普通行交付执行时
 * 广播 RUN_BEGIN；shell 回到空闲（下一条命令等待）时携状态广播 RUN_END。 */
static void run_begin(session_t *s)
{
    if (s->phase_run)
        return;
    s->phase_run = true;
    msg_t m;
    msg_make_run_begin(&m);
    broadcast_msg(s, &m);
    buf_free(&m.payload);
}

static void run_end(session_t *s, int status)
{
    if (!s->phase_run)
        return;
    s->phase_run = false;
    msg_t m;
    msg_make_run_end(&m, status);
    broadcast_msg(s, &m);
    buf_free(&m.payload);
}

/* ---- 断开 ---- */

static bool flush_out(session_t *s, client_t *c)
{
    while (c->out.len > 0) {
        ssize_t n = write(c->fd, c->out.data, c->out.len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            client_disconnect(s, c, false);
            return false;
        }
        buf_consume(&c->out, (size_t)n);
    }
    if (c->out.len == 0) {
        c->out_armed = false;
        if (c->want_close) {
            client_disconnect(s, c, false);
            return false;
        }
        client_update_events(s, c);
    }
    return true;
}

static void update_user(session_t *s);

static void client_disconnect(session_t *s, client_t *c, bool unused_err)
{
    (void)unused_err;
    if (c->dead)
        return;
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    client_t **pp = &s->clients;
    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;

    if (c->has_name && c->id == s->executor_id) {
        /* 前台任务的生命周期绑定在执行者上：执行者一离开，正在跑的前台作业
         * 一律整组挂断（含 TUI），并放弃 shell 当前命令——运行中的复合命令
         * （for/while）要继续下一轮、多行未完成（PS2）要被补完，都不允许。 */
        if (s->running)
            kill_foreground_job(s);
        /* 丢弃离开者未交付的键入，别让它被下一位执行者"补全"执行 */
        discard_pending_input(s);
        if (s->running || whichprompt == 2)
            s->abort_cmd = true;
        client_t *nxt = first_named_client(s, 0);
        /* 立即更新 shell 的 SHSH_USER：之后（含 abort 后）shell 打出的 PS1
         * 就直接是新执行者名，不再有"旧名过渡提示符" */
        shsh_shell_set_user(nxt ? nxt->name : SHSHD_NAME);
        if (nxt) {
            s->executor_id = nxt->id;
            sys_to_line(s, nxt->name);
            broadcast_role(s);
            update_user(s); /* 让 shell 打出带新执行者名的提示符 */
        } else {
            s->executor_id = SH_EXEC_SHSHD;
            broadcast_role(s);
            update_user(s);
        }
    }
    c->dead = true;
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);
}

static void flush_all(session_t *s)
{
    for (int guard = 0; guard < 100000; guard++) {
        client_t *c = NULL;
        for (client_t *t = s->clients; t; t = t->next)
            if (t->out.len > 0) {
                c = t;
                break;
            }
        if (!c)
            return;
        flush_out(s, c);
    }
    log_error("关闭会话: 部分客户端未能刷新输出");
}

/* ---- pty 输出与前台作业 ---- */

static void emit_out(session_t *s, const uint8_t *p, size_t n)
{
    if (!n)
        return;
    msg_t m;
    msg_make_out(&m, p, n);
    broadcast_msg(s, &m);
    buf_free(&m.payload);
}

/* 过滤一段 master 输出：剥离提示符 sentinel（SOH=0x01/STX=0x02，终端不可见
 * 且 dash 原样输出），并把提示符串缓存下来供新加入者单播（§3.5/3.6）。
 * 其余字节顺序不变地广播出去。跨读的半个提示符由常驻的 in_ps 状态承接。 */
/* pty master 侧输出：行规程回显、shell 输出（含 OSC 633 标记）、前台命令
 * 输出统一在此广播给所有客户端。统一文本协议下不再剥离任何字节——OSC 633
 * 标记需原样透传给终端/机器客户端；这里只缓存最近一段提示符供新加入者单播。 */
static void pty_emit(session_t *s, const uint8_t *p, size_t n)
{
    /* 缓存最近输出尾部用于新客户端"当前提示符"单播（简单实现：保留最后
     * PS1 边界之后的内容由标记扫描在客户端完成；服务端仅缓存最近片段）。 */
    emit_out(s, p, n);
}

/* pty master 侧读：行规程回显、shell 层输出、前台命令输出都从这里广播 */
static void pty_read_all(session_t *s)
{
    uint8_t buf[65536];
    for (;;) {
        ssize_t n = read(s->pty_master, buf, sizeof buf);
        if (n > 0) {
            pty_emit(s, buf, (size_t)n);
            continue;
        }
        break; /* EAGAIN / EIO / EINTR */
    }
}

/* 挂断当前前台进程组（执行者掉线 / 会话结束） */
static void kill_foreground_job(session_t *s)
{
    if (s->running && shsh_fg_pgid > 0)
        kill(-shsh_fg_pgid, SIGHUP);
}

/* ---- 用户输入写入 pty master（非阻塞，防短写丢数据） ---- */

/* 把积压的用户输入尽量写入 master；写入无进展（短写/队列满/EAGAIN）即返回，
 * 余量留待会话循环下次续写。master 为非阻塞：行规程输入队列满时单次 write
 * 会短写甚至返回 0，必须循环写并保留余量，否则大块粘贴/快速输入会丢字节。
 * 每写入一块即排空一次 master 输出（canonical+ECHO 下回显走输出队列），
 * 让输入队列与回显队列交替腾挪——只灌输入不排空回显，内核会在队列压力下
 * 丢输入字节。 */
/* 每次写入 master 的最大块：小块灌入 + 立即排空回显，避免 canonical+ECHO
 * 下回显队列跟不上而被内核丢字节（队列满时 write 返回 0，交外层轮询等待）。 */
#define PTY_IN_WRITE_CHUNK 1024

static void pty_flush_input(session_t *s)
{
    while (s->pty_in.len > 0) {
        /* 写入前先排空回显/输出：保证本次 write 时回显队列为空，小块产生的
         * 回显（<= PTY_IN_WRITE_CHUNK）不会溢出，内核也就不会丢输入字节。 */
        pty_read_all(s);
        size_t n = s->pty_in.len < PTY_IN_WRITE_CHUNK
                       ? s->pty_in.len : PTY_IN_WRITE_CHUNK;
        ssize_t w = write(s->pty_master, s->pty_in.data, n);
        if (w > 0) {
            buf_consume(&s->pty_in, (size_t)w);
            continue;
        }
        if (w == 0)
            break;  /* 输入队列满，写入无进展：交外层轮询等待消费者 */
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        /* EIO 等：master 侧已不可写，丢弃待写输入 */
        buf_clear(&s->pty_in);
        break;
    }
    pty_read_all(s);   /* 收尾再排空一次 */
}

/* ---- 会话内 help（由会话层直接打印） ---- */

static const char *sh_help_text =
    "shsh — 多用户共享 shell\n"
    "\n"
    "会话内建命令：\n"
    "  cd [目录]            切换会话工作目录（对所有用户生效）\n"
    "  pwd                  显示会话工作目录\n"
    "  echo [-n] ...        输出文本\n"
    "  export NAME=value    设置会话变量（对后续命令与子进程可见）\n"
    "  unset NAME           删除会话变量\n"
    "  NAME=value           设置会话变量\n"
    "  to <用户>            把执行权转交给 <用户>；to shshd 交还给系统\n"
    "  help                 显示本帮助\n"
    "  exit                 离开会话（执行者离开会自动移交执行权）\n"
    "\n"
    "执行权：\n"
    "  同一时刻只有一名用户可执行命令，其他人的输入被直接丢弃。\n"
    "  执行者离开或掉线时，执行权自动移交给最早加入的在线用户。\n"
    "  所有用户的输入过程实时共享。\n"
    "\n"
    "快捷键（执行者）：\n"
    "  退格 / Ctrl+W / Ctrl+U  canonical 行编辑\n"
    "  Ctrl+C               执行者：中断前台命令（raw/TUI 前台作按键送出）；非执行者：离开\n"
    "  Ctrl+D               执行者：运行态交给前台（如 EOF）、空闲态离开；非执行者：离开\n"
    "\n"
    "会话外：\n"
    "  shsh -l                 列出所有会话及其在线用户\n"
    "  shsh -l <会话>          列出该会话的在线用户\n"
    "  shsh <用户名> [会话]    加入会话（省略会话名时为默认会话）\n"
    "  shshd [会话]            启动会话（省略会话名时为默认会话）\n";

static void cmd_help(session_t *s)
{
    emit_out(s, (const uint8_t *)sh_help_text, strlen(sh_help_text));
}

/* 转交执行权。target 为 "shshd" 时由 shshd 立即再转交给其他用户 */
static void cmd_to(session_t *s, client_t *c, const char *target)
{
    if (target[0] == '\0') {
        out_text(s, "to: 用法: to <用户>\r\n");
        update_user(s);
        return;
    }
    if (strcmp(target, SHSHD_NAME) == 0) {
        s->executor_id = SH_EXEC_SHSHD;
        client_t *t = first_named_client(s, c->id);
        if (t) {
            s->executor_id = t->id;
            sys_to_line(s, t->name);
        }
        broadcast_role(s);
        update_user(s);
        return;
    }
    client_t *t = client_by_name(s, target);
    if (t == c) {
        out_text(s, "to: 不能转交给自己\r\n");
    } else if (!t) {
        out_text(s, "to: 用户 %s 不在会话中\r\n", target);
    } else {
        s->executor_id = t->id;
        broadcast_role(s);
    }
    update_user(s);
}

/* ---- 命令源：pty slave → 命令管道 ----
 *
 * 空闲态执行者的键入经 master 进入行规程（canonical+ECHO），回显由 master
 * 读出广播；回车交付的整行从 slave 读出，由会话层按行拦截 to/exit/help，
 * 普通行写入命令管道交给 shell 层解析执行。运行命令期间注销 slave 读，
 * 输入归前台程序所有。 */

static void cmd_read_arm(session_t *s)
{
    if (s->slave_armed)
        return;
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = s->pty_slave};
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->pty_slave, &ev) == 0)
        s->slave_armed = true;
}

static void cmd_read_disarm(session_t *s)
{
    if (!s->slave_armed)
        return;
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->pty_slave, NULL);
    s->slave_armed = false;
}

/* 把一段文本（含结尾换行）注入 shell 命令源 */
static void inject_line(session_t *s, const char *line)
{
    size_t n = strlen(line);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(s->cmd_pipe[1], line + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }
}

/* 执行权变更后刷新提示符：直接改 shell 变量（SHSH_USER），并按相位让提示符
 * 尽快出现——运行态广播合成提示符，空闲注入空行触发重打，PS2 交给 abort。 */
static void update_user(session_t *s)
{
    /* shell 变量即时更新（不经命令源）：此后任何一次提示符打印都带新名字，
     * 不会再出现"先按旧名打一次 PS1 再改名"的过渡提示符 */
    shsh_shell_set_user(executor_name(s));
    if (s->running) {
        /* 运行态：shell 不会立刻打印 PS1，先广播合成提示符让移交可见 */
        char ps1[512];
        compose_prompt(s, ps1, sizeof ps1);
        out_text(s, "%s", ps1);
    } else if (whichprompt == 1) {
        /* 空闲：注入一个空行让 shell 重打提示符（此刻不会污染任何命令） */
        inject_line(s, "\n");
    }
    /* PS2 续行：变量已更新，待 abort/回 PS1 后 shell 自会以新名打印，不注入 */
}

static bool line_token(const char *line, const char *tok, const char **rest)
{
    size_t n = strlen(tok);
    if (strncmp(line, tok, n) != 0)
        return false;
    if (line[n] != '\0' && line[n] != ' ' && line[n] != '\t')
        return false;
    if (rest) {
        const char *p = line + n;
        while (*p == ' ' || *p == '\t')
            p++;
        *rest = p;
    }
    return true;
}

/* 处理 cmd_buf 中已交付的完整行：拦截 to/exit/help；普通行写入命令管道。
 * 返回 true 表示已把内容交给 shell（普通行或刷新提示符的注入），
 * 调用方应让 shell 继续。 */
static bool deliver_lines(session_t *s)
{
    for (;;) {
        char *nl = memchr(s->cmd_buf, '\n', s->cmd_len);
        if (!nl)
            return false;
        size_t llen = (size_t)(nl - s->cmd_buf);
        char line[4096];
        size_t n = llen;
        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, s->cmd_buf, n);
        line[n] = '\0';
        size_t consumed = llen + 1;
        s->cmd_len -= consumed;
        memmove(s->cmd_buf, s->cmd_buf + consumed, s->cmd_len);

        /* 仅在新命令边界（PS1）按行首词法拦截 to/exit/help。多行命令的
         * 续行（PS2，如 if/heredoc 体）里出现的同名行属于该命令本身，必须
         * 原样交给 shell——否则会被误判为会话级指令并导致执行权中途移交。 */
        if (whichprompt == 1) {
            if (line_token(line, "exit", NULL)) {
                client_t *c = client_by_id(s, s->executor_id);
                if (c)
                    request_leave(s, c);
                continue;
            }
            const char *rest = NULL;
            if (line_token(line, "to", &rest)) {
                client_t *c = client_by_id(s, s->executor_id);
                if (c)
                    cmd_to(s, c, rest);
                continue;
            }
            if (line_token(line, "help", NULL)) {
                cmd_help(s);
                inject_line(s, "\n"); /* 让 shell 回到 PS1 */
                return true;
            }
        }
        /* 普通行（含空行）：交给 shell 解析执行 */
        run_begin(s);
        inject_line(s, line);
        inject_line(s, "\n");
        return true;
    }
}

/* 命令源是否有待读内容：命令管道里已有残余，或 cmd_buf 攒齐了一行 */
static bool have_pending_cmd(session_t *s)
{
    if (s->cmd_len && memchr(s->cmd_buf, '\n', s->cmd_len))
        return true;
    struct pollfd p = {.fd = s->cmd_pipe[0], .events = POLLIN};
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

static void on_cmd_input(session_t *s)
{
    /* 行规程对所提交行的回显在 master 一侧，必须先于命令执行/执行权切换
     * 广播出去，否则 shell 输出、移交后的新提示符会插到回显前面 */
    pty_read_all(s);

    char chunk[sizeof s->cmd_buf];
    ssize_t r = read(s->pty_slave, chunk, sizeof chunk);
    if (r > 0) {
        if (s->cmd_len + (size_t)r > sizeof s->cmd_buf)
            s->cmd_len = 0; /* 超长行：丢弃保护，等下一规范行 */
        else {
            memcpy(s->cmd_buf + s->cmd_len, chunk, (size_t)r);
            s->cmd_len += (size_t)r;
        }
    }
}

/* ---- 客户端输入 ---- */

static void request_leave(session_t *s, client_t *c)
{
    msg_t bye;
    msg_make_bye(&bye, "已退出会话");
    send_msg(s, c, &bye, false);
    buf_free(&bye.payload);
    c->want_close = true;
}

static void handle_raw(session_t *s, client_t *c, const msg_t *m)
{
    if (!c->has_name)
        return;
    const uint8_t *data = NULL;
    size_t len = 0;
    msg_parse_bytes(m, &data, &len);
    if (!len)
        return;

    /* ^C/^D 对任何用户都接收，但只有执行者写入 PTY 的普通字节才放行 */
    buf_t pass;
    buf_init(&pass);
    bool leave = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = data[i];
        if (ch == 0x04) {
            /* ^D 两模式：非执行者——其前台即 shsh 自身，^D=退出（离开）；
             * 执行者运行态——前台是共享命令，把 ^D 交给前台任务自定（canonical
             * 得到 VEOF/EOF，raw 得到按键），人留在会话；执行者空闲/续行——
             * 会话级离开（续行态同时放弃半成品，见 client_disconnect）。 */
            if (c->id == s->executor_id && s->running) {
                buf_append_byte(&pass, ch);
                continue;
            }
            leave = true;
            break;
        }
        if (ch == 0x03) {
            /* ^C 两模式：非执行者——前台即 shsh 自身，^C=结束（离开）；执行者
             * 运行态——把字节交给 pty 行规程的 VINTR，由内核按当前前台进程组
             * 投递 SIGINT（前台程序若自行关了 ISIG，则作普通按键送达）；空闲
             * 态无前台作业，丢弃。 */
            if (c->id != s->executor_id) {
                leave = true;
                break;
            }
            if (s->running)
                buf_append_byte(&pass, ch);
            continue;
        }
        if (c->id == s->executor_id)
            buf_append_byte(&pass, ch);
    }
    if (pass.len) {
        buf_append(&s->pty_in, pass.data, pass.len);
        pty_flush_input(s);   /* 循环写，未完则留待会话循环续写（防短写丢数据） */
    }
    buf_free(&pass);
    if (leave)
        request_leave(s, c);
}

static void handle_winch(session_t *s, client_t *c, const msg_t *m)
{
    uint16_t rows, cols;
    if (msg_parse_winch(m, &rows, &cols) != 0)
        return;
    c->rows = rows;
    c->cols = cols;
    if (c->has_name && c->id == s->executor_id && rows > 0 && cols > 0)
        pty_set_size(s->pty_master, rows, cols);
}

/* ---- 会话管理 ---- */

static void reply_and_close(session_t *s, client_t *c, const char *reason)
{
    msg_t bye;
    msg_make_bye(&bye, reason);
    send_msg(s, c, &bye, true);
    buf_free(&bye.payload);
    c->want_close = true;
}

static void handle_hello(session_t *s, client_t *c, const msg_t *m)
{
    char name[SH_NAME_MAX + 1];
    uint16_t rows, cols;
    if (c->has_name || msg_parse_hello(m, name, sizeof name, &rows, &cols) != 0) {
        reply_and_close(s, c, "无效的 HELLO");
        return;
    }
    if (!sh_name_valid(name)) {
        reply_and_close(s, c, "非法的用户名");
        return;
    }
    if (strcmp(name, SHSHD_NAME) == 0 || client_by_name(s, name)) {
        char reason[SH_NAME_MAX + 16];
        snprintf(reason, sizeof reason, "%s 已存在", name);
        reply_and_close(s, c, reason);
        return;
    }
    c->has_name = true;
    c->id = s->next_id++;
    c->rows = rows;
    c->cols = cols;
    snprintf(c->name, sizeof c->name, "%s", name);

    bool took_over = false;
    if (s->executor_id == SH_EXEC_SHSHD) {
        s->executor_id = c->id; /* 无人执行时，新加入者接管 */
        took_over = true;
    }

    msg_t w;
    msg_make_welcome(&w, c->id, s->executor_id, executor_name(s), NULL, 0);
    send_msg(s, c, &w, false);
    buf_free(&w.payload);
    broadcast_role(s);

    if (took_over) {
        update_user(s); /* 让 shell 打出带新执行者名的提示符 */
    } else if (s->ps_ok && !s->running && whichprompt == 1) {
        /* 跟随者：单播缓存的当前提示符（§3.5 零历史），不广播、不惊动 shell。
         * PS2 续行中缓存的是"上一次 PS1"，并非当前相位，故不单播 */
        msg_t pm;
        msg_make_out(&pm, s->ps_cache, s->ps_cache_len);
        send_msg(s, c, &pm, false);
        buf_free(&pm.payload);
    }
}

static void handle_list(session_t *s, client_t *c)
{
    size_t n = 1;
    for (client_t *t = s->clients; t; t = t->next)
        if (t->has_name)
            n++;
    user_info_t *users = xmalloc(n * sizeof *users);
    users[0].id = SH_EXEC_SHSHD;
    snprintf(users[0].name, sizeof users[0].name, "%s", SHSHD_NAME);
    users[0].is_exec = (s->executor_id == SH_EXEC_SHSHD);
    size_t i = 1;
    for (client_t *t = s->clients; t; t = t->next) {
        if (!t->has_name)
            continue;
        users[i].id = t->id;
        users[i].is_exec = (t->id == s->executor_id);
        snprintf(users[i].name, sizeof users[i].name, "%s", t->name);
        i++;
    }
    msg_t m;
    msg_make_listresp(&m, s->name, users, n);
    send_msg(s, c, &m, true); /* LIST 不需要 HELLO */
    buf_free(&m.payload);
    free(users);
    c->want_close = true;
}

static void dispatch(session_t *s, client_t *c, const msg_t *m)
{
    switch (m->type) {
    case C2S_HELLO:
        handle_hello(s, c, m);
        break;
    case C2S_RAW:
        handle_raw(s, c, m);
        break;
    case C2S_WINCH:
        handle_winch(s, c, m);
        break;
    case C2S_LIST:
        handle_list(s, c);
        break;
    default:
        break;
    }
}

static void process_input(session_t *s, client_t *c)
{
    for (;;) {
        if (c->want_close)
            return;
        msg_t m;
        int r = msg_try_extract(&c->in, &m);
        if (r < 0) {
            reply_and_close(s, c, "协议错误");
            return;
        }
        if (r == 0)
            return;
        dispatch(s, c, &m);
        buf_free(&m.payload);
        if (c->want_close)
            return;
    }
}

static void accept_clients(session_t *s)
{
    for (;;) {
        int fd = accept4(s->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            break;
        }
        client_add(s, fd);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.fd = fd};
        if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            client_disconnect(s, client_find(s, fd), false);
            continue;
        }
    }
}

static void session_shutdown(session_t *s, const char *reason)
{
    if (s->running)
        kill_foreground_job(s);
    msg_t bye;
    msg_make_bye(&bye, reason);
    broadcast_msg(s, &bye);
    buf_free(&bye.payload);
    s->stopping = true;
}

const char *session_name(session_t *s)
{
    return s->name;
}

const char *session_sockpath(session_t *s)
{
    return s->sockpath;
}

int session_cmd_fd(session_t *s)
{
    return s->cmd_pipe[0];
}

int session_pty_slave(session_t *s)
{
    return s->pty_slave;
}

/* 让本进程成为会话首领，以便随后把 pty slave 收作控制终端（作业控制/TOSTOP/
 * VINTR 都依赖控制终端）。从交互 shell 启动时本进程通常已是进程组组长，
 * setsid 会返回 EPERM；此时 fork 出非组长的子进程接棒，父进程退出（守护化，
 * 启动命令不变）。子进程是唯一继续者：pty、socket、事件循环都在其中建立。 */
static int become_session_leader(void)
{
    if (setsid() >= 0)
        return 0;               /* 直接成为会话首领 */
    if (errno != EPERM)
        return -1;
    /* 已是进程组组长，无法 setsid；fork 一份在父组内的非组长子进程继续 */
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);               /* 父进程交棒退出，子进程在后台继续服务 */
    if (setsid() < 0)
        return -1;
    return 0;
}

int session_start(session_t **out, const char *name)
{
    session_t *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->sockpath, sizeof s->sockpath, "%s/%s.sock", SH_SOCK_DIR, name);
    s->next_id = 1; /* 0 保留给 shshd */
    s->pty_master = -1;
    s->pty_slave = -1;
    s->cmd_pipe[0] = -1;
    s->cmd_pipe[1] = -1;
    s->executor_id = SH_EXEC_SHSHD;
    buf_init(&s->pty_in);

    if (mkdir(SH_SOCK_DIR, 0777) != 0 && errno != EEXIST)
        fatal("无法创建 %s: %s", SH_SOCK_DIR, strerror(errno));
    chmod(SH_SOCK_DIR, 01777);

    if (access(s->sockpath, F_OK) == 0) {
        int tmp = socket(AF_UNIX, SOCK_STREAM, 0);
        if (tmp < 0)
            fatal("socket: %s", strerror(errno));
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        set_sun_path(&a, s->sockpath);
        if (connect(tmp, (struct sockaddr *)&a, sizeof a) == 0) {
            close(tmp);
            free(s);
            return -1; /* 会话已存在 */
        }
        close(tmp);
        unlink(s->sockpath);
        log_info("清理陈旧 socket %s", s->sockpath);
    }

    /* 先成会话首领、再把 pty slave 收作控制终端：作业控制、TOSTOP、
     * ^C(VINTR) 等标准终端语义都以此为前提（见 dash_decision.md 4）。 */
    if (become_session_leader() != 0)
        fatal("无法成为会话首领: %s", strerror(errno));
    if (pty_open(&s->pty_master, &s->pty_slave) != 0)
        fatal("无法创建 pty: %s", strerror(errno));
    if (pty_acquire_ctty(s->pty_slave) != 0)
        log_error("无法取得控制终端: %s（作业控制不可用）", strerror(errno));
    s->pty_tio_ok = pty_config_shared(s->pty_slave, &s->pty_tio) == 0;
    /* slave 保持阻塞：会话层读 canonical 行、shell 层 fd0/1/2 都指向它，
     * 非阻塞会破坏 shell 的 read 内建与前台程序的行输入 */
    pty_set_size(s->pty_master, DEFAULT_ROWS, DEFAULT_COLS);

    /* 命令管道：读端给 shell 层当命令源，移到 fd>=10 以避开 dash 内部
     * 使用的 fd（其重定向保存槽从 10 起取最小可用，占用即跳过） */
    if (pipe2(s->cmd_pipe, O_CLOEXEC) != 0)
        fatal("创建命令管道失败: %s", strerror(errno));
    {
        int hi = fcntl(s->cmd_pipe[0], F_DUPFD_CLOEXEC, 10);
        if (hi >= 0) {
            close(s->cmd_pipe[0]);
            s->cmd_pipe[0] = hi;
        }
    }

    s->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s->listen_fd < 0)
        fatal("socket: %s", strerror(errno));
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    set_sun_path(&a, s->sockpath);
    if (bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) < 0)
        fatal("bind %s: %s", s->sockpath, strerror(errno));
    chmod(s->sockpath, 0666);
    if (listen(s->listen_fd, 16) < 0)
        fatal("listen: %s", strerror(errno));
    set_nonblock(s->listen_fd);

    s->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s->epoll_fd < 0)
        fatal("epoll_create1: %s", strerror(errno));
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = s->listen_fd};
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->listen_fd, &ev) < 0)
        fatal("epoll_ctl(listen): %s", strerror(errno));
    struct epoll_event pev = {.events = EPOLLIN, .data.fd = s->pty_master};
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->pty_master, &pev) < 0)
        fatal("epoll_ctl(pty): %s", strerror(errno));

    /* 信号收编：SIGTERM/SIGHUP 关停会话（SIGHUP 亦会在本进程关闭 pty master
     * 时由内核发给会话首领，必须阻塞以经 signalfd 处理，否则会以默认处置
     * 把"正常退出"变成被信号杀死）；SIGCHLD 唤醒事件循环（由 shell 层 wait3
     * 自行 reap，会话层消费后丢弃）。SIGINT 不入掩码——它由 shell 层捕获
     * 用于中断合成与 EXINT 恢复，会话层不接管（见 dash_decision.md 3.1）。 */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    s->sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (s->sig_fd < 0)
        fatal("signalfd: %s", strerror(errno));
    signal(SIGPIPE, SIG_IGN);
    struct epoll_event sev = {.events = EPOLLIN, .data.fd = s->sig_fd};
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->sig_fd, &sev) < 0)
        fatal("epoll_ctl(signal): %s", strerror(errno));

    /* 初始执行者为 shshd：先落环境变量，shell 层 init 导入后首个提示符
     * 即呈 "shshd:\W$ "（见 dash_decision.md 3.2） */
    setenv("SHSH_USER", SHSHD_NAME, 1);

    /* 注册 shell 层同步钩子 */
    g_session = s;
    shsh_cmd_wait = srv_cmd_wait;
    shsh_job_wait = srv_job_wait;
    shsh_read_wait = srv_read_wait;

    *out = s;
    return 0;
}

/* 会话事件循环的驱动体。
 * block=true ：空闲态——循环处理事件直到命令源就绪或会话停止；
 * block=false：运行态——处理一轮事件即返回，供 shell 层前台等待重试。 */
static void session_drive(session_t *s, bool block)
{
    struct epoll_event events[EPOLL_MAX_EVENTS];
    do {
        /* 有积压的用户输入：先尽力续写；pty master 的 POLLOUT 恒为真、不能
         * 用作可写唤醒，故退化为按 PTY_IN_POLL_MS 超时轮询，边写边等。 */
        if (s->pty_in.len)
            pty_flush_input(s);
        int timeout = s->pty_in.len ? PTY_IN_POLL_MS : -1;
        int n = epoll_wait(s->epoll_fd, events, EPOLL_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fatal("epoll_wait: %s", strerror(errno));
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (fd == s->listen_fd) {
                accept_clients(s);
                continue;
            }
            if (fd == s->pty_master) {
                pty_read_all(s);
                continue;
            }
            if (fd == s->pty_slave) {
                on_cmd_input(s);
                continue;
            }
            if (fd == s->sig_fd) {
                struct signalfd_siginfo si;
                while (read(s->sig_fd, &si, sizeof si) == sizeof si) {
                    if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGHUP)
                        session_shutdown(s, "会话已结束");
                    /* SIGCHLD：丢弃，由 shell 层 wait3 自行 reap */
                }
                continue;
            }
            client_t *c = client_find(s, fd);
            if (!c)
                continue;
            if (ev & (EPOLLERR | EPOLLHUP)) {
                client_disconnect(s, c, false);
                continue;
            }
            if (ev & EPOLLOUT) {
                if (!flush_out(s, c))
                    continue;
            }
            if (ev & (EPOLLIN | EPOLLRDHUP)) {
                char chunk[8192];
                bool closed = false;
                for (;;) {
                    if (c->in.len > CLIENT_IN_LIMIT) {
                        closed = true;
                        break;
                    }
                    /* 待写输入已达高水位：暂停读，交由 AF_UNIX 反向背压；
                     * 余量留待 pty_in 排空后再读（fd 仍挂 EPOLLIN，会再触发） */
                    if (s->pty_in.len >= PTY_IN_HIGH_WATER)
                        break;
                    ssize_t r = read(c->fd, chunk, sizeof chunk);
                    if (r > 0) {
                        buf_append(&c->in, chunk, (size_t)r);
                        continue;
                    }
                    if (r == 0) {
                        closed = true;
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if (errno == EINTR)
                        continue;
                    closed = true;
                    break;
                }
                if (closed) {
                    client_disconnect(s, c, false);
                    continue;
                }
                process_input(s, c);
                if (c->want_close && !c->out_armed) {
                    client_disconnect(s, c, false);
                    continue;
                }
            }
        }
    } while (block && !s->stopping && !s->abort_cmd && !have_pending_cmd(s));
}

/* ---- shell 层钩子 ---- */

/* 空闲态：shell 需要下一条命令时调用。同步驱动会话事件循环，直到把一行
 * 普通命令写入命令管道（0）或会话停止（-1）。 */
static int srv_cmd_wait(int fd)
{
    session_t *s = g_session;
    if (!s || fd != s->cmd_pipe[0])
        return -1;
    s->running = false;
    /* 回到命令边界：把 pty 行规程复位回会话基线。前台 raw 程序/TUI 可能改
     * 过 termios，且被杀（SIGHUP）时来不及自行复位；不复位会让下一次输入
     * 无法 canonical 解析（换行不认、无回显），会话像"卡住"。 */
    if (s->pty_tio_ok)
        tcsetattr(s->pty_slave, TCSANOW, &s->pty_tio);
    shsh_read_abort = 0;   /* 清上一轮 read 的放弃标志 */
    /* 回到空闲：上一条命令已结束，携其状态收尾上一相位 */
    run_end(s, exitstatus);
    for (;;) {
        if (s->stopping)
            return -1;
        if (s->abort_cmd) {
            /* 执行者离开、shell 停在未完成的多行命令（PS2）：放弃半成品
             * 解析，经 EXINT 回到 PS1（noreturn，不会返回到此处）。
             * 运行态（复合命令）的情形在 srv_job_wait 处理。 */
            s->abort_cmd = false;
            shsh_shell_abort();
        }
        if (deliver_lines(s))
            return 0;
        if (have_pending_cmd(s))
            return 0;
        cmd_read_arm(s);
        session_drive(s, true);
        cmd_read_disarm(s);
    }
}

/* 运行态：shell 前台等待作业时调用。处理一轮会话事件（drain pty 输出、
 * 服务客户端把执行者输入写 master），随即返回让 shell 重试 wait3。 */
static void srv_job_wait(void)
{
    session_t *s = g_session;
    if (!s || s->stopping)
        return;
    s->running = true;
    session_drive(s, false);
    if (s->abort_cmd) {
        /* 执行者离开：结束**整个**前台任务（不只是当前 pipeline）——放弃
         * shell 当前命令的执行（EXINT），复合命令不再继续下一轮。前台组
         * 已在 client_disconnect 里挂断，清掉 pgid 免残留被后续 ^C 误用。 */
        s->abort_cmd = false;
        shsh_fg_pgid = 0;
        shsh_shell_abort(); /* noreturn：回 run_fd 落点，丢弃当前命令 */
    }
}

/* 内建 read 阻塞等待 fd0 输入时调用（见 shsh_shell.h）：驱动一轮会话事件，
 * 把执行者键入转发到 master、把回显/输出广播出去，然后返回让 read 重试。
 * running 置真使会话层不抢读 slave（输入归 read 独占）。执行者掉线或会话
 * 停止时置 shsh_read_abort，令 read 立即返回。 */
static void srv_read_wait(void)
{
    session_t *s = g_session;
    if (!s) {
        shsh_read_abort = 1;
        return;
    }
    if (s->stopping || s->abort_cmd) {
        shsh_read_abort = 1;
        return;
    }
    shsh_read_abort = 0;
    s->running = true;
    session_drive(s, false);
    if (s->stopping || s->abort_cmd)
        shsh_read_abort = 1;
}

void session_free(session_t *s)
{
    if (g_session == s) {
        g_session = NULL;
        shsh_cmd_wait = NULL;
        shsh_job_wait = NULL;
        shsh_read_wait = NULL;
    }
    flush_all(s); /* 关停前把排队的 BYE 送出（运行循环已不再负责 flush） */
    while (s->clients)
        client_disconnect(s, s->clients, false);
    kill_foreground_job(s);
    unlink(s->sockpath);
    close(s->listen_fd);
    close(s->epoll_fd);
    close(s->sig_fd);
    if (s->cmd_pipe[0] >= 0)
        close(s->cmd_pipe[0]);
    if (s->cmd_pipe[1] >= 0)
        close(s->cmd_pipe[1]);
    if (s->pty_master >= 0)
        close(s->pty_master);
    if (s->pty_slave >= 0)
        close(s->pty_slave);
    buf_free(&s->pty_in);
    free(s);
}