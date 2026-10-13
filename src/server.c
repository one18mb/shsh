#include "server.h"
#include "buf.h"
#include "common.h"
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

/* ---- 统一文本协议：OSC 633 带内标记 ----
 * shshd↔shsh 之间只有一条文本字节流，命令边界/退出码/执行权/关闭都编码为
 * 人眼不可见的 OSC 633 序列（终止符 ST = ESC \\）。 */
#define OSC633 "\033]633;"
#define OSC_ST "\033\\"
/* 客户端→服务端：终端尺寸（不写入 PTY，服务端剥离后转 TIOCSWINSZ）。
 * 形如 ESC ] 633 ; W ; <rows> ; x <cols> ESC \\ */
#define OSC_W_PREFIX OSC633 "W;"

#define EPOLL_MAX_EVENTS 64
#define CLIENT_OUT_HIGH_WATER (1u << 20)  /* 单客户端输出积压软上限：达此值即
                                           * 暂停读其输入（背压），而非断连 */
#define CLIENT_OUT_HARD_MAX (64u << 20)   /* 硬上限：仅防失控客户端 OOM */
#define CLIENT_IN_LIMIT (4u << 20)  /* 单客户端未裁决输入积压上限 */
/* 待写入 pty master 的用户输入上限（高水位）：达此值即暂停读客户端 socket，
 * 由 AF_UNIX 反向施加背压（见下方读取循环与 pty_flush_input）。 */
#define PTY_IN_HIGH_WATER (1u << 20)
/* 余量待写时已用 EPOLLOUT 边沿唤醒（pty_arm_out）；该超时只是无事件时
 * 周期性兜底续写。 */
#define PTY_IN_POLL_MS 10
/* 回显晚到时最多额外等待 master EPOLLIN 的时间（见 echo_pending 门控） */
#define ECHO_SETTLE_MS 2
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
/* 上一条命令的退出状态（dash 层 error.c 的 exitstatus）：写入 C/D 标记 */
extern int exitstatus;
/* 最近一次提示符：1=PS1（新命令边界），2=PS2（多行命令续行，见 parser.h）。
 * 会话层据此只在 PS1 拦截 to/exit/help、只在 PS1 注入 SHSH_USER，
 * 避免把多行命令中路过的 exit/续行当作会话级指令处理。 */
extern int whichprompt;

typedef struct client {
    int fd;
    uint32_t id;
    char name[SH_NAME_MAX + 1];
    bool has_name;    /* 首行握手已完成（已命名，加入会话） */
    bool is_list;     /* 首行是 list：纯文本查询，应答到 EOF 即关闭 */
    bool want_close;  /* 输出排空后关闭 */
    bool out_armed;   /* epoll 已注册 EPOLLOUT */
    bool read_paused; /* 输出积压达软上限，已暂停读该客户端输入（背压） */
    bool dead;
    uint16_t rows, cols;
    buf_t in, out;    /* in：未命名时累积握手首行；out：待写回的文本流 */
    char wcarry[40];  /* 跨读的半截 OSC W 尺寸标记 */
    size_t wcarry_len;
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
    bool abort_cmd;     /* 需放弃未完成的多行命令（执行者中途离开，见 srv_cmd_wait） */
    bool echo_pending;  /* slave 已读到 canonical 行，但该行回显尚未从 master
                            * 排空（回显可能晚一个调度滴答才可读）；为真时
                            * 事件循环须再读一轮 master 再交付命令，保证
                            * 字节序 回显→A */
    bool echo_after_write; /* 本次 pty 续写在 write 之后已读到 master 输出 */
    client_t *clients;  /* 按连接顺序，头部 = 最早加入 */
    uint32_t next_id;
    uint32_t executor_id;  /* 0=shshd；否则为持有执行权的 client id（内部用） */
    bool stopping;
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
static int srv_cmd_drained(void);

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
    s->echo_pending = false;
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
    /* 背压：暂停读输入时去掉 EPOLLIN（否则 level-trigger 会忙等空转）；
     * 待输出积压回落到软上限以下再恢复（见 flush_out）。 */
    ev.events = (c->read_paused ? 0 : EPOLLIN) | EPOLLRDHUP |
                (c->out_armed ? EPOLLOUT : 0);
    ev.data.fd = c->fd;
    return epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* ---- 发送（统一文本字节流） ---- */

static void client_queue(session_t *s, client_t *c, const void *data,
                         size_t len, bool allow_unnamed)
{
    if (c->want_close)
        return;
    if (!c->has_name && !allow_unnamed)
        return;
    if (!len)
        return;
    buf_append(&c->out, data, len);
    if (c->out.len > CLIENT_OUT_HARD_MAX) {
        log_error("客户端 %s 输出积压超硬上限，断开", c->name);
        c->want_close = true;
    }
    if (!c->out_armed) {
        c->out_armed = true;
        client_update_events(s, c);
    }
}

static void broadcast_bytes(session_t *s, const void *data, size_t len)
{
    for (client_t *c = s->clients; c; c = c->next)
        client_queue(s, c, data, len, false);
}

static void broadcast_str(session_t *s, const char *str)
{
    broadcast_bytes(s, str, strlen(str));
}

/* 构造 633 D 提示符边界标记到 buf（返回长度）。 */
static size_t build_mark_d(char *buf, size_t sz, int status,
                           const char *user, const char *wbase)
{
    return (size_t)snprintf(buf, sz,
                            OSC633 "D;s=%d;u=%s;w=%s" OSC_ST,
                            status, user ? user : "", wbase ? wbase : "");
}

/* 构造 633 E（会话关闭）标记。 */
static size_t build_mark_e(char *buf, size_t sz, const char *reason)
{
    return (size_t)snprintf(buf, sz,
                            OSC633 "E;reason=%s" OSC_ST,
                            reason ? reason : "");
}

/* 广播一段格式化文本（系统提示行、help 等） */
static void out_text(session_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void out_text(session_t *s, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    broadcast_str(s, buf);
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
    /* 有界拷贝：cwd 可达 PATH_MAX，显式截断并保证终止，避免 sprintf 截断告警 */
    size_t n = strlen(src);
    if (n >= sz)
        n = sz - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

/* 构造一枚"当前提示符"两段：OSC 633 D 标记 + 可见 PS1 文本。 */
/* D 标记/可见 PS1 的缓冲上限：u 最长 SH_NAME_MAX，w 最长 PATH_MAX，
 * 连同固定模板与退出码仍远小于此 */
#define PROMPT_MARK_MAX (PATH_MAX + SH_NAME_MAX + 64)

static void build_current_prompt(session_t *s, char *mark, size_t msz,
                              char *ps, size_t psz)
{
    char w[PATH_MAX];
    cwd_basename(w, sizeof w);
    build_mark_d(mark, msz, exitstatus, executor_name(s), w);
    snprintf(ps, psz, "%s:%s$ ", executor_name(s), w);
}

/* 合成并单播给某客户端一枚"当前提示符"（OSC 633 D 标记 + 可见 PS1）。
 * 新加入的跟随者在空闲 PS1 时收到它，从而立刻知道当前执行者/目录，并看到
 * 提示符（零历史：不补历史输出，也不打扰 shell）。执行权变化时 shell 会经
 * update_user 重打提示符，D 标记里的 u= 即新执行者。 */
static void unicast_current_prompt(session_t *s, client_t *c)
{
    char mark[PROMPT_MARK_MAX];
    char ps[PROMPT_MARK_MAX];
    build_current_prompt(s, mark, sizeof mark, ps, sizeof ps);
    client_queue(s, c, mark, strlen(mark), true);
    client_queue(s, c, ps, strlen(ps), true);
}

/* 广播合成提示符：执行权在运行态移交（执行者掉线杀死前台任务）时 shell 不会
 * 立刻重打 PS1，会话层直接广播 D+PS1，u= 即新执行者，各客户端即刻切换
 * 终端模式；shell abort 回 PS1 后还会再打一枚真实 D。 */
static void broadcast_current_prompt(session_t *s)
{
    char mark[PROMPT_MARK_MAX];
    char ps[PROMPT_MARK_MAX];
    build_current_prompt(s, mark, sizeof mark, ps, sizeof ps);
    broadcast_str(s, mark);
    broadcast_str(s, ps);
}

/* shshd 执行 to：唯一的系统广播。显示为新行上的 shshd 命令行 */
static void sys_to_line(session_t *s, const char *target)
{
    char w[300];
    cwd_basename(w, sizeof w);
    out_text(s, "\n%s:%s$ to %s\n", SHSHD_NAME, w, target);
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
    /* 积压回落到软上限以下：恢复读输入（解除背压） */
    if (c->read_paused && c->out.len < CLIENT_OUT_HIGH_WATER) {
        c->read_paused = false;
        client_update_events(s, c);
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
            update_user(s); /* shell 重打 D+PS1，u= 即新执行者 */
        } else {
            s->executor_id = SH_EXEC_SHSHD;
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

/* 共享终端输出：pty master 读到的一切（行规程回显、shell 输出含 OSC 633
 * 标记、前台命令输出）原样广播给所有已命名客户端，逐字节透传、不做封装或
 * 剥离——OSC 633 标记本就是给终端/机器客户端看的带内控制。
 * 任何成功读出都同时清除"等回显"门控（回显严格按写入顺序排在最前面，
 * 只要 master 有输出，在途键入的回显必已包含在内）。 */
static void pty_emit(session_t *s, const uint8_t *p, size_t n)
{
    (void)n;
    s->echo_pending = false;
    broadcast_bytes(s, p, n);
}

/* pty master 侧读：行规程回显、shell 层输出、前台命令输出都从这里广播。
 * 返回本轮是否读到了字节（供回显门控判定）。 */
static bool pty_read_all(session_t *s)
{
    bool any = false;
    uint8_t buf[65536];
    for (;;) {
        ssize_t n = read(s->pty_master, buf, sizeof buf);
        if (n > 0) {
            pty_emit(s, buf, (size_t)n);
            any = true;
            continue;
        }
        break; /* EAGAIN / EIO / EINTR */
    }
    return any;
}

/* ---- 命令相位标记（OSC 633，统一走共享输出流） ----
 * A：命令行交付、即将执行（会话层在此广播）；
 * C：命令结束携带退出码（shell 层 setprompt 在下一 PS1 前发出，保证顺序为
 *    A→输出→C→D，见 parser.c / shsh_shell.h 的 shsh_phase_run）。
 * 发 A 前先排空 pty master：行回显必须排在 A 之前，与真实终端字节序一致。 */
static void phase_run_begin(session_t *s)
{
    /* 到达此处时回显门控已确保本行回显排空（见 on_cmd_input/session_drive），
     * 故回显必然在 A 之前，无需在此再排空。 */
    broadcast_str(s, OSC633 "A" OSC_ST);
    shsh_shell_phase_begin(); /* 令 shell 在下次 PS1 先打 C 再打 D */
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

/* 余量待写时把 master 加挂 EPOLLOUT——slave 侧程序一读走输入，master 即刻
 * 可写，边沿唤醒避免 10ms 定时轮询把大块灌入拖成 ~100KB/s。 */
static void pty_arm_out(session_t *s, bool arm)
{
    struct epoll_event ev;
    ev.events = EPOLLIN | (arm ? EPOLLOUT : 0);
    ev.data.fd = s->pty_master;
    epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, s->pty_master, &ev);
}

static void pty_flush_input(session_t *s)
{
    s->echo_after_write = false;
    bool wrote = false;
    while (s->pty_in.len > 0) {
        /* 写入前先排空回显/输出：保证本次 write 时回显队列为空，小块产生的
         * 回显（<= PTY_IN_WRITE_CHUNK）不会溢出，内核也就不会丢输入字节。 */
        pty_read_all(s);
        size_t n = s->pty_in.len < PTY_IN_WRITE_CHUNK
                       ? s->pty_in.len : PTY_IN_WRITE_CHUNK;
        ssize_t w = write(s->pty_master, s->pty_in.data, n);
        if (w > 0) {
            buf_consume(&s->pty_in, (size_t)w);
            wrote = true;
            /* write 之后立刻能读到输出则回显已就绪（读到即清门控）；读不到
             * （部分内核回显晚一个滴答）则由 on_cmd_input 后的事件循环再等
             * 一轮 master 事件，保证回显排在 A 之前 */
            if (pty_read_all(s))
                s->echo_after_write = true;
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
    if (!wrote)
        pty_read_all(s);   /* 纯排空（本轮无新写入） */
    pty_arm_out(s, s->pty_in.len > 0);
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
    "  shsh -l [-s 会话]       列出会话及在线用户\n"
    "  shsh login -n 用户名 [-s 会话] [-d]   加入会话（-d 为 send 后台端点）\n"
    "  shsh logout -n 用户名 [-s 会话]       停止端点\n"
    "  shshd [-s 会话]         启动会话（省略会话名时为默认会话）\n"
    "  send [-n 用户名 -s 会话] <命令>       非交互同步执行（见 send -h）\n";

static void cmd_help(session_t *s)
{
    broadcast_bytes(s, (const uint8_t *)sh_help_text, strlen(sh_help_text));
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
        /* update_user 令 shell 重打 D+PS1，u= 即新执行者 */
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
        /* 运行态：shell 不会立刻打印 PS1，直接广播合成 D+PS1 让移交可见，
         * 客户端据 D 里的 u= 切换执行者终端模式 */
        broadcast_current_prompt(s);
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
                /* help 是会话层内建、不交给 shell 执行，但仍按"已交付命令"
                 * 发 A：回显已在上方广播，随后是帮助文本，注入空行让 shell
                 * 空转回 PS1 时发 C/P/D。机器客户端（send）才能凭 P 正常收尾，
                 * 否则 help 无 A/C/P，send 会一直等到超时。 */
                phase_run_begin(s);
                cmd_help(s);
                inject_line(s, "\n"); /* 让 shell 回到 PS1 */
                return true;
            }
        }
        /* 普通行（含空行）：交给 shell 解析执行；A 标记命令交付相位。
         * 空行也开相位，shell 空转后即回 PS1（C 退出码 0、D 复现）。 */
        phase_run_begin(s);
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
        if (s->cmd_len + (size_t)r > sizeof s->cmd_buf) {
            s->cmd_len = 0; /* 超长行：丢弃保护，等下一规范行 */
            s->echo_pending = false;
        } else {
            memcpy(s->cmd_buf + s->cmd_len, chunk, (size_t)r);
            s->cmd_len += (size_t)r;
            /* 规范行已可从 slave 读出，但其回显此刻未必已在 master 可读
             * （实测部分内核回显晚一个调度滴答）。若本次续写在 write 后已
             * 读到过输出，则回显必然已广播；否则置门控，事件循环会再排空
             * 一轮 master 才把这行交给 shell（发 A）。 */
            s->echo_pending = !s->echo_after_write;
            if (s->echo_pending && pty_read_all(s))
                s->echo_pending = false; /* 紧邻再读恰好赶上，无需等待 */
        }
    }
}

/* ---- 客户端输入（统一文本字节流） ----
 *
 * 连接建立后客户端先发一行文本握手：name\n 加入会话，或 list\n 做一次性
 * 列表查询。转正后客户端流即纯键入：普通字节（含 ^C/^D）按执行者身份裁决
 * 是否写入共享 PTY；唯一的带外控制是夹带在流里的 OSC 633 W 尺寸标记
 * （ESC ] 633 ; W ; rows ; x cols ST），服务端剥离后转 TIOCSWINSZ，
 * 绝不落进 PTY 输入。半截标记跨读由 c->wcarry 承接。 */

/* 向客户端排队 E（关闭）标记；allow_unnamed 用于握手被拒时尚未命名的连接。
 * 标记发出后排空输出即关闭——"终止"信号 = EOF（Unix 惯例）。 */
static void send_mark_e(session_t *s, client_t *c, const char *reason,
                       bool allow_unnamed)
{
    char mark[256];
    size_t n = build_mark_e(mark, sizeof mark, reason);
    client_queue(s, c, mark, n, allow_unnamed);
}

static void request_leave(session_t *s, client_t *c)
{
    send_mark_e(s, c, "已退出会话", false);
    c->want_close = true;
}

/* OSC W 尺寸标记前缀与长度上限：
 * ESC ] 633 ; W ; = 9 字节；rows/cols 各至多 5 位；';x' 2；ST 2 → ≤23 */
#define OSC_W_PREFIX_LEN (sizeof(OSC_W_PREFIX) - 1)
#define OSC_W_MARK_MAX 32

static void apply_winch(session_t *s, client_t *c,
                       uint16_t rows, uint16_t cols)
{
    c->rows = rows;
    c->cols = cols;
    if (c->id == s->executor_id && rows > 0 && cols > 0)
        pty_set_size(s->pty_master, rows, cols);
}

/* 裁决一个普通键入字节（^C/^D 按执行者身份与相位两模式处理）：
 * 返回 false 表示该字节触发"离开会话"，调用方应停止后续处理。 */
static bool gate_input_byte(session_t *s, client_t *c, unsigned char ch,
                          buf_t *pass)
{
    if (ch == 0x04) {
        /* ^D：非执行者——其前台即 shsh 自身，^D=离开；执行者运行态——交给
         * 共享前台任务自定（canonical 得 EOF、raw 得按键），人留下；执行者
         * 空闲/续行——会话级离开（续行半成品随断开丢弃，见 client_disconnect）。 */
        if (c->id == s->executor_id && s->running) {
            buf_append_byte(pass, ch);
            return true;
        }
        return false;
    }
    if (ch == 0x03) {
        /* ^C：非执行者直接离开；执行者运行态把字节交给行规程 VINTR（内核按
         * 当前前台进程组投 SIGINT；程序关了 ISIG 则作普通按键）；空闲丢弃。 */
        if (c->id != s->executor_id)
            return false;
        if (s->running)
            buf_append_byte(pass, ch);
        return true;
    }
    if (c->id == s->executor_id)
        buf_append_byte(pass, ch);
    return true;
}

static void feed_named_input(session_t *s, client_t *c)
{
    /* 接上上次跨读的半截 W 标记 */
    if (c->wcarry_len) {
        size_t n = c->in.len;
        buf_reserve(&c->in, n + c->wcarry_len);
        memmove(c->in.data + c->wcarry_len, c->in.data, n);
        memcpy(c->in.data, c->wcarry, c->wcarry_len);
        c->in.len = n + c->wcarry_len;
        c->wcarry_len = 0;
    }

    buf_t pass;
    buf_init(&pass);
    bool leave = false;
    size_t n = c->in.len;
    size_t i = 0;
    while (i < n) {
        unsigned char ch = c->in.data[i];
        if (ch != '\033') {
            if (!gate_input_byte(s, c, ch, &pass)) {
                leave = true;
                break;
            }
            i++;
            continue;
        }
        size_t avail = n - i;
        if (avail < OSC_W_PREFIX_LEN) {
            /* 缓冲在可能的标记前缀处结束：前缀确实吻合则留存等下一读；
             * 不吻合（如 ESC[ 方向键）则 ESC 作普通字节放行 */
            if (memcmp(c->in.data + i, OSC_W_PREFIX, avail) == 0)
                break;
            if (!gate_input_byte(s, c, ch, &pass)) {
                leave = true;
                break;
            }
            i++;
            continue;
        }
        if (memcmp(c->in.data + i, OSC_W_PREFIX, OSC_W_PREFIX_LEN) != 0) {
            /* 前缀不符：普通 ESC（方向键、其它 OSC 等），放行 */
            if (!gate_input_byte(s, c, ch, &pass)) {
                leave = true;
                break;
            }
            i++;
            continue;
        }
        /* 前缀完整：严格解析 rows;xcols + ST */
        size_t j = i + OSC_W_PREFIX_LEN;
        uint32_t rows = 0, cols = 0;
        int digits = 0;
        bool malformed = false;
        bool incomplete = false;
        if (j >= n || !isdigit(c->in.data[j]))
            malformed = true;
        while (!malformed && j < n && isdigit(c->in.data[j])) {
            if (digits++ >= 5) {
                malformed = true;
                break;
            }
            rows = rows * 10u + (unsigned)(c->in.data[j] - '0');
            j++;
        }
        if (!malformed) {
            if (j + 2 > n)
                incomplete = true; /* ;x 尚未到齐，缓冲结束 */
            else if (c->in.data[j] != ';' || c->in.data[j + 1] != 'x')
                malformed = true;
            else
                j += 2;
        }
        if (!malformed && !incomplete) {
            digits = 0;
            if (j >= n || !isdigit(c->in.data[j]))
                malformed = true;
            while (!malformed && j < n && isdigit(c->in.data[j])) {
                if (digits++ >= 5) {
                    malformed = true;
                    break;
                }
                cols = cols * 10u + (unsigned)(c->in.data[j] - '0');
                j++;
            }
        }
        if (!malformed && !incomplete) {
            if (j + 1 >= n)
                incomplete = true; /* ST 跨读 */
            else if (c->in.data[j] != '\033' || c->in.data[j + 1] != '\\')
                malformed = true;
            else
                j += 2;
        }
        if (malformed || incomplete) {
            if (incomplete && n - i < OSC_W_MARK_MAX)
                break; /* 半截合法标记留存，下一读拼齐 */
            /* 畸形或半截超长：ESC 当普通字节，余部重新扫描 */
            if (!gate_input_byte(s, c, ch, &pass)) {
                leave = true;
                break;
            }
            i++;
            continue;
        }
        if (rows > 65535 || cols > 65535) {
            if (!gate_input_byte(s, c, ch, &pass)) {
                leave = true;
                break;
            }
            i++;
            continue;
        }
        /* 完整 W 标记：剥离（不放行），转尺寸变更 */
        apply_winch(s, c, (uint16_t)rows, (uint16_t)cols);
        i = j;
    }
    if (i < n && !leave) {
        /* i 指向半截 W 前缀/标记：留存，其余均已裁决 */
        c->wcarry_len = n - i;
        if (c->wcarry_len > sizeof c->wcarry)
            c->wcarry_len = 0; /* 理论不可达（上限 32）：丢弃以防越界 */
        else
            memcpy(c->wcarry, c->in.data + i, c->wcarry_len);
    }
    if (pass.len) {
        buf_append(&s->pty_in, pass.data, pass.len);
        /* 循环写，未完留待会话循环续写（防短写丢数据） */
        pty_flush_input(s);
    }
    buf_free(&pass);
    /* 本批全部处理完毕（半截已留存 wcarry） */
    buf_clear(&c->in);
    if (leave)
        request_leave(s, c);
}

/* ---- 会话管理 ---- */

static void reply_and_close(session_t *s, client_t *c, const char *reason)
{
    send_mark_e(s, c, reason, true);
    c->want_close = true;
}

/* 首行握手：name 转正。list 查询走 handle_list。 */
static void handle_name(session_t *s, client_t *c, const char *name)
{
    if (c->has_name) {
        reply_and_close(s, c, "无效的握手");
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
    c->rows = DEFAULT_ROWS;
    c->cols = DEFAULT_COLS;
    snprintf(c->name, sizeof c->name, "%s", name);

    bool took_over = (s->executor_id == SH_EXEC_SHSHD);
    if (took_over)
        s->executor_id = c->id; /* 无人执行时，新加入者即时接管 */

    if (took_over) {
        /* 接管：让 shell 重打提示符，D 标记里 u= 即新执行者 */
        update_user(s);
    } else if (!s->running && whichprompt == 1) {
        /* 跟随者：单播当前提示符（零历史：不补历史输出，不惊动 shell）。
         * 运行态/PS2 中加入则不补，等下一枚 D 自然到来即可知情。 */
        unicast_current_prompt(s, c);
    }
}

/* 一次性纯文本列表查询：发 list 行的连接读到 EOF 即得整块文本。 */
static void handle_list(session_t *s, client_t *c)
{
    c->is_list = true;
    buf_t out;
    buf_init(&out);
    /* 各名字均受 SH_NAME_MAX 约束；用 %.*s 精度把写入量钉死，杜绝截断告警 */
    char tmp[SH_NAME_MAX * 2 + 64];
    snprintf(tmp, sizeof tmp,
             "%.*s\n活动用户:\n%.*s\n所有用户:\n%s",
             SH_NAME_MAX, s->name,
             SH_NAME_MAX, executor_name(s), SHSHD_NAME);
    buf_append(&out, tmp, strlen(tmp));
    for (client_t *t = s->clients; t; t = t->next) {
        if (!t->has_name)
            continue;
        snprintf(tmp, sizeof tmp, " %.*s", SH_NAME_MAX, t->name);
        buf_append(&out, tmp, strlen(tmp));
    }
    buf_append(&out, "\n", 1);
    /* list 连接从未命名：允许在 has_name 之前排队 */
    client_queue(s, c, out.data, out.len, true);
    buf_free(&out);
    c->want_close = true;
}

static void process_input(session_t *s, client_t *c)
{
    if (c->want_close || c->is_list)
        return;
    if (!c->has_name) {
        /* 首行握手：name\n 或 list\n（容忍 CRLF）。合法首行最长
         * SH_NAME_MAX 字节 + 可选 CR + LF；等不到换行又超长即拒绝。 */
        uint8_t *nl = memchr(c->in.data, '\n', c->in.len);
        if (!nl) {
            if (c->in.len > SH_NAME_MAX + 2)
                reply_and_close(s, c, "非法的用户名");
            return;
        }
        size_t llen = (size_t)(nl - c->in.data);
        size_t trim = llen;
        if (trim && c->in.data[trim - 1] == '\r')
            trim--;
        char line[SH_NAME_MAX + 1];
        bool bad = (trim == 0 || trim > SH_NAME_MAX);
        if (!bad) {
            memcpy(line, c->in.data, trim);
            line[trim] = '\0';
            if (sh_name_valid(line))
                bad = false;
            else
                bad = true;
        }
        buf_consume(&c->in, llen + 1);
        if (bad) {
            reply_and_close(s, c, "非法的用户名");
            return;
        }
        if (strcmp(line, "list") == 0) {
            handle_list(s, c);
            return;
        }
        handle_name(s, c, line);
        if (c->want_close)
            return;
    }
    if (c->has_name && c->in.len)
        feed_named_input(s, c);
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
    /* 会话关闭：向共享流广播 E 标记；session_free 的 flush_all 排空后，
     * 各客户端读到 EOF——终止信号即 EOF。 */
    char mark[256];
    size_t n = build_mark_e(mark, sizeof mark, reason);
    broadcast_bytes(s, mark, n);
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
    shsh_cmd_drained = srv_cmd_drained;

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
        /* 规范行已读出但回显尚未排空：master 通常下一个调度滴答即 EPOLLIN，
         * 事件驱动地等它；ECHO_SETTLE_MS 只是极端兜底，避免死等。 */
        if (s->echo_pending)
            timeout = ECHO_SETTLE_MS;
        int n = epoll_wait(s->epoll_fd, events, EPOLL_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fatal("epoll_wait: %s", strerror(errno));
        }
        if (n == 0 && s->echo_pending) {
            /* 兜底：再排一次，仍无输出就放行（宁可不延迟也不卡死） */
            pty_read_all(s);
            s->echo_pending = false;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (fd == s->listen_fd) {
                accept_clients(s);
                continue;
            }
            if (fd == s->pty_master) {
                if (ev & EPOLLOUT)
                    pty_flush_input(s); /* master 可写：续写积压输入 */
                if (ev & EPOLLIN)
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
                    /* 输出积压达软上限：暂停读该客户端输入（背压）。去掉
                     * EPOLLIN 免 level-trigger 忙等；输出排空后 flush_out
                     * 会恢复。生产者（终端/send）经内核/FIFO 自然阻塞。 */
                    if (c->out.len >= CLIENT_OUT_HIGH_WATER) {
                        c->read_paused = true;
                        client_update_events(s, c);
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
        /* 规范行已就绪但回显还在途：继续转事件循环（master EPOLLIN/2ms
         * 兜底），回显广播出去前不把行交给 shell，保证 回显→A 的字节序。 */
    } while (block && !s->stopping && !s->abort_cmd &&
             (!have_pending_cmd(s) || s->echo_pending));
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
    /* C 标记由 shell 层在下一次 setprompt(PS1) 发出（相位标志由 A 置位，
     * 见 parser.c），保证流中顺序 A→输出→C→D。此处无需服务端动作。 */
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

/* shell 层在 PS1 处询问："命令输入是否已全部消费、可以发 P 了吗？"
 * 排空条件：命令管道、会话行缓冲、pty slave 输入都不再有待读的规范行。
 * shell 层据此在发 C 的同一路输出里追加一枚 P（见 parser.c），顺序天然正确。 */
static int srv_cmd_drained(void)
{
    session_t *s = g_session;
    if (!s)
        return 0;
    if (s->cmd_len && memchr(s->cmd_buf, '\n', s->cmd_len))
        return 0;
    struct pollfd p = {.fd = s->cmd_pipe[0], .events = POLLIN};
    if (s->cmd_pipe[0] >= 0 && poll(&p, 1, 0) == 1 && (p.revents & POLLIN))
        return 0;
    /* pty slave 在 canonical 模式下仅在整行就绪时可读 */
    struct pollfd q = {.fd = s->pty_slave, .events = POLLIN};
    if (s->pty_slave >= 0 && poll(&q, 1, 0) == 1 && (q.revents & POLLIN))
        return 0;
    return 1;
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
        shsh_cmd_drained = NULL;
    }
    flush_all(s); /* 关停前把排队的 E 标记送出（运行循环已不再负责 flush） */
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