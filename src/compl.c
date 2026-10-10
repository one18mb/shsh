#include "compl.h"
#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

extern char **environ;

static const char *builtins[] = {"cd",  "pwd", "echo", "export", "unset",
                                 "exit", "to", "help"};

/* 引号上下文 */
enum { Q_NONE = 0, Q_DOUBLE = 1, Q_SINGLE = 2 };

void compl_free(compl_t *c)
{
    for (size_t i = 0; i < c->n; i++) {
        free(c->items[i]);
        free(c->display ? c->display[i] : NULL);
    }
    free(c->items);
    free(c->display);
    c->items = NULL;
    c->display = NULL;
    c->n = 0;
}

/* 词边界：对齐 bash 的默认 word-break 集合（空白与 "\"'><=;|&(:"） */
static bool is_delim(char c)
{
    return isspace((unsigned char)c) || c == '"' || c == '\'' || c == '>' ||
           c == '<' || c == '=' || c == ';' || c == '|' || c == '&' ||
           c == '(' || c == ':';
}

/* insert：实际插入到命令行的文本；display：清单里显示的名字 */
static void push(compl_t *c, const char *insert, const char *display)
{
    if (c->overflow)
        return;
    if (c->n >= SH_COMPL_MAX) {
        c->overflow = true;
        return;
    }
    c->items = xrealloc(c->items, (c->n + 2) * sizeof *c->items);
    c->display = xrealloc(c->display, (c->n + 2) * sizeof *c->display);
    c->items[c->n] = xstrdup(insert);
    c->display[c->n] = xstrdup(display ? display : insert);
    c->n++;
    c->items[c->n] = NULL;
    c->display[c->n] = NULL;
}

typedef struct {
    char *ins;
    char *disp;
} cand_pair_t;

static int cmp_pair(const void *a, const void *b)
{
    return strcmp(((const cand_pair_t *)a)->ins, ((const cand_pair_t *)b)->ins);
}

/* 插入文本相同即视为同一候选；排序/去重时两个数组保持同步 */
static void sort_uniq(compl_t *c)
{
    if (c->n < 2)
        return;
    cand_pair_t *p = xmalloc(c->n * sizeof *p);
    for (size_t i = 0; i < c->n; i++) {
        p[i].ins = c->items[i];
        p[i].disp = c->display[i];
    }
    qsort(p, c->n, sizeof *p, cmp_pair);
    size_t w = 1;
    for (size_t i = 1; i < c->n; i++) {
        if (strcmp(p[i].ins, p[w - 1].ins) == 0) {
            free(p[i].ins);
            free(p[i].disp);
        } else {
            p[w++] = p[i];
        }
    }
    c->n = w;
    for (size_t i = 0; i < w; i++) {
        c->items[i] = p[i].ins;
        c->display[i] = p[i].disp;
    }
    c->items[w] = NULL;
    c->display[w] = NULL;
    free(p);
}

/* ---- 转义 / 反转义 ---- */

static bool needs_escape(unsigned char c)
{
    if (isspace(c))
        return true;
    switch (c) {
    case '\\': case '\'': case '"': case '$': case '&': case '|':
    case ';': case '<': case '>': case '(': case ')': case '*':
    case '?': case '[': case ']': case '{': case '}': case '!':
    case '#': case '~': case '`':
        return true;
    default:
        return false;
    }
}

/* 按引号上下文转义：单引号内不转义；双引号内只转义 \ " $ ` */
static void escape_name(const char *s, int ctx, buf_t *out)
{
    if (ctx == Q_SINGLE) {
        buf_append_str(out, s);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (ctx == Q_DOUBLE) {
            if (*p == '"' || *p == '\\' || *p == '$' || *p == '`')
                buf_append_byte(out, '\\');
            buf_append_byte(out, *p);
        } else {
            if (needs_escape(*p))
                buf_append_byte(out, '\\');
            buf_append_byte(out, *p);
        }
    }
}

/* 反转义：用于把用户已输入（可能带转义/引号）的词转成原始名以做匹配 */
static void dequote(const char *s, char *out, size_t sz)
{
    size_t o = 0;
    int q = Q_NONE;
    for (const char *p = s; *p && o + 1 < sz; p++) {
        if (q != Q_SINGLE && *p == '\\' && p[1]) {
            out[o++] = p[1];
            p++;
            continue;
        }
        if (q == Q_NONE && (*p == '\'' || *p == '"')) {
            q = (*p == '\'') ? Q_SINGLE : Q_DOUBLE;
            continue;
        }
        if ((q == Q_SINGLE && *p == '\'') || (q == Q_DOUBLE && *p == '"')) {
            q = Q_NONE;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* line[0..upto) 处的引号上下文 */
static int quote_ctx_at(const char *line, size_t upto)
{
    int q = Q_NONE;
    for (size_t i = 0; i < upto; i++) {
        char c = line[i];
        if (q != Q_SINGLE && c == '\\' && i + 1 < upto) {
            i++;
            continue;
        }
        if (q == Q_NONE && (c == '\'' || c == '"'))
            q = (c == '\'') ? Q_SINGLE : Q_DOUBLE;
        else if ((q == Q_SINGLE && c == '\'') || (q == Q_DOUBLE && c == '"'))
            q = Q_NONE;
    }
    return q;
}

/* ---- 各类补全 ---- */

static void complete_command(compl_t *out, const char *prefix, int ctx)
{
    size_t plen = strlen(prefix);
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++) {
        if (out->overflow)
            break;
        if (strncmp(builtins[i], prefix, plen) == 0) {
            buf_t e;
            buf_init(&e);
            escape_name(builtins[i], ctx, &e);
            buf_append_byte(&e, '\0');
            push(out, (char *)e.data, (char *)e.data);
            buf_free(&e);
        }
    }

    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char *dup = xstrdup(path);
    for (char *tok = strtok(dup, ":"); tok && !out->overflow;
         tok = strtok(NULL, ":")) {
        if (!*tok)
            continue;
        DIR *d = opendir(tok);
        if (!d)
            continue;
        struct dirent *e;
        while (!out->overflow && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            if (strncmp(e->d_name, prefix, plen) != 0)
                continue;
            char full[PATH_MAX];
            size_t tl = strlen(tok), nl = strlen(e->d_name);
            if (tl + nl + 2 >= sizeof full)
                continue;
            memcpy(full, tok, tl);
            full[tl] = '/';
            memcpy(full + tl + 1, e->d_name, nl + 1);
            struct stat st;
            if (!(stat(full, &st) == 0 && S_ISREG(st.st_mode) &&
                  (st.st_mode & 0111)))
                continue;
            buf_t eb;
            buf_init(&eb);
            escape_name(e->d_name, ctx, &eb);
            buf_append_byte(&eb, '\0');
            push(out, (char *)eb.data, (char *)eb.data);
            buf_free(&eb);
        }
        closedir(d);
    }
    free(dup);
}

static void complete_var(compl_t *out, const char *prefix)
{
    size_t plen = strlen(prefix);
    for (char **e = environ; e && *e && !out->overflow; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        size_t nlen = (size_t)(eq - *e);
        if (nlen != plen || strncmp(*e, prefix, plen) != 0)
            continue;
        char name[256];
        snprintf(name, sizeof name, "$%.*s", (int)nlen, *e);
        push(out, name, name);
    }
}

/* 路径补全。dir_raw/base_raw 是用户已输入的原文（可能含转义），
 * 匹配用反转义后的 base，插入用「原文目录 + 转义后的名字」。 */
static void complete_path(compl_t *out, const char *cwd, const char *dir_raw,
                          const char *base_raw, int ctx, bool *dir_only)
{
    char base[PATH_MAX];
    dequote(base_raw, base, sizeof base);

    char full[PATH_MAX];
    const char *look;
    if (dir_raw[0] == '\0') {
        size_t cl = strlen(cwd);
        if (cl + 2 >= sizeof full)
            return;
        memcpy(full, cwd, cl);
        full[cl] = '/';
        full[cl + 1] = '\0';
        look = full;
    } else if (dir_raw[0] == '/') {
        look = dir_raw;
    } else if (dir_raw[0] == '~' && (dir_raw[1] == '/' || dir_raw[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home)
            home = "/";
        size_t hl = strlen(home), dl = strlen(dir_raw + 1);
        if (hl + dl + 2 >= sizeof full)
            return;
        memcpy(full, home, hl);
        full[hl] = '/';
        memcpy(full + hl + 1, dir_raw + 1, dl + 1);
        look = full;
    } else {
        size_t cl = strlen(cwd), dl = strlen(dir_raw);
        if (cl + dl + 2 >= sizeof full)
            return;
        memcpy(full, cwd, cl);
        full[cl] = '/';
        memcpy(full + cl + 1, dir_raw, dl + 1);
        look = full;
    }

    DIR *d = opendir(look);
    if (!d)
        return;
    size_t blen = strlen(base);
    struct dirent *e;
    while (!out->overflow && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, base, blen) != 0)
            continue;
        if (e->d_name[0] == '.' && base[0] != '.')
            continue;
        size_t ll = strlen(look), nl = strlen(e->d_name);
        if (ll + nl + 2 >= sizeof full)
            continue;
        memcpy(full, look, ll);
        full[ll] = '/';
        memcpy(full + ll + 1, e->d_name, nl + 1);
        struct stat st;
        bool isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        *dir_only = isdir;

        buf_t disp; /* 清单里只显示待选条目名（bash 风格），不显示完整路径 */
        buf_init(&disp);
        escape_name(e->d_name, ctx, &disp);
        if (isdir)
            buf_append_byte(&disp, '/');
        buf_append_byte(&disp, '\0');

        buf_t cand;
        buf_init(&cand);
        buf_append_str(&cand, dir_raw); /* 目录部分保持用户原样 */
        buf_append_str(&cand, (char *)disp.data);
        buf_append_byte(&cand, '\0');
        push(out, (char *)cand.data, (char *)disp.data);
        buf_free(&cand);
        buf_free(&disp);
    }
    closedir(d);
}

/* ---- 候选清单：按执行者终端宽度做列对齐（列优先，bash 风格） ---- */

static void build_listing(compl_t *c, int cols, buf_t *listing)
{
    if (c->n == 0)
        return;
    if (cols < 20)
        cols = 80;

    size_t maxw = 0;
    for (size_t i = 0; i < c->n; i++) {
        size_t l = strlen(c->display[i]);
        if (l > maxw)
            maxw = l;
    }
    size_t colw = maxw + 2;
    size_t ncols = (size_t)cols / colw;
    if (ncols < 1)
        ncols = 1;
    size_t nrows = (c->n + ncols - 1) / ncols;

    for (size_t r = 0; r < nrows; r++) {
        for (size_t col = 0; col < ncols; col++) {
            size_t idx = col * nrows + r;
            if (idx >= c->n)
                continue;
            buf_append_str(listing, c->display[idx]);
            bool last_in_row = (idx + nrows >= c->n);
            if (!last_in_row) {
                size_t l = strlen(c->display[idx]);
                for (size_t k = l; k < colw; k++)
                    buf_append_byte(listing, ' ');
            }
        }
        buf_append_str(listing, "\r\n");
    }
}

/* 替换文本 = 用户已输入的原词（不改变输入） */
static void set_repl_original(buf_t *replace, const char *line, size_t start,
                              size_t cursor)
{
    buf_clear(replace);
    if (cursor > start)
        buf_append(replace, line + start, cursor - start);
}

void compl_compute(const char *line, size_t cursor, const char *cwd, int cols,
                   compl_t *out, size_t *word_start, buf_t *replace,
                   buf_t *listing)
{
    out->items = NULL;
    out->display = NULL;
    out->n = 0;
    out->overflow = false;
    buf_clear(replace);
    buf_clear(listing);

    if (cursor > strlen(line))
        cursor = strlen(line);
    size_t start = cursor;
    while (start > 0 && !is_delim(line[start - 1]))
        start--;
    *word_start = start;

    char word[512];
    size_t wlen = cursor - start;
    if (wlen >= sizeof word)
        wlen = sizeof word - 1;
    memcpy(word, line + start, wlen);
    word[wlen] = '\0';

    /* 是否命令位：该词之前只有空白 */
    bool first = true;
    for (size_t i = 0; i < start; i++)
        if (!isspace((unsigned char)line[i])) {
            first = false;
            break;
        }

    int ctx = quote_ctx_at(line, start);
    char dq[512];
    dequote(word, dq, sizeof dq);

    bool dir_only = false;
    if (word[0] == '$' && ctx != Q_SINGLE) {
        complete_var(out, word + 1);
    } else if (first && !strchr(dq, '/')) {
        complete_command(out, dq, ctx);
    } else {
        const char *slash = strrchr(dq, '/');
        /* 目录部分按「原文」取，长度对齐：用 word 的同一偏移 */
        size_t dir_raw_len = 0;
        if (slash) {
            size_t dq_off = (size_t)(slash - dq) + 1;
            dir_raw_len = dq_off <= wlen ? dq_off : wlen;
        }
        char dir_raw[512];
        memcpy(dir_raw, word, dir_raw_len);
        dir_raw[dir_raw_len] = '\0';
        complete_path(out, cwd, dir_raw, word + dir_raw_len, ctx, &dir_only);
    }
    sort_uniq(out);

    if (out->overflow) {
        /* 目标过多：不展开、不列清单，只提示（保持光标与输入不变） */
        set_repl_original(replace, line, start, cursor);
        buf_append_str(listing, "候选过多（超过 ");
        char nbuf[16];
        snprintf(nbuf, sizeof nbuf, "%d", SH_COMPL_MAX);
        buf_append_str(listing, nbuf);
        buf_append_str(listing, " 个），请继续输入更具体的前缀\r\n");
        return;
    }
    if (out->n == 0) {
        set_repl_original(replace, line, start, cursor); /* 无候选：输入原样保留 */
        return;
    }
    if (out->n == 1) {
        buf_clear(replace);
        buf_append_str(replace, out->items[0]);
        if (word[0] == '$' && ctx != Q_SINGLE) {
            /* 变量补全不加空格 */
        } else if (dir_only) {
            /* 目录候选已带 /，停下等下一次 Tab 逐级下探 */
        } else {
            buf_append_byte(replace, ' ');
        }
        return;
    }

    /* 多候选：给出共同前缀（在转义后的字符串上计算，避免插到转义符中间） */
    size_t common = strlen(out->items[0]);
    for (size_t i = 1; i < out->n; i++) {
        size_t j = 0;
        while (j < common && out->items[i][j] == out->items[0][j])
            j++;
        common = j;
    }
    if (common > wlen)
        buf_append(replace, out->items[0], common);
    else
        set_repl_original(replace, line, start, cursor); /* 无进展：保留原词 */
    build_listing(out, cols, listing);
}