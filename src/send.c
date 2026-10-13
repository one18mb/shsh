/* send —— shsh 统一文本协议的同步命令通道（非交互，供脚本/Agent）。
 *
 * send 既不连 shshd、也不启动 shsh：它只与一个"已运行的 shsh 端点"通过
 * ~/.shsh/<会话>__<用户>.{in,out,cursor,pid} 文件通信。shsh 是唯一的
 * shshd 客户端，同时充当端点 —— send 往 FIFO 写命令，shsh 把它转发给
 * shshd；shshd 的输出被 shsh 原样 tee 到 .out，send 从游标读增量。
 *
 * 用法：
 *   send [-n name] [-s session] [-l] [-t 秒] <命令>
 *   send [-n name] [-s session] [-l] [-t 秒] -        # 从 stdin 读多行脚本
 *   send -h
 *
 * 命令可含换行（多行脚本）：把整段注入 FIFO，逐行交付执行，全部结束后返回。
 * 默认不回显"自己这条命令的输入"（省 token，只留命令输出）；-l 则连回显
 * 一起返回（调试用）。别人的活动始终返回。
 *
 * 目标端点寻址（优先级）：-n/-s > $SHSH_NAME/$SHSH_SESSION > ~/.shsh/current
 * （上次成功绑定）。成功后把绑定写回 ~/.shsh/current。
 *
 * 退出码：0 成功；1 运行错误（端点不存在/超时/会话关闭）；2 用法错误。 */

#include "common.h"
#include "osc633.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#define DEFAULT_TIMEOUT 300.0
#define REASON_MAX      256
#define IDLE_MS         100

typedef struct {
    char in[PATH_MAX], out[PATH_MAX], cur[PATH_MAX], pid[PATH_MAX];
} paths_t;

static void die(const char *fmt, ...)
{
    va_list ap;
    fputs("send: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void die_usage(const char *fmt, ...)
{
    va_list ap;
    fputs("send: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void paths_build(paths_t *p, const char *session, const char *name)
{
    const char *home = getenv("HOME");
    char base[PATH_MAX - 16];
    if (!home || !*home)
        die("HOME 未设置");
    int r = snprintf(base, sizeof base, "%s/.shsh/%s__%s", home, session,
                     name);
    if (r < 0 || (size_t)r >= sizeof base)
        die("路径过长");
    snprintf(p->in,  sizeof p->in,  "%s.in", base);
    snprintf(p->out, sizeof p->out, "%s.out", base);
    snprintf(p->cur, sizeof p->cur, "%s.cursor", base);
    snprintf(p->pid, sizeof p->pid, "%s.pid", base);
}

static void current_path(char *buf, size_t sz)
{
    const char *home = getenv("HOME");
    snprintf(buf, sz, "%s/.shsh/current", (home && *home) ? home : ".");
}

static void copy_bounded(char *dst, size_t sz, const char *src)
{
    size_t n = strlen(src);
    if (n >= sz)
        n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void current_read(char *name, size_t nsz, char *session, size_t ssz)
{
    char path[PATH_MAX];
    current_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[SH_NAME_MAX + 2];
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (*line)
            copy_bounded(name, nsz, line);
    }
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (*line)
            copy_bounded(session, ssz, line);
    }
    fclose(f);
}

static void current_write(const char *name, const char *session)
{
    char path[PATH_MAX];
    current_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f)
        return; /* best-effort：绑定写不进去不影响本次执行 */
    fprintf(f, "%s\n%s\n", name, session);
    fclose(f);
}

static int read_pid(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1)
        pid = 0;
    fclose(f);
    return pid;
}

static int alive(int pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static long long read_cursor(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    long long v = 0;
    if (fscanf(f, "%lld", &v) != 1)
        v = 0;
    fclose(f);
    return v;
}

static void write_cursor(const char *path, long long v)
{
    FILE *f = fopen(path, "w");
    if (!f)
        die("无法写游标 %s: %s", path, strerror(errno));
    fprintf(f, "%lld\n", v);
    fclose(f);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- 共享流的 633 状态机（默认抑制自身输入回显） ---- */

typedef struct {
    const char *self;
    char exec[SH_NAME_MAX + 1];
    int have_exec;
    int opt_echo;      /* -l：连自身输入回显也返回 */
    int suppress_visible; /* 待抑制的可见字节数（自身 PS1 / 续行 PS2） */
    int echo_phase;    /* 注入后、见自身首枚 A 前：其间可见字节为回显 */
    int have_to;       /* 命令是 to <目标>：完成条件是 D;u=目标 */
    char to_target[SH_NAME_MAX + 1];
    int injected;
    int c_since_inject; /* 注入后已见的自身 C 数（P 判定结束用） */
    int done, got_e;
    int silent;        /* 前缀扫描：只更新状态，不外发不完成 */
    char reason[REASON_MAX];
    long long chunk_base;
    const uint8_t *chunk;
    long long mark_end_abs;
} pc_t;

static void field_get(const uint8_t *q, size_t n, const char *key,
                      char *out, size_t sz)
{
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && q[e] != ';')
            e++;
        if (e - i >= klen && memcmp(q + i, key, klen) == 0) {
            size_t vlen = e - i - klen;
            if (vlen >= sz)
                vlen = sz - 1;
            memcpy(out, q + i + klen, vlen);
            out[vlen] = '\0';
            return;
        }
        i = (e < n) ? e + 1 : e;
    }
    out[0] = '\0';
}

static void on_raw(void *ctx, const uint8_t *p, size_t n)
{
    pc_t *c = ctx;
    if (c->silent)
        return;
    size_t i = 0;
    if (c->suppress_visible > 0) {
        size_t k = (size_t)c->suppress_visible;
        if (k > n)
            k = n;
        c->suppress_visible -= (int)k;
        i = k;
    }
    if (c->echo_phase)
        return; /* 注入后的回显区：整段抑制 */
    if (i < n)
        fwrite(p + i, 1, n - i, stdout);
}

static void on_mark(void *ctx, int is633, char code, const uint8_t *args,
                    size_t alen, const uint8_t *raw, size_t rawlen)
{
    pc_t *c = ctx;
    if (!is633)
        return; /* 非 633 的其它 OSC：不外发 */
    if (code == 'D') {
        char u[SH_NAME_MAX + 1];
        field_get(args, alen, "u=", u, sizeof u);
        if (u[0]) {
            snprintf(c->exec, sizeof c->exec, "%s", u);
            c->have_exec = 1;
        }
        if (c->silent)
            return;
        /* 自身提示符：按 PS1 长度精确抑制可见文本 "<u>:<w>$ "；他人正常回显 */
        if (!c->opt_echo && u[0] && strcmp(u, c->self) == 0) {
            char w[SH_NAME_MAX + 1];
            field_get(args, alen, "w=", w, sizeof w);
            c->suppress_visible = (int)(strlen(u) + strlen(w) + 3);
        }
        if (c->have_to && c->injected && u[0] &&
            strcmp(u, c->to_target) == 0)
            c->done = 1;
    } else if (code == 'A') {
        if (!c->silent && c->have_exec && strcmp(c->exec, c->self) == 0)
            c->echo_phase = 0; /* 回显区结束，其后为命令输出 */
    } else if (code == 'C') {
        /* 一条顶层命令结束（携带退出码） */
        if (!c->silent && !c->have_to && c->have_exec &&
            strcmp(c->exec, c->self) == 0)
            c->c_since_inject++;
    } else if (code == 'P') {
        /* 空闲且命令队列已排空：本批输入全部消费完 → 结束。
         * 要求已注入且至少见过一枚自身 C，避开注入前那一枚 P。 */
        if (!c->silent && !c->have_to && c->injected &&
            c->c_since_inject >= 1)
            c->done = 1;
    } else if (code == 'Q') {
        /* 续行提示符(PS2)：args 为十进制长度，抑制其后这么多可见字节。
         * -l 模式不抑制（与 PS1 一致，保留完整回显）。 */
        if (!c->silent && !c->opt_echo) {
            int len = 0;
            for (size_t k = 0; k < alen && args[k] >= '0' && args[k] <= '9';
                 k++)
                len = len * 10 + (args[k] - '0');
            c->suppress_visible = len;
        }
    } else if (code == 'E') {
        if (!c->silent) {
            field_get(args, alen, "reason=", c->reason, sizeof c->reason);
            c->got_e = 1;
        }
    }
    if (c->done)
        c->mark_end_abs = c->chunk_base +
                          (long long)((raw + rawlen) - c->chunk);
}

/* ---- 执行 ---- */

/* 把整段命令写入端点 FIFO。非阻塞打开（无读者即 ENXIO，不会永久阻塞），
 * 写满则 poll 等可写、期间复查端点存活——任何异常都干净报错返回，绝不因
 * SIGPIPE/阻塞卡死或崩溃。 */
static void inject(const paths_t *p, const char *text)
{
    size_t total = strlen(text);
    int fd = open(p->in, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENXIO)
            die("端点已断开（FIFO 无读者），中止");
        die("打开注入 FIFO 失败: %s", strerror(errno));
    }
    size_t off = 0;
    while (off < total) {
        ssize_t w = write(fd, text + off, total - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!alive(read_pid(p->pid))) {
                close(fd);
                die("端点已退出（日志见同目录 .log），中止");
            }
            struct pollfd pf = {.fd = fd, .events = POLLOUT};
            poll(&pf, 1, 1000); /* 等可写；超时后重试并复查存活 */
            continue;
        }
        int e = errno;
        close(fd);
        die("写入 FIFO 失败: %s", e == EPIPE ? "端点已断开" : strerror(e));
    }
    close(fd);
}

static int drive(pc_t *c, const paths_t *p, const char *cmd, double timeout)
{
    int pid = read_pid(p->pid);
    if (!alive(pid))
        die("端点 shsh 未运行（%s 无存活进程）；请先 shsh login 建端点",
            p->pid);
    FILE *f = fopen(p->out, "rb");
    if (!f)
        die("端点不存在：%s 打不开（先 shsh login 建端点）", p->out);
    long long cur = read_cursor(p->cur);

    osc633_scan_t scan;
    osc633_scan_init(&scan);

    /* 前缀：静默扫描 [0,cur) 建立初始角色（exec） */
    c->silent = 1;
    {
        uint8_t b[65536];
        long long pos = 0;
        fseek(f, 0, SEEK_SET);
        while (pos < cur) {
            size_t wan = (size_t)(cur - pos);
            if (wan > sizeof b)
                wan = sizeof b;
            size_t rd = fread(b, 1, wan, f);
            if (rd == 0)
                break;
            c->chunk = b;
            c->chunk_base = pos;
            osc633_scan_feed(&scan, b, rd, c, on_raw, on_mark);
            pos += (long long)rd;
        }
    }
    c->silent = 0;
    /* 起始抑制态：若游标恰好落在自身提示符 D 之后，下一枚 D 会自动设置；
     * 此处不预置，保持 suppress_visible/echo_phase 为零 */

    long long pos = cur;
    fseek(f, pos, SEEK_SET);
    double deadline = now_sec() + timeout;

    for (;;) {
        /* 投递放循环顶部：完成点（P）之后增量可能为空，不能只在读到新数据时
         * 才投递，否则会一直空等。成为执行者即投递；A 出现前执行权被转走
         * 则稍后重投。 */
        if (!c->injected) {
            if (c->have_exec && strcmp(c->exec, c->self) == 0) {
                inject(p, cmd);
                c->injected = 1;
                c->c_since_inject = 0;
                if (!c->opt_echo)
                    c->echo_phase = 1; /* 其后至首枚 A 的可见字节为回显 */
            }
        } else if (!c->have_to && c->c_since_inject == 0 && c->have_exec &&
                   strcmp(c->exec, c->self) != 0) {
            c->injected = 0;
        }

        struct stat st;
        if (fstat(fileno(f), &st) != 0)
            die("读取 out 失败");
        long long size = (long long)st.st_size;
        if (size <= pos) {
            if (!alive(pid))
                die("端点 shsh 已退出（日志同目录 .log）");
            if (now_sec() > deadline)
                break;
            usleep(IDLE_MS * 1000);
            continue;
        }
        uint8_t b[65536];
        size_t wan = (size_t)(size - pos);
        if (wan > sizeof b)
            wan = sizeof b;
        size_t rd = fread(b, 1, wan, f);
        if (rd == 0)
            break;
        c->chunk = b;
        c->chunk_base = pos;
        osc633_scan_feed(&scan, b, rd, c, on_raw, on_mark);
        pos += (long long)rd;
        fflush(stdout);
        if (c->got_e)
            die("会话关闭: %s", c->reason[0] ? c->reason : "未知原因");
        if (c->done) {
            write_cursor(p->cur, c->mark_end_abs);
            return 0;
        }
    }
    fprintf(stderr, "send: 执行超时（%.0fs）", timeout);
    if (c->have_exec && strcmp(c->exec, c->self) != 0)
        fprintf(stderr, "；执行权在 %s 处", c->exec);
    fprintf(stderr, "\n");
    return 1;
}

/* 识别整段内容是否为单条 to <目标>；返回 1 表示是，目标写入 target。
 * 只看第一行，且要求其后没有别的行（多行脚本不按 to 处理）。 */
static int to_target_of(const char *cmd, char *target, size_t tsz)
{
    target[0] = '\0';
    char line[SH_NAME_MAX + 16];
    size_t n = 0;
    while (cmd[n] && cmd[n] != '\n' && n < sizeof line - 1) {
        line[n] = cmd[n];
        n++;
    }
    line[n] = '\0';
    /* 第一行之后还有内容 → 是脚本，不是 to */
    if (cmd[n] == '\n' && cmd[n + 1] != '\0')
        return 0;
    size_t m = strlen(line);
    while (m > 0 && (line[m - 1] == ' ' || line[m - 1] == '\t' ||
                     line[m - 1] == '\r'))
        line[--m] = '\0';
    if (strcmp(line, "to") == 0)
        return 1;
    if (line[0] == 't' && line[1] == 'o' &&
        (line[2] == ' ' || line[2] == '\t')) {
        const char *r = line + 2;
        while (*r == ' ' || *r == '\t')
            r++;
        copy_bounded(target, tsz, r);
        return 1;
    }
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "用法:\n"
        "  send [-n 用户名] [-s 会话名] [-l] [-t 秒] <命令>\n"
        "  send [-n 用户名] [-s 会话名] [-l] [-t 秒] -   从 stdin 读多行脚本\n"
        "  send -h\n"
        "\n"
        "  -n/-s  目标端点（省略时取 $SHSH_NAME/$SHSH_SESSION 或 ~/.shsh/current）\n"
        "  -l     连自身命令回显一起返回（默认只返回命令输出，省 token）\n"
        "  -t     超时秒数（默认 %d，可用 $SHSH_SEND_TIMEOUT 覆盖）\n"
        "  命令   一个参数：可含换行；为 - 时从 stdin 读\n",
        (int)DEFAULT_TIMEOUT);
}

static char *read_stdin_all(void)
{
    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        ssize_t n = read(STDIN_FILENO, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            die("读 stdin 失败: %s", strerror(errno));
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    const char *opt_n = NULL, *opt_s = NULL;
    int opt_l = 0;
    double timeout = DEFAULT_TIMEOUT;
    const char *cmd = NULL;
    int endflags = 0;

    signal(SIGPIPE, SIG_IGN); /* FIFO/socket 断开走返回码，不因信号崩溃 */

    const char *envt = getenv("SHSH_SEND_TIMEOUT");
    if (envt && *envt) {
        double v = atof(envt);
        if (v > 0)
            timeout = v;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!endflags && strcmp(a, "--") == 0) {
            endflags = 1;
        } else if (!endflags && strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (!endflags && strcmp(a, "-n") == 0) {
            if (++i >= argc)
                die_usage("-n 需要用户名");
            opt_n = argv[i];
        } else if (!endflags && strcmp(a, "-s") == 0) {
            if (++i >= argc)
                die_usage("-s 需要会话名");
            opt_s = argv[i];
        } else if (!endflags && strcmp(a, "-l") == 0) {
            opt_l = 1;
        } else if (!endflags && strcmp(a, "-t") == 0) {
            if (++i >= argc)
                die_usage("-t 需要秒数");
            double v = atof(argv[i]);
            if (v <= 0)
                die_usage("超时秒数无效: %s", argv[i]);
            timeout = v;
        } else if (!cmd) {
            cmd = a; /* 最后一个位置参数即命令 */
        } else {
            die_usage("多余参数: %s（命令应作为单个参数传入）", a);
        }
    }
    if (!cmd) {
        usage(stderr);
        return 2;
    }

    /* 命令文本：- 从 stdin 读；否则原样（可含换行）。保证以换行结尾。 */
    char *text;
    if (strcmp(cmd, "-") == 0) {
        text = read_stdin_all();
    } else {
        text = xstrdup(cmd);
    }
    size_t tlen = strlen(text);
    if (tlen == 0 || text[tlen - 1] != '\n') {
        text = xrealloc(text, tlen + 2);
        text[tlen] = '\n';
        text[tlen + 1] = '\0';
        tlen++;
    }

    /* 目标解析：-n/-s > env > ~/.shsh/current */
    char name[SH_NAME_MAX + 1] = "";
    char session[SH_NAME_MAX + 1] = "";
    if (opt_n)
        snprintf(name, sizeof name, "%s", opt_n);
    if (opt_s)
        snprintf(session, sizeof session, "%s", opt_s);
    if (!name[0]) {
        const char *e = getenv("SHSH_NAME");
        if (e && *e)
            snprintf(name, sizeof name, "%s", e);
    }
    if (!session[0]) {
        const char *e = getenv("SHSH_SESSION");
        if (e && *e)
            snprintf(session, sizeof session, "%s", e);
    }
    if (!name[0] || !session[0]) {
        char cn[SH_NAME_MAX + 1] = "", cs[SH_NAME_MAX + 1] = "";
        current_read(cn, sizeof cn, cs, sizeof cs);
        if (!name[0] && cn[0])
            snprintf(name, sizeof name, "%s", cn);
        if (!session[0] && cs[0])
            snprintf(session, sizeof session, "%s", cs);
    }
    if (!session[0])
        snprintf(session, sizeof session, "%s", SH_DEFAULT_SESSION);
    if (!name[0])
        die_usage("未指定用户名：用 -n、$SHSH_NAME 或先 send login");

    if (!sh_name_valid(name))
        die_usage("非法的用户名: %s", name);
    if (!sh_name_valid(session))
        die_usage("非法的会话名: %s", session);

    paths_t p;
    paths_build(&p, session, name);

    pc_t c;
    memset(&c, 0, sizeof c);
    c.self = name;
    c.opt_echo = opt_l;
    c.have_to = to_target_of(text, c.to_target, sizeof c.to_target);

    /* 端点存在性校验（send 不启动 shsh） */
    if (!alive(read_pid(p.pid)))
        die("端点 %s__%s 不存在；请先: shsh login -n %s -s %s [-d]",
            session, name, name, session);

    int rc = drive(&c, &p, text, timeout);
    free(text);
    if (rc == 0)
        current_write(name, session); /* 成功后写回绑定 */
    return rc;
}