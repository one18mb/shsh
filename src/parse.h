#ifndef SH_PARSE_H
#define SH_PARSE_H

#include <stddef.h>

/* 变量查询回调：返回变量值，未定义返回 NULL。ctx 为调用方上下文。 */
typedef const char *(*sh_lookup_fn)(void *ctx, const char *name);

/* 语句连接符 */
enum { SH_OP_SEMI = 0, SH_OP_AND = 1, SH_OP_OR = 2 };

/* 重定向类型 */
enum { SH_RD_OUT = 0, SH_RD_APPEND = 1, SH_RD_IN = 2, SH_RD_DUP = 3 };

typedef struct sh_redir {
    int fd;       /* 被重定向的 fd（0/1/2） */
    int kind;     /* SH_RD_* */
    char *target; /* 文件名（OUT/APPEND/IN） */
    int dup_fd;   /* DUP 的源 fd */
    struct sh_redir *next;
} sh_redir_t;

typedef struct sh_command {
    char **argv; /* NULL 结尾；元素是「模板」，执行前用 sh_expand_word 展开 */
    size_t argc;
    int expanded; /* 是否已展开（执行前只展开一次） */
    sh_redir_t *redirs;
} sh_command_t;

typedef struct sh_pipeline {
    sh_command_t **cmds;
    size_t n;
} sh_pipeline_t;

typedef struct sh_stmt {
    int op; /* 与前一条语句的连接符：SH_OP_* */
    sh_pipeline_t *pl;
    struct sh_stmt *next;
} sh_stmt_t;

typedef struct {
    sh_stmt_t *stmts; /* NULL = 无有效命令（空白行） */
    char err[160];    /* 非空表示解析失败 */
} sh_program_t;

/* 解析一行：
 *   list     := pipeline ((';' | '&&' | '||') pipeline)*
 *   pipeline := command ('|' command)*
 *   command  := (word | redirection)*
 * 词法：空白分词、单/双引号、反斜杠转义、$VAR/${VAR}/$? 展开（不做字段分割）；
 * 重定向：> >> < 2> 2>> 2>&1（N 与操作符必须紧邻）。 */
void sh_parse(const char *line, sh_program_t *out);

/* 展开一个词模板（执行时调用）：引号/转义 + $VAR/${VAR}/$? */
char *sh_expand_word(const char *tpl, sh_lookup_fn lookup, void *ctx);
void sh_program_free(sh_program_t *p);

#endif