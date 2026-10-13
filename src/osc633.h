#ifndef SH_OSC633_H
#define SH_OSC633_H

#include <stddef.h>
#include <stdint.h>

/* 带内 OSC 633 扫描器：把字节流切成「可见字节」与「完整 OSC 序列」。
 * 跨调用的半截序列留在扫描器内部，下次喂入拼齐；超长滞留放弃滞留原样吐出。
 * 纯切分，不含角色/相位策略——客户端（client.c）与 agent 通道（send.c）共用。 */

#define OSC633_HOLD_MAX 1024

typedef struct osc633_scan osc633_scan_t;

typedef void (*osc633_raw_fn)(void *ctx, const uint8_t *p, size_t n);

/* 每遇完整 OSC 调一次：is633=true 时 code 为标记首字符（A/C/D/E/W...），
 * args 指向 code 之后的参数（如 "s=0;u=a;w=w"，已去掉一个起始分号）；
 * is633=false 表示其它 OSC（如窗口标题），code=0。raw 始终为含 ESC] 的整段。 */
typedef void (*osc633_mark_fn)(void *ctx, int is633, char code,
                               const uint8_t *args, size_t args_len,
                               const uint8_t *raw, size_t raw_len);

struct osc633_scan {
    uint8_t hold[OSC633_HOLD_MAX];
    size_t hold_len;
};

void osc633_scan_init(osc633_scan_t *s);
void osc633_scan_feed(osc633_scan_t *s, const uint8_t *p, size_t n,
                      void *ctx, osc633_raw_fn on_raw,
                      osc633_mark_fn on_mark);

#endif
