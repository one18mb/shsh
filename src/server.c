#include "server.h"
#include "buf.h"
#include "common.h"
#include "exec.h"
#include "parse.h"
#include "proto.h"
#include "pty.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
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
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

/* executor_id == 0 表示由 shshd（系统虚拟用户）持有执行权 */
#define SH_EXEC_SHSHD 0u
#define SHSHD_NAME "shshd"

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
    char cwd[PATH_MAX]; /* 会话工作目录（cd 内建维护） */
    int last_status; /* 上一条命令的 $? */
    /* 执行器状态：一行提交的命令按语句/pipeline 推进 */
    sh_program_t cur_prog; /* 正在执行的一行 */
    sh_stmt_t *cur_stmt;   /* 下一条待执行语句 */
    int line_status;       /* 当前行的状态（&& / || 短路依据） */
    pid_t kids[16];        /* 当前 pipeline 的子进程 */
    size_t nkids;
    pid_t cur_pgid;   /* 当前 pipeline 的进程组 */
    pid_t last_kid;   /* 最后一个子进程（其状态即 pipeline 状态） */
    int pipeline_status;
    bool exec_stop; /* exit / to：终止本行后续语句 */
    client_t *clients; /* 按连接顺序，头部 = 最早加入 */
    uint32_t next_id;
    uint32_t executor_id;
    bool running;
    uint32_t run_client_id; /* 收到 RUN_BEGIN 的客户端；0 = 无 */
    bool stopping;
};

static void client_disconnect(session_t *s, client_t *c, bool unused_err);
static bool flush_out(session_t *s, client_t *c);
static void broadcast_ps1(session_t *s);
static void send_ps1_to(session_t *s, client_t *c);
static void kill_foreground_job(session_t *s);
static void process_line(session_t *s, client_t *c, const uint8_t *data,
                         size_t len);
static void cmd_to(session_t *s, client_t *c, const char *target);

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

static void broadcast_except(session_t *s, uint32_t skip_id, const msg_t *m)
{
    buf_t f;
    buf_init(&f);
    if (msg_encode(m, &f) == 0) {
        for (client_t *c = s->clients; c; c = c->next)
            if (c->id != skip_id || !c->has_name)
                client_queue(s, c, &f, false);
    }
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

/* \W = 当前文件夹名 */
static void cwd_basename(session_t *s, char *out, size_t sz)
{
    const char *b = strrchr(s->cwd, '/');
    const char *src = b ? (b[1] ? b + 1 : "/") : s->cwd;
    size_t n = strlen(src);
    if (n >= sz)
        n = sz - 1;
    memcpy(out, src, n);
    out[n] = '\0';
}

static void compose_ps1(session_t *s, char *out, size_t sz)
{
    char w[300];
    cwd_basename(s, w, sizeof w);
    snprintf(out, sz, "%s:%s$ ", executor_name(s), w);
}

static void broadcast_ps1(session_t *s)
{
    if (s->running)
        return;
    char ps1[512];
    compose_ps1(s, ps1, sizeof ps1);
    msg_t m;
    msg_make_out(&m, ps1, strlen(ps1));
    broadcast_msg(s, &m);
    buf_free(&m.payload);
}

static void send_ps1_to(session_t *s, client_t *c)
{
    if (s->running)
        return;
    char ps1[512];
    compose_ps1(s, ps1, sizeof ps1);
    msg_t m;
    msg_make_out(&m, ps1, strlen(ps1));
    send_msg(s, c, &m, false);
    buf_free(&m.payload);
}

/* shshd 执行 to：唯一的系统广播。显示为新行上的 shshd 命令行 */
static void sys_to_line(session_t *s, const char *target)
{
    char w[300];
    cwd_basename(s, w, sizeof w);
    out_text(s, "\n%s:%s$ to %s\n", SHSHD_NAME, w, target);
}

static void broadcast_role(session_t *s)
{
    msg_t m;
    msg_make_role(&m, s->executor_id); /* 0 = shshd，客户端据此进入只读态 */
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
    if (s->run_client_id == c->id)
        s->run_client_id = 0;

    if (c->has_name && c->id == s->executor_id) {
        /* 执行者离开/掉线：先挂断其正在运行的前台作业，再移交执行权 */
        if (s->running)
            kill_foreground_job(s);
        client_t *nxt = first_named_client(s, 0);
        if (nxt) {
            s->executor_id = nxt->id;
            sys_to_line(s, nxt->name);
            broadcast_role(s);
            if (s->running) {
                /* 命令仍在退出中：让新执行者接管输入（可 ^C） */
                msg_t rb;
                msg_make_run_begin(&rb);
                send_msg(s, nxt, &rb, false);
                buf_free(&rb.payload);
                s->run_client_id = nxt->id;
            } else {
                broadcast_ps1(s);
            }
        } else {
            s->executor_id = SH_EXEC_SHSHD;
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

/* ---- 命令执行（自研最小 shell 引擎） ----
 *
 * 会话状态（cwd / 变量 / $?）保存在服务端；内建命令在服务端执行（重定向用 fd
 * 保存/恢复），外部命令在 pty 上 fork。每条 pipeline 一个进程组：Ctrl+C 与
 * 掉线挂断都由服务端 kill(-pgid) 完成，pty 本身不产生信号（见 exec.c 注释）。 */

#define SH_MAX_PIPE 16

/* 有界字符串拷贝（源可能远大于目标） */
static void copy_str(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n)
        l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static void emit_out(session_t *s, const uint8_t *p, size_t n)
{
    if (!n)
        return;
    msg_t m;
    msg_make_out(&m, p, n);
    broadcast_msg(s, &m);
    buf_free(&m.payload);
}

/* fd < 0 表示广播给所有客户端；否则写到该 fd（重定向场景） */
static void emit_fd(session_t *s, int fd, const void *p, size_t n)
{
    if (!n)
        return;
    if (fd < 0) {
        emit_out(s, p, n);
        return;
    }
    const uint8_t *b = p;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }
}

static void emit_fmt(session_t *s, int fd, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void emit_fmt(session_t *s, int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit_fd(s, fd, buf, strlen(buf));
}

static void pty_read_all(session_t *s)
{
    uint8_t buf[65536];
    for (;;) {
        ssize_t n = read(s->pty_master, buf, sizeof buf);
        if (n > 0) {
            emit_out(s, buf, (size_t)n);
            continue;
        }
        break; /* EAGAIN / EIO / EINTR */
    }
}

/* 挂断当前 pipeline（执行者掉线 / 会话结束） */
static void kill_foreground_job(session_t *s)
{
    if (s->running && s->cur_pgid > 0)
        kill(-s->cur_pgid, SIGHUP);
}

/* 变量查询：$? 取当前行已执行到的状态（行首为上一行状态），其余查环境 */
static const char *var_lookup(void *ctx, const char *name)
{
    session_t *s = ctx;
    if (strcmp(name, "?") == 0) {
        static char buf[16];
        snprintf(buf, sizeof buf, "%d", s->line_status);
        return buf;
    }
    return getenv(name);
}

/* 执行前展开：把命令的模板词与重定向目标展开为最终字符串
 * （在“执行到该语句”时才展开，因此 $? 与同行内先赋值后引用都正确） */
static void materialize_command(session_t *s, sh_command_t *cmd)
{
    if (cmd->expanded)
        return;
    cmd->expanded = 1;
    for (size_t i = 0; i < cmd->argc; i++) {
        char *e = sh_expand_word(cmd->argv[i], var_lookup, s);
        free(cmd->argv[i]);
        cmd->argv[i] = e;
    }
    for (sh_redir_t *r = cmd->redirs; r; r = r->next) {
        if (!r->target)
            continue;
        char *e = sh_expand_word(r->target, var_lookup, s);
        free(r->target);
        r->target = e;
    }
}

/* ---- 内建命令 ---- */

static bool bi_is_state(const char *n)
{
    return strcmp(n, "cd") == 0 || strcmp(n, "export") == 0 ||
           strcmp(n, "unset") == 0 || strcmp(n, "to") == 0 ||
           strcmp(n, "exit") == 0;
}

static bool bi_is_output(const char *n)
{
    return strcmp(n, "echo") == 0 || strcmp(n, "pwd") == 0 ||
           strcmp(n, "help") == 0;
}

/* 会话内 help：说明内建命令、执行权规则与快捷键 */
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
    "\n"
    "快捷键（执行者）：\n"
    "  ← →  Ctrl+A / Ctrl+E 行内移动\n"
    "  Ctrl+U               清空当前行\n"
    "  Ctrl+C / Ctrl+D      离开会话（命令运行中 Ctrl+C 为中断该命令）\n"
    "\n"
    "会话外：\n"
    "  shsh -l              列出所有会话\n"
    "  shsh -l -s <会话>    列出该会话的用户\n"
    "  shsh -s <会话> -n <用户名>   加入会话\n"
    "  shshd <会话>         启动会话\n"
    "\n"
    "当前 shell 不支持：通配符、$(...)、前缀赋值、&/作业控制、\n"
    "if/for/while 等复合语句；需要时可用 sh -c '...'。\n";

static void cmd_help(session_t *s, int out_fd)
{
    emit_fd(s, out_fd, sh_help_text, strlen(sh_help_text));
}

/* 会话变量赋值：NAME=value */
static bool apply_assignment(const char *word)
{
    const char *eq = strchr(word, '=');
    if (!eq || eq == word)
        return false;
    for (const char *q = word; q < eq; q++)
        if (!(isalnum((unsigned char)*q) || *q == '_') ||
            (q == word && isdigit((unsigned char)*q)))
            return false;
    char *name = xmalloc((size_t)(eq - word) + 1);
    memcpy(name, word, (size_t)(eq - word));
    name[eq - word] = '\0';
    if (setenv(name, eq + 1, 1) != 0)
        log_error("setenv %s 失败: %s", name, strerror(errno));
    free(name);
    return true;
}

static void cmd_cd(session_t *s, const sh_command_t *cmd, int err_fd)
{
    const char *arg = cmd->argc > 1 ? cmd->argv[1] : getenv("HOME");
    if (!arg || !*arg)
        arg = "/";
    char path[PATH_MAX];
    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
        const char *home = getenv("HOME");
        if (!home)
            home = "/";
        snprintf(path, sizeof path, "%s%s", home, arg + 1);
    } else {
        snprintf(path, sizeof path, "%s", arg);
    }
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) {
        emit_fmt(s, err_fd, "cd: %s: %s\n", arg, strerror(errno));
        s->last_status = 1;
        return;
    }
    copy_str(s->cwd, sizeof s->cwd, resolved);
    s->last_status = 0;
}

/* 内建命令实现（服务端与子进程共用）。返回状态；
 * *stop 表示需要终止本行后续语句（exit / to）。 */
static int run_builtin(session_t *s, const sh_command_t *cmd, int out_fd,
                       int err_fd, bool in_child, bool *stop)
{
    *stop = false;
    const char *name = cmd->argc > 0 ? cmd->argv[0] : NULL;
    if (!name)
        return 0;

    if (strcmp(name, "echo") == 0) {
        bool nl = true;
        size_t i = 1;
        if (cmd->argc > 1 && strcmp(cmd->argv[1], "-n") == 0) {
            nl = false;
            i = 2;
        }
        buf_t out;
        buf_init(&out);
        for (size_t k = i; k < cmd->argc; k++) {
            if (k > i)
                buf_append_byte(&out, ' ');
            buf_append_str(&out, cmd->argv[k]);
        }
        if (nl)
            buf_append_str(&out, "\n");
        emit_fd(s, out_fd, out.data, out.len);
        buf_free(&out);
        return 0;
    }
    if (strcmp(name, "pwd") == 0) {
        emit_fmt(s, out_fd, "%s\n", s->cwd);
        return 0;
    }
    if (strcmp(name, "help") == 0) {
        cmd_help(s, out_fd);
        return 0;
    }
    if (strcmp(name, "cd") == 0) {
        if (in_child) {
            /* 子进程内 cd 只影响自身副本，等价于无副作用 */
            s->last_status = 0;
            return 0;
        }
        cmd_cd(s, cmd, err_fd);
        return s->last_status;
    }
    if (strcmp(name, "export") == 0) {
        bool ok = true;
        for (size_t i = 1; i < cmd->argc; i++) {
            if (!apply_assignment(cmd->argv[i])) {
                emit_fmt(s, err_fd, "export: 用法: export NAME=value\n");
                ok = false;
                break;
            }
        }
        s->last_status = ok ? 0 : 1;
        return s->last_status;
    }
    if (strcmp(name, "unset") == 0) {
        for (size_t i = 1; i < cmd->argc; i++)
            unsetenv(cmd->argv[i]);
        s->last_status = 0;
        return 0;
    }
    if (strcmp(name, "exit") == 0) {
        if (in_child) {
            s->last_status = 0;
            *stop = true;
            return 0;
        }
        s->last_status = 0;
        *stop = true; /* 由调用方发 BYE 并关闭 */
        return 0;
    }
    if (strcmp(name, "to") == 0) {
        if (in_child) {
            emit_fmt(s, err_fd, "to: 只能在会话中直接执行\n");
            s->last_status = 1;
            return 1;
        }
        *stop = true; /* 由调用方处理（需要客户端上下文） */
        return 0;
    }
    return 127; /* 非内建 */
}

/* 子进程内的内建命令（管道 / 重定向场景） */
static int child_builtin(void *ctx, const sh_command_t *cmd)
{
    session_t *s = ctx;
    const char *name = cmd->argc > 0 ? cmd->argv[0] : NULL;
    if (!name || (!bi_is_state(name) && !bi_is_output(name)))
        return 127;
    bool stop = false;
    int st = run_builtin(s, cmd, STDOUT_FILENO, STDERR_FILENO, true, &stop);
    if (stop)
        return st; /* 子进程内 exit：直接返回 */
    return st;
}

/* ---- 服务端内建命令的重定向（fd 保存/恢复） ---- */

typedef struct {
    int fd[8];
    int saved[8];
    int n;
} saved_fds_t;

static bool redir_touches(const sh_command_t *cmd, int fd)
{
    for (const sh_redir_t *r = cmd->redirs; r; r = r->next)
        if (r->fd == fd)
            return true;
    return false;
}

static bool redir_apply_srv(const sh_command_t *cmd, saved_fds_t *sv,
                            char *err, size_t errsz)
{
    sv->n = 0;
    for (const sh_redir_t *r = cmd->redirs; r; r = r->next) {
        if (sv->n >= (int)(sizeof sv->fd / sizeof sv->fd[0])) {
            snprintf(err, errsz, "重定向过多");
            return false;
        }
        int saved = dup(r->fd);
        if (saved < 0 && errno != EBADF) {
            snprintf(err, errsz, "保存 fd %d 失败: %s", r->fd, strerror(errno));
            return false;
        }
        sv->fd[sv->n] = r->fd;
        sv->saved[sv->n] = saved;
        sv->n++;

        if (r->kind == SH_RD_DUP) {
            if (dup2(r->dup_fd, r->fd) < 0) {
                snprintf(err, errsz, "%d>&%d: %s", r->fd, r->dup_fd,
                         strerror(errno));
                return false;
            }
            continue;
        }
        int flags = r->kind == SH_RD_OUT
                        ? (O_WRONLY | O_CREAT | O_TRUNC)
                        : (r->kind == SH_RD_APPEND
                               ? (O_WRONLY | O_CREAT | O_APPEND)
                               : O_RDONLY);
        int nfd = open(r->target, flags, 0666);
        if (nfd < 0) {
            snprintf(err, errsz, "%s: %s", r->target, strerror(errno));
            return false;
        }
        if (dup2(nfd, r->fd) < 0) {
            snprintf(err, errsz, "重定向 fd %d 失败: %s", r->fd,
                     strerror(errno));
            close(nfd);
            return false;
        }
        if (nfd > STDERR_FILENO)
            close(nfd);
    }
    return true;
}

static void redir_undo_srv(saved_fds_t *sv)
{
    for (int i = 0; i < sv->n; i++) {
        if (sv->saved[i] >= 0) {
            dup2(sv->saved[i], sv->fd[i]);
            close(sv->saved[i]);
        } else {
            close(sv->fd[i]);
        }
    }
    sv->n = 0;
}

static void send_cycle_begin(session_t *s, client_t *c)
{
    msg_t m;
    msg_make_run_begin(&m);
    send_msg(s, c, &m, false);
    buf_free(&m.payload);
}

static void send_cycle_end(session_t *s, client_t *c, int status)
{
    msg_t m;
    msg_make_run_end(&m, status);
    send_msg(s, c, &m, false);
    buf_free(&m.payload);
}

/* ---- 执行器：一行 → 语句 → pipeline ---- */

static void exec_advance(session_t *s);

static void finish_line(session_t *s)
{
    int status = s->line_status;
    s->last_status = status;
    s->running = false;
    s->cur_stmt = NULL;
    s->exec_stop = false;
    s->nkids = 0;
    s->cur_pgid = 0;
    sh_program_free(&s->cur_prog);
    client_t *rc =
        s->run_client_id ? client_by_id(s, s->run_client_id) : NULL;
    s->run_client_id = 0;
    if (rc) {
        msg_t re;
        msg_make_run_end(&re, status);
        send_msg(s, rc, &re, false);
        buf_free(&re.payload);
    }
    broadcast_ps1(s);
}

/* 启动一条 pipeline。返回其状态；*forked 表示已 fork（等 SIGCHLD 推进） */
static int start_pipeline(session_t *s, sh_pipeline_t *pl, bool *forked)
{
    *forked = false;
    size_t n = pl->n;

    /* 单条命令：赋值与内建在服务端执行 */
    if (n == 1 && pl->cmds[0]->argc > 0) {
        sh_command_t *cmd = pl->cmds[0];
        materialize_command(s, cmd);
        const char *name = cmd->argv[0];
        bool is_asg = cmd->argc == 1 && apply_assignment(name);
        bool is_prefix_asg =
            !is_asg && cmd->argc > 1 && apply_assignment(name);
        if (is_asg)
            return 0;
        if (is_prefix_asg) {
            emit_fmt(s, -1, "shsh: 暂不支持前缀赋值（%s ...）\n", name);
            return 2;
        }
        if (bi_is_state(name) || bi_is_output(name)) {
            saved_fds_t sv;
            char err[160] = "";
            if (!redir_apply_srv(cmd, &sv, err, sizeof err)) {
                emit_fmt(s, -1, "shsh: %s\n", err);
                redir_undo_srv(&sv);
                return 1;
            }
            int out_fd = redir_touches(cmd, STDOUT_FILENO) ? STDOUT_FILENO : -1;
            int err_fd = redir_touches(cmd, STDERR_FILENO) ? STDERR_FILENO : -1;
            bool stop = false;
            int st = run_builtin(s, cmd, out_fd, err_fd, false, &stop);
            redir_undo_srv(&sv);
            if (stop) {
                client_t *c =
                    s->run_client_id ? client_by_id(s, s->run_client_id)
                                     : NULL;
                if (strcmp(name, "exit") == 0 && c) {
                    msg_t bye;
                    msg_make_bye(&bye, "已退出会话");
                    send_msg(s, c, &bye, false);
                    buf_free(&bye.payload);
                    c->want_close = true;
                } else if (strcmp(name, "to") == 0) {
                    cmd_to(s, c, cmd->argc > 1 ? cmd->argv[1] : "");
                }
                s->exec_stop = true;
            }
            return st;
        }
    }

    /* fork 路径（管道 / 外部命令 / 纯重定向） */
    if (n > SH_MAX_PIPE) {
        emit_fmt(s, -1, "shsh: 管道过长（最多 %d 段）\n", SH_MAX_PIPE);
        return 1;
    }
    int npipe = (int)n - 1;
    int pfd[SH_MAX_PIPE][2];
    int close_fds[SH_MAX_PIPE * 2];
    size_t nclose = 0;
    for (int i = 0; i < npipe; i++) {
        if (pipe2(pfd[i], O_CLOEXEC) != 0) {
            emit_fmt(s, -1, "shsh: 创建管道失败: %s\n", strerror(errno));
            for (int k = 0; k < i; k++) {
                close(pfd[k][0]);
                close(pfd[k][1]);
            }
            return 1;
        }
        close_fds[nclose++] = pfd[i][0];
        close_fds[nclose++] = pfd[i][1];
    }

    pid_t pgid = 0;
    s->nkids = 0;
    s->last_kid = 0;
    s->pipeline_status = 0;
    memset(s->kids, 0, sizeof s->kids);
    for (size_t i = 0; i < n; i++) {
        sh_command_t *cmd = pl->cmds[i];
        materialize_command(s, cmd);
        int in_fd = (i == 0) ? s->pty_slave : pfd[i - 1][0];
        int out_fd = (i + 1 == n) ? s->pty_slave : pfd[i][1];
        int err_fd = s->pty_slave;
        /* 显式重定向优先：该 fd 已被重定向时不再套用管道/pty 默认 */
        if (redir_touches(cmd, STDIN_FILENO))
            in_fd = -1;
        if (redir_touches(cmd, STDOUT_FILENO))
            out_fd = -1;
        if (redir_touches(cmd, STDERR_FILENO))
            err_fd = -1;
        const char *cn = cmd->argc > 0 ? cmd->argv[0] : NULL;
        sh_child_fn fn = (cn && (bi_is_state(cn) || bi_is_output(cn)))
                             ? child_builtin
                             : NULL;
        pid_t pid = exec_spawn(cmd, pgid, s->cwd, in_fd, out_fd, err_fd,
                               close_fds, nclose, fn, s);
        if (pid < 0) {
            emit_fmt(s, -1, "shsh: fork 失败: %s\n", strerror(errno));
            for (int k = 0; k < npipe; k++) {
                close(pfd[k][0]);
                close(pfd[k][1]);
            }
            s->pipeline_status = 1;
            s->nkids = 0;
            return 1;
        }
        if (pgid == 0)
            pgid = pid;
        if (s->nkids < sizeof s->kids / sizeof s->kids[0])
            s->kids[s->nkids++] = pid;
        s->last_kid = pid;
    }
    for (int i = 0; i < npipe; i++) {
        close(pfd[i][0]);
        close(pfd[i][1]);
    }
    s->cur_pgid = pgid;
    *forked = true;
    return 0;
}

static void exec_advance(session_t *s)
{
    for (;;) {
        if (s->exec_stop || !s->cur_stmt) {
            finish_line(s);
            return;
        }
        sh_stmt_t *st = s->cur_stmt;
        if ((st->op == SH_OP_AND && s->line_status != 0) ||
            (st->op == SH_OP_OR && s->line_status == 0)) {
            s->cur_stmt = st->next; /* 短路 */
            continue;
        }
        s->cur_stmt = st->next;
        bool forked = false;
        int st_status = start_pipeline(s, st->pl, &forked);
        if (forked)
            return;
        s->line_status = st_status;
    }
}

static void on_child_exit(session_t *s)
{
    bool done = false;
    for (;;) {
        int status = 0;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if (p <= 0)
            break;
        /* 固定边界遍历：nkids 在循环内会减小，不能用它作上界 */
        for (size_t i = 0; i < SH_MAX_PIPE; i++) {
            if (s->kids[i] != p)
                continue;
            s->kids[i] = 0;
            if (p == s->last_kid)
                s->pipeline_status = WIFEXITED(status)
                                         ? WEXITSTATUS(status)
                                         : 128 + WTERMSIG(status);
            s->nkids--;
            if (s->nkids == 0)
                done = true;
            break;
        }
    }
    if (done) {
        pty_read_all(s); /* 收尾输出 */
        s->line_status = s->pipeline_status;
        exec_advance(s);
    }
}

/* ---- 命令处理 ---- */

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

/* 转交执行权。target 为 "shshd" 时由 shshd 立即再转交给其他用户 */
static void cmd_to(session_t *s, client_t *c, const char *target)
{
    if (target[0] == '\0') {
        out_text(s, "to: 用法: to <用户>\r\n");
        broadcast_ps1(s);
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
        broadcast_ps1(s); /* 无一用户时提示符为 shshd:\W$ ，无人可输入 */
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
    broadcast_ps1(s);
}

/* 执行一条已提交的命令：回显共享 → 赋值 / 内建 / 外部命令 */
static void process_line(session_t *s, client_t *c, const uint8_t *data,
                         size_t len)
{
    char *line = xmalloc(len + 1);
    memcpy(line, data, len);
    line[len] = '\0';

    /* 去掉行尾 \r（终端可能发来 \r\n） */
    if (len > 0 && line[len - 1] == '\r')
        line[--len] = '\0';

    if (len == 0) {
        /* 空行：只刷新执行者自己的提示符 */
        send_cycle_begin(s, c);
        send_cycle_end(s, c, 0);
        send_ps1_to(s, c);
        free(line);
        return;
    }

    /* 命令回显共享给其他用户（执行者本人由本地回显呈现） */
    msg_t echo;
    msg_make_cmd_echo(&echo, line, len);
    broadcast_except(s, c->id, &echo);
    buf_free(&echo.payload);
    msg_t nl;
    msg_make_cmd_echo(&nl, "\n", 1);
    broadcast_except(s, c->id, &nl);
    buf_free(&nl.payload);

    if (line_token(line, "exit", NULL)) {
        free(line);
        msg_t bye;
        msg_make_bye(&bye, "已退出会话");
        send_msg(s, c, &bye, false);
        buf_free(&bye.payload);
        c->want_close = true;
        return;
    }
    const char *rest = NULL;
    if (line_token(line, "to", &rest)) {
        /* 内建命令同样用空相位括起来，保证客户端门控时序一致 */
        send_cycle_begin(s, c);
        cmd_to(s, c, rest);
        send_cycle_end(s, c, 0);
        free(line);
        return;
    }

    send_cycle_begin(s, c);
    sh_program_free(&s->cur_prog);
    sh_parse(line, &s->cur_prog);
    free(line);
    if (s->cur_prog.err[0]) {
        out_text(s, "shsh: %s\n", s->cur_prog.err);
        s->last_status = 2;
        send_cycle_end(s, c, 2);
        broadcast_ps1(s);
        return;
    }
    if (!s->cur_prog.stmts) {
        s->last_status = 0;
        send_cycle_end(s, c, 0);
        send_ps1_to(s, c);
        return;
    }
    s->running = true;
    s->run_client_id = c->id;
    s->line_status = s->last_status; /* $? 以进入本行前的状态起算 */
    s->exec_stop = false;
    s->cur_stmt = s->cur_prog.stmts;
    s->nkids = 0;
    s->cur_pgid = 0;
    exec_advance(s); /* fork 路径由 SIGCHLD 继续推进 */
}

static void handle_line(session_t *s, client_t *c, const msg_t *m)
{
    if (!c->has_name || c->id != s->executor_id || s->running) {
        /* 非法提交：回一个空相位让客户端门控复位，避免其卡住 */
        if (c->has_name) {
            send_cycle_begin(s, c);
            send_cycle_end(s, c, 1);
        }
        return;
    }
    const uint8_t *data = NULL;
    size_t len = 0;
    msg_parse_bytes(m, &data, &len);
    if (len > SH_MSG_MAX)
        return;
    process_line(s, c, data, len);
}

static void handle_raw(session_t *s, client_t *c, const msg_t *m)
{
    if (!c->has_name || c->id != s->executor_id || !s->running)
        return;
    const uint8_t *data = NULL;
    size_t len = 0;
    msg_parse_bytes(m, &data, &len);
    if (!len)
        return;
    /* Ctrl+C 由服务端翻译为整个前台进程组的 SIGINT：
     * 命令的子进程不在 pty 会话中，行规程不会自动产生信号 */
    buf_t pass;
    buf_init(&pass);
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0x03) {
            if (s->cur_pgid > 0)
                kill(-s->cur_pgid, SIGINT);
            continue;
        }
        buf_append_byte(&pass, data[i]);
    }
    if (pass.len)
        (void)!write(s->pty_master, pass.data, pass.len);
    buf_free(&pass);
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

    if (!s->running) {
        if (took_over)
            broadcast_ps1(s);
        else
            send_ps1_to(s, c);
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
    case C2S_LINE:
        handle_line(s, c, m);
        break;
    case C2S_RAW:
        handle_raw(s, c, m);
        break;
    case C2S_WINCH:
        handle_winch(s, c, m);
        break;
    case C2S_QUIT:
        c->want_close = true;
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

int session_start(session_t **out, const char *name)
{
    session_t *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->sockpath, sizeof s->sockpath, "%s/%s.sock", SH_SOCK_DIR, name);
    s->next_id = 1; /* 0 保留给 shshd */
    s->pty_master = -1;
    s->pty_slave = -1;
    if (!getcwd(s->cwd, sizeof s->cwd))
        snprintf(s->cwd, sizeof s->cwd, "/");

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

    if (pty_open(&s->pty_master, &s->pty_slave) != 0)
        fatal("无法创建 pty: %s", strerror(errno));
    s->pty_tio_ok = tcgetattr(s->pty_slave, &s->pty_tio) == 0;
    pty_set_size(s->pty_master, DEFAULT_ROWS, DEFAULT_COLS);

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

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    s->sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (s->sig_fd < 0)
        fatal("signalfd: %s", strerror(errno));
    signal(SIGPIPE, SIG_IGN);
    struct epoll_event sev = {.events = EPOLLIN, .data.fd = s->sig_fd};
    if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, s->sig_fd, &sev) < 0)
        fatal("epoll_ctl(signal): %s", strerror(errno));

    *out = s;
    return 0;
}

int session_run(session_t *s)
{
    struct epoll_event events[EPOLL_MAX_EVENTS];
    while (!s->stopping) {
        int n = epoll_wait(s->epoll_fd, events, EPOLL_MAX_EVENTS, -1);
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
            if (fd == s->sig_fd) {
                struct signalfd_siginfo si;
                while (read(s->sig_fd, &si, sizeof si) == sizeof si) {
                    if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
                        session_shutdown(s, "会话已结束");
                        break;
                    }
                    if (si.ssi_signo == SIGCHLD)
                        on_child_exit(s);
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
    }
    flush_all(s);
    return 0;
}

void session_free(session_t *s)
{
    while (s->clients)
        client_disconnect(s, s->clients, false);
    kill_foreground_job(s);
    sh_program_free(&s->cur_prog);
    unlink(s->sockpath);
    close(s->listen_fd);
    close(s->epoll_fd);
    close(s->sig_fd);
    if (s->pty_master >= 0)
        close(s->pty_master);
    if (s->pty_slave >= 0)
        close(s->pty_slave);
    free(s);
}