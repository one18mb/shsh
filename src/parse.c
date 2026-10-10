#include "parse.h"
#include "buf.h"
#include "common.h"

#include <ctype.h>

/* ---- 词法：token ---- */

enum {
    T_WORD = 0,
    T_PIPE,
    T_SEMI,
    T_AND,
    T_OR,
    T_REDIR, /* rd_kind / rd_fd / rd_dup_from / text=文件名 */
};

typedef struct {
    int type;
    char *text; /* T_WORD / T_REDIR(文件目标) */
    int rd_kind;
    int rd_fd;
    int rd_dup_from;
} tok_t;

typedef struct {
    tok_t *toks;
    size_t n, cap;
    char err[160];
} lex_t;

static bool name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* 展开 $VAR / ${VAR} / $?；*pp 指向 '$'，返回时指向已消费文本之后 */
static void expand(buf_t *w, const char **pp, sh_lookup_fn lookup, void *ctx)
{
    const char *p = *pp + 1;
    char name[128];
    size_t n = 0;

    if (*p == '{') {
        p++;
        while (*p && *p != '}' && n < sizeof name - 1)
            name[n++] = *p++;
        if (*p == '}')
            p++;
    } else if (*p == '?') {
        name[n++] = '?';
        p++;
    } else {
        while (name_char(*p) && n < sizeof name - 1)
            name[n++] = *p++;
    }
    name[n] = '\0';

    if (n == 0) {
        buf_append_byte(w, '$'); /* 单独的 $ 视为字面量 */
        *pp = p;
        return;
    }
    const char *v = lookup ? lookup(ctx, name) : NULL;
    if (v)
        buf_append_str(w, v);
    *pp = p;
}

/* 扫描一个词：返回「模板」（保留原文的引号与转义，不含展开），
 * 展开在执行时进行（见 sh_expand_word），这样 $? / 同行内先赋值后引用才正确。 */
static char *scan_word(const char **pp, char *err, size_t errsz)
{
    const char *p = *pp;
    const char *start = p;
    int quote = 0;

    for (; *p;) {
        char c = *p;
        if (quote == 0) {
            if (isspace((unsigned char)c) || c == ';' || c == '|' ||
                c == '&' || c == '<' || c == '>')
                break;
            if (c == '\\' && p[1]) {
                p += 2;
                continue;
            }
            if (c == '\\' && p[1] == '\0') {
                p++;
                continue;
            }
            if (c == '\'') {
                quote = '\'';
                p++;
                continue;
            }
            if (c == '"') {
                quote = '"';
                p++;
                continue;
            }
            p++;
            continue;
        }
        if (quote == '\'') {
            if (c == '\'')
                quote = 0;
            p++;
            continue;
        }
        if (c == '"') {
            quote = 0;
            p++;
            continue;
        }
        if (c == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
            p += 2;
            continue;
        }
        p++;
    }
    if (quote != 0) {
        snprintf(err, errsz, "语法错误：缺少收尾的%s引号",
                 quote == '\'' ? "单" : "双");
        return NULL;
    }
    size_t len = (size_t)(p - start);
    char *raw = xmalloc(len + 1);
    memcpy(raw, start, len);
    raw[len] = '\0';
    *pp = p;
    return raw;
}

/* 展开一个模板（执行时调用）：引号/转义 + $VAR/${VAR}/$? */
static char *scan_expand(const char **pp, sh_lookup_fn lookup, void *ctx,
                         char *err, size_t errsz)
{
    buf_t word;
    buf_init(&word);
    const char *p = *pp;
    int quote = 0;

    for (; *p;) {
        char c = *p;
        if (quote == 0) {
            if (isspace((unsigned char)c) || c == ';' || c == '|' ||
                c == '&' || c == '<' || c == '>')
                break;
            if (c == '\'') {
                quote = '\'';
                p++;
                continue;
            }
            if (c == '"') {
                quote = '"';
                p++;
                continue;
            }
            if (c == '\\') {
                if (p[1]) {
                    buf_append_byte(&word, (uint8_t)p[1]);
                    p += 2;
                } else {
                    buf_append_byte(&word, '\\');
                    p++;
                }
                continue;
            }
            if (c == '$') {
                expand(&word, &p, lookup, ctx);
                continue;
            }
            buf_append_byte(&word, (uint8_t)c);
            p++;
            continue;
        }
        if (quote == '\'') {
            if (c == '\'') {
                quote = 0;
                p++;
                continue;
            }
            buf_append_byte(&word, (uint8_t)c);
            p++;
            continue;
        }
        if (c == '"') {
            quote = 0;
            p++;
            continue;
        }
        if (c == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
            buf_append_byte(&word, (uint8_t)p[1]);
            p += 2;
            continue;
        }
        if (c == '$') {
            expand(&word, &p, lookup, ctx);
            continue;
        }
        buf_append_byte(&word, (uint8_t)c);
        p++;
    }
    if (quote != 0) {
        snprintf(err, errsz, "缺少收尾引号");
        buf_free(&word);
        return NULL;
    }
    buf_append_byte(&word, '\0');
    char *res = xstrdup((char *)word.data);
    buf_free(&word);
    *pp = p;
    return res;
}

char *sh_expand_word(const char *tpl, sh_lookup_fn lookup, void *ctx)
{
    const char *p = tpl;
    char err[64];
    char *r = scan_expand(&p, lookup, ctx, err, sizeof err);
    return r ? r : xstrdup("");
}

static void tok_push(lex_t *lx, tok_t t)
{
    if (lx->n + 1 >= lx->cap) {
        lx->cap = lx->cap ? lx->cap * 2 : 8;
        lx->toks = xrealloc(lx->toks, lx->cap * sizeof *lx->toks);
    }
    lx->toks[lx->n++] = t;
}

static void lex_free(lex_t *lx)
{
    for (size_t i = 0; i < lx->n; i++)
        free(lx->toks[i].text);
    free(lx->toks);
    lx->toks = NULL;
    lx->n = lx->cap = 0;
}

static void lex_line(const char *line, lex_t *lx)
{
    lx->toks = NULL;
    lx->n = lx->cap = 0;
    lx->err[0] = '\0';
    const char *p = line;

    while (*p) {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        int fd = -1;
        if (isdigit((unsigned char)*p) && (p[1] == '>' || p[1] == '<')) {
            fd = *p - '0';
            p++;
        }
        if (*p == ';') {
            tok_t t = {.type = T_SEMI};
            tok_push(lx, t);
            p++;
            continue;
        }
        if (*p == '|' && p[1] == '|') {
            tok_t t = {.type = T_OR};
            tok_push(lx, t);
            p += 2;
            continue;
        }
        if (*p == '|') {
            tok_t t = {.type = T_PIPE};
            tok_push(lx, t);
            p++;
            continue;
        }
        if (*p == '&' && p[1] == '&') {
            tok_t t = {.type = T_AND};
            tok_push(lx, t);
            p += 2;
            continue;
        }
        if (*p == '&') {
            snprintf(lx->err, sizeof lx->err, "暂不支持后台执行（&）");
            return;
        }
        if (*p == '>' || *p == '<') {
            bool is_out = (*p == '>');
            p++;
            tok_t t = {.type = T_REDIR,
                       .rd_fd = fd >= 0 ? fd : (is_out ? 1 : 0),
                       .rd_kind = is_out ? SH_RD_OUT : SH_RD_IN};
            if (is_out && *p == '>') {
                t.rd_kind = SH_RD_APPEND;
                p++;
            }
            if (*p == '&') {
                p++;
                if (isdigit((unsigned char)*p)) {
                    t.rd_kind = SH_RD_DUP;
                    t.rd_dup_from = *p - '0';
                    p++;
                } else {
                    snprintf(lx->err, sizeof lx->err,
                             "语法错误：%s& 需要数字目标（如 2>&1）",
                             is_out ? ">" : "<");
                    return;
                }
            } else if (*p && !isspace((unsigned char)*p) && *p != ';' &&
                       *p != '|' && *p != '&' && *p != '<' && *p != '>') {
                t.text = scan_word(&p, lx->err, sizeof lx->err);
                if (!t.text)
                    return;
            }
            tok_push(lx, t);
            continue;
        }
        if (fd >= 0) {
            /* 形如 "2abc"：把数字当作普通词的开始 */
            buf_t w;
            buf_init(&w);
            char d = (char)('0' + fd);
            buf_append_byte(&w, (uint8_t)d);
            const char *q = p;
            char *rest = scan_word(&q, lx->err, sizeof lx->err);
            if (!rest) {
                buf_free(&w);
                return;
            }
            buf_append_str(&w, rest);
            free(rest);
            buf_append_byte(&w, '\0');
            tok_t t = {.type = T_WORD, .text = xstrdup((char *)w.data)};
            buf_free(&w);
            tok_push(lx, t);
            p = q;
            continue;
        }
        char *w = scan_word(&p, lx->err, sizeof lx->err);
        if (!w)
            return;
        tok_t t = {.type = T_WORD, .text = w};
        tok_push(lx, t);
    }
}

/* ---- 语法分析 ---- */

static sh_redir_t *redir_dup(const tok_t *t)
{
    sh_redir_t *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->fd = t->rd_fd;
    r->kind = t->rd_kind;
    r->dup_fd = t->rd_dup_from;
    r->target = t->text ? xstrdup(t->text) : NULL;
    return r;
}

static void pipeline_free(sh_pipeline_t *pl)
{
    if (!pl)
        return;
    for (size_t i = 0; i < pl->n; i++) {
        sh_command_t *c = pl->cmds[i];
        if (!c)
            continue;
        for (size_t j = 0; j < c->argc; j++)
            free(c->argv[j]);
        free(c->argv);
        sh_redir_t *r = c->redirs;
        while (r) {
            sh_redir_t *nx = r->next;
            free(r->target);
            free(r);
            r = nx;
        }
        free(c);
    }
    free(pl->cmds);
    free(pl);
}

void sh_program_free(sh_program_t *p)
{
    sh_stmt_t *st = p->stmts;
    while (st) {
        sh_stmt_t *nx = st->next;
        pipeline_free(st->pl);
        free(st);
        st = nx;
    }
    p->stmts = NULL;
}

/* 解析一个 pipeline；失败返回 NULL 并写 err */
static sh_pipeline_t *parse_pipeline(lex_t *lx, size_t *ip, char *err,
                                     size_t errsz)
{
    sh_pipeline_t *pl = xmalloc(sizeof *pl);
    memset(pl, 0, sizeof *pl);
    size_t cap = 0;

    for (;;) {
        sh_command_t *cmd = xmalloc(sizeof *cmd);
        memset(cmd, 0, sizeof *cmd);
        size_t acap = 0;

        while (*ip < lx->n) {
            tok_t *t = &lx->toks[*ip];
            if (t->type == T_WORD) {
                if (cmd->argc + 1 >= acap) {
                    acap = acap ? acap * 2 : 8;
                    cmd->argv = xrealloc(cmd->argv, acap * sizeof *cmd->argv);
                }
                cmd->argv[cmd->argc++] = xstrdup(t->text);
                (*ip)++;
                continue;
            }
            if (t->type == T_REDIR) {
                sh_redir_t *r = redir_dup(t);
                (*ip)++;
                if ((r->kind == SH_RD_OUT || r->kind == SH_RD_APPEND ||
                     r->kind == SH_RD_IN) &&
                    !r->target) {
                    if (*ip < lx->n && lx->toks[*ip].type == T_WORD) {
                        r->target = xstrdup(lx->toks[*ip].text);
                        (*ip)++;
                    } else {
                        snprintf(err, errsz, "语法错误：重定向缺少目标文件");
                        free(r);
                        goto fail;
                    }
                }
                r->next = cmd->redirs;
                cmd->redirs = r;
                continue;
            }
            break; /* 管道/连接符/结束 */
        }

        if (cmd->argc == 0 && !cmd->redirs) {
            snprintf(err, errsz, "语法错误：缺少命令");
            free(cmd->argv);
            free(cmd);
            goto fail;
        }
        if (cmd->argv)
            cmd->argv[cmd->argc] = NULL;

        if (pl->n + 1 >= cap) {
            cap = cap ? cap * 2 : 4;
            pl->cmds = xrealloc(pl->cmds, cap * sizeof *pl->cmds);
        }
        pl->cmds[pl->n++] = cmd;

        if (*ip < lx->n && lx->toks[*ip].type == T_PIPE) {
            (*ip)++;
            continue;
        }
        break;
    }
    return pl;
fail:
    pipeline_free(pl);
    return NULL;
}

void sh_parse(const char *line, sh_program_t *out)
{
    out->stmts = NULL;
    out->err[0] = '\0';

    lex_t lx;
    lex_line(line, &lx);
    if (lx.err[0]) {
        snprintf(out->err, sizeof out->err, "%s", lx.err);
        lex_free(&lx);
        return;
    }

    sh_stmt_t **tail = &out->stmts;
    size_t i = 0;
    int op = SH_OP_SEMI;
    while (i < lx.n) {
        if (lx.toks[i].type == T_SEMI && op == SH_OP_SEMI && !out->stmts) {
            i++; /* 允许行首分号 */
            continue;
        }
        sh_pipeline_t *pl = parse_pipeline(&lx, &i, out->err, sizeof out->err);
        if (!pl) {
            sh_program_free(out);
            lex_free(&lx);
            return;
        }
        sh_stmt_t *st = xmalloc(sizeof *st);
        st->op = op;
        st->pl = pl;
        st->next = NULL;
        *tail = st;
        tail = &st->next;

        op = SH_OP_SEMI;
        if (i < lx.n) {
            if (lx.toks[i].type == T_SEMI)
                op = SH_OP_SEMI;
            else if (lx.toks[i].type == T_AND)
                op = SH_OP_AND;
            else if (lx.toks[i].type == T_OR)
                op = SH_OP_OR;
            else {
                snprintf(out->err, sizeof out->err, "语法错误：意外的记号");
                sh_program_free(out);
                lex_free(&lx);
                return;
            }
            i++;
        }
    }
    lex_free(&lx);
}