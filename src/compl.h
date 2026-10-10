#ifndef SH_COMPL_H
#define SH_COMPL_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>

/* 补全结果 */
/* 候选数量上限：超过则不再展开，只提示（避免同步展开巨量目标拖垮会话） */
#define SH_COMPL_MAX 200

typedef struct {
    char **items;   /* 插入文本（含目录前缀、已转义），已排序去重 */
    char **display; /* 展示文本（仅待选条目名，已转义） */
    size_t n;
    bool overflow; /* 候选超过上限 */
} compl_t;

void compl_free(compl_t *c);

/* 计算补全：
 *   line/cursor  当前输入行与光标位置（byte offset）
 *   cwd          会话工作目录（服务端状态）
 *   cols         执行者终端宽度（用于候选清单的列对齐；<=0 用 80）
 * 输出：
 *   out         候选列表（已排序）
 *   word_start  被替换区间起点（line 中的 byte offset）
 *   replace     替换文本（唯一候选时含补齐的 / 或空格；否则为可能延长的共同前缀）
 *   listing     需要展示给执行者的候选清单（无需展示时为空）
 * 说明：不做字段分割，只按空白与 ; | & < > 分界；引号内的空格会被当作分界（已知简化）。 */
void compl_compute(const char *line, size_t cursor, const char *cwd, int cols,
                   compl_t *out, size_t *word_start, buf_t *replace,
                   buf_t *listing);

#endif