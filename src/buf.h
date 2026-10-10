#ifndef SH_BUF_H
#define SH_BUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_reserve(buf_t *b, size_t extra);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_append_str(buf_t *b, const char *s);
void buf_append_byte(buf_t *b, uint8_t c);
/* 丢弃头部 n 字节 */
void buf_consume(buf_t *b, size_t n);
void buf_clear(buf_t *b);

#endif
