#include "proto.h"
#include "common.h"

#include <arpa/inet.h>

/* ---- 序列化 ----
 * 字符串 = u8 长度 + 字节（长度上限 255） */

static void put_u8(buf_t *b, uint8_t v)
{
    buf_append(b, &v, 1);
}

static void put_u16(buf_t *b, uint16_t v)
{
    uint16_t n = htons(v);
    buf_append(b, &n, 2);
}

static void put_u32(buf_t *b, uint32_t v)
{
    uint32_t n = htonl(v);
    buf_append(b, &n, 4);
}

static void put_str(buf_t *b, const char *s)
{
    size_t l = strlen(s);
    put_u8(b, (uint8_t)l);
    buf_append(b, s, l);
}

/* ---- 反序列化 ----
 * 全部带边界检查，失败返回 -1 */

static int get_u8(const uint8_t *p, size_t n, size_t *off, uint8_t *v)
{
    if (*off + 1 > n)
        return -1;
    *v = p[(*off)++];
    return 0;
}

static int get_u16(const uint8_t *p, size_t n, size_t *off, uint16_t *v)
{
    if (*off + 2 > n)
        return -1;
    uint16_t t;
    memcpy(&t, p + *off, 2);
    *off += 2;
    *v = ntohs(t);
    return 0;
}

static int get_u32(const uint8_t *p, size_t n, size_t *off, uint32_t *v)
{
    if (*off + 4 > n)
        return -1;
    uint32_t t;
    memcpy(&t, p + *off, 4);
    *off += 4;
    *v = ntohl(t);
    return 0;
}

static int get_str(const uint8_t *p, size_t n, size_t *off,
                   char *out, size_t outsz)
{
    uint8_t l;
    if (get_u8(p, n, off, &l))
        return -1;
    if (*off + l > n)
        return -1;
    if ((size_t)l >= outsz)
        return -1;
    memcpy(out, p + *off, l);
    out[l] = '\0';
    *off += l;
    return 0;
}

int msg_encode(const msg_t *m, buf_t *out)
{
    if (m->payload.len > SH_MSG_MAX)
        return -1;
    put_u32(out, (uint32_t)m->payload.len);
    put_u16(out, m->type);
    buf_append(out, m->payload.data, m->payload.len);
    return 0;
}

int msg_try_extract(buf_t *in, msg_t *m)
{
    if (in->len < 6)
        return 0;
    uint32_t len;
    uint16_t typ;
    memcpy(&len, in->data, 4);
    memcpy(&typ, in->data + 4, 2);
    len = ntohl(len);
    typ = ntohs(typ);
    if (len > SH_MSG_MAX)
        return -1;
    if (in->len < (size_t)6 + len)
        return 0;
    m->type = typ;
    buf_init(&m->payload);
    buf_append(&m->payload, in->data + 6, len);
    buf_consume(in, 6 + len);
    return 1;
}

void msg_make_hello(msg_t *m, const char *name, uint16_t rows, uint16_t cols)
{
    buf_init(&m->payload);
    put_str(&m->payload, name);
    put_u16(&m->payload, rows);
    put_u16(&m->payload, cols);
    m->type = C2S_HELLO;
}

void msg_make_list(msg_t *m)
{
    buf_init(&m->payload);
    m->type = C2S_LIST;
}

void msg_make_welcome(msg_t *m, uint32_t self_id, uint32_t exec_id,
                      const char *exec_name,
                      const uint8_t *replay, size_t replay_len)
{
    buf_init(&m->payload);
    put_u32(&m->payload, self_id);
    put_u32(&m->payload, exec_id);
    put_str(&m->payload, exec_name);
    put_u32(&m->payload, (uint32_t)replay_len);
    if (replay_len)
        buf_append(&m->payload, replay, replay_len);
    m->type = S2C_WELCOME;
}

void msg_make_notice(msg_t *m, const char *text)
{
    buf_init(&m->payload);
    put_str(&m->payload, text);
    m->type = S2C_NOTICE;
}

void msg_make_bye(msg_t *m, const char *reason)
{
    buf_init(&m->payload);
    put_str(&m->payload, reason);
    m->type = S2C_BYE;
}

void msg_make_listresp(msg_t *m, const char *session,
                       const user_info_t *users, size_t n)
{
    buf_init(&m->payload);
    put_str(&m->payload, session);
    put_u16(&m->payload, (uint16_t)n);
    for (size_t i = 0; i < n; i++) {
        put_u32(&m->payload, users[i].id);
        put_u8(&m->payload, users[i].is_exec ? 1 : 0);
        put_str(&m->payload, users[i].name);
    }
    m->type = S2C_LISTRESP;
}

int msg_parse_hello(const msg_t *m, char *name, size_t namesz,
                    uint16_t *rows, uint16_t *cols)
{
    size_t off = 0;
    const uint8_t *p = m->payload.data;
    size_t pl = m->payload.len;
    if (get_str(p, pl, &off, name, namesz))
        return -1;
    if (get_u16(p, pl, &off, rows))
        return -1;
    if (get_u16(p, pl, &off, cols))
        return -1;
    return 0;
}

int msg_parse_welcome(const msg_t *m, uint32_t *self_id, uint32_t *exec_id,
                      char *exec_name, size_t execsz)
{
    size_t off = 0;
    const uint8_t *p = m->payload.data;
    size_t pl = m->payload.len;
    if (get_u32(p, pl, &off, self_id))
        return -1;
    if (get_u32(p, pl, &off, exec_id))
        return -1;
    if (get_str(p, pl, &off, exec_name, execsz))
        return -1;
    return 0; /* 尾部 replay 忽略 */
}

int msg_parse_text(const msg_t *m, char *text, size_t textsz)
{
    size_t off = 0;
    return get_str(m->payload.data, m->payload.len, &off, text, textsz);
}

int msg_parse_listresp(const msg_t *m, char *session, size_t ssz,
                       user_info_t *users, size_t cap, size_t *n)
{
    size_t off = 0;
    const uint8_t *p = m->payload.data;
    size_t pl = m->payload.len;
    if (get_str(p, pl, &off, session, ssz))
        return -1;
    uint16_t nu;
    if (get_u16(p, pl, &off, &nu))
        return -1;
    if (nu > cap)
        return -1;
    for (size_t i = 0; i < nu; i++) {
        uint8_t flag;
        if (get_u32(p, pl, &off, &users[i].id))
            return -1;
        if (get_u8(p, pl, &off, &flag))
            return -1;
        if (get_str(p, pl, &off, users[i].name, sizeof users[i].name))
            return -1;
        users[i].is_exec = flag != 0;
    }
    *n = nu;
    return 0;
}

/* ---- M1：终端共享 ---- */

static void make_bytes_msg(msg_t *m, uint16_t type, const void *data, size_t len)
{
    buf_init(&m->payload);
    buf_append(&m->payload, data, len);
    m->type = type;
}

void msg_make_out(msg_t *m, const void *data, size_t len)
{
    make_bytes_msg(m, S2C_OUT, data, len);
}

void msg_make_run_begin(msg_t *m)
{
    make_bytes_msg(m, S2C_RUN_BEGIN, NULL, 0);
}

void msg_make_run_end(msg_t *m, int status)
{
    buf_init(&m->payload);
    put_u32(&m->payload, (uint32_t)status);
    m->type = S2C_RUN_END;
}

void msg_make_role(msg_t *m, uint32_t exec_id)
{
    buf_init(&m->payload);
    put_u32(&m->payload, exec_id);
    m->type = S2C_ROLE;
}

void msg_make_raw(msg_t *m, const void *data, size_t len)
{
    make_bytes_msg(m, C2S_RAW, data, len);
}

void msg_make_winch(msg_t *m, uint16_t rows, uint16_t cols)
{
    buf_init(&m->payload);
    put_u16(&m->payload, rows);
    put_u16(&m->payload, cols);
    m->type = C2S_WINCH;
}

int msg_parse_bytes(const msg_t *m, const uint8_t **data, size_t *len)
{
    *data = m->payload.data;
    *len = m->payload.len;
    return 0;
}

int msg_parse_run_end(const msg_t *m, int *status)
{
    size_t off = 0;
    uint32_t v;
    if (get_u32(m->payload.data, m->payload.len, &off, &v))
        return -1;
    *status = (int)v;
    return 0;
}

int msg_parse_role(const msg_t *m, uint32_t *exec_id)
{
    size_t off = 0;
    return get_u32(m->payload.data, m->payload.len, &off, exec_id);
}

int msg_parse_winch(const msg_t *m, uint16_t *rows, uint16_t *cols)
{
    size_t off = 0;
    if (get_u16(m->payload.data, m->payload.len, &off, rows))
        return -1;
    return get_u16(m->payload.data, m->payload.len, &off, cols);
}

/* ---- 大块数据（供补全等消息使用） ---- */

void proto_put_u32(buf_t *b, uint32_t v)
{
    uint32_t n = htonl(v);
    buf_append(b, &n, 4);
}

void proto_put_blob(buf_t *b, const void *p, size_t n)
{
    proto_put_u32(b, (uint32_t)n);
    buf_append(b, p, n);
}

int proto_get_u32(const uint8_t *p, size_t n, size_t *off, uint32_t *v)
{
    if (*off + 4 > n)
        return -1;
    uint32_t t;
    memcpy(&t, p + *off, 4);
    *off += 4;
    *v = ntohl(t);
    return 0;
}

int proto_get_blob(const uint8_t *p, size_t n, size_t *off,
                   const uint8_t **out, size_t *outlen)
{
    uint32_t l;
    if (proto_get_u32(p, n, off, &l))
        return -1;
    if (*off + l > n)
        return -1;
    *out = p + *off;
    *outlen = l;
    *off += l;
    return 0;
}
