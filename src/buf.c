#include "buf.h"
#include "common.h"

void buf_init(buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf_t *b)
{
    free(b->data);
    buf_init(b);
}

void buf_reserve(buf_t *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(buf_t *b, const void *p, size_t n)
{
    if (!n)
        return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_append_str(buf_t *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

void buf_append_byte(buf_t *b, uint8_t c)
{
    buf_append(b, &c, 1);
}

void buf_consume(buf_t *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void buf_clear(buf_t *b)
{
    b->len = 0;
}
