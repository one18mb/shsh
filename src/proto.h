#ifndef SH_PROTO_H
#define SH_PROTO_H

#include "buf.h"
#include "common.h"

#include <stdbool.h>
#include <stdint.h>

/* 消息类型 */
enum {
    /* C -> S */
    C2S_HELLO = 1,
    C2S_LINE,
    C2S_RAW,
    C2S_WINCH,
    C2S_QUIT,
    C2S_LIST,
    /* S -> C */
    S2C_WELCOME = 64,
    S2C_OUT,
    S2C_CMD_ECHO,
    S2C_RUN_BEGIN,
    S2C_RUN_END,
    S2C_ROLE,
    S2C_NOTICE,
    S2C_LISTRESP,
    S2C_BYE,
};

typedef struct {
    uint16_t type;
    buf_t payload;
} msg_t;

/* 帧 = u32 BE payload 长度 + u16 BE 类型 + payload */
int msg_encode(const msg_t *m, buf_t *out); /* 追加到 out */
/* 从输入缓冲提取一个完整帧：1=成功, 0=数据不足, -1=协议错误 */
int msg_try_extract(buf_t *in, msg_t *m);

void msg_make_hello(msg_t *m, const char *name, uint16_t rows, uint16_t cols);
void msg_make_quit(msg_t *m);
void msg_make_list(msg_t *m);
void msg_make_welcome(msg_t *m, uint32_t self_id, uint32_t exec_id,
                      const char *exec_name,
                      const uint8_t *replay, size_t replay_len);
void msg_make_notice(msg_t *m, const char *text);
void msg_make_bye(msg_t *m, const char *reason);

typedef struct {
    uint32_t id;
    char name[SH_NAME_MAX + 1];
    bool is_exec;
} user_info_t;

void msg_make_listresp(msg_t *m, const char *session,
                       const user_info_t *users, size_t n);

/* 大块数据（u32 长度前缀）读写：命令行与候选清单可能超过 255 字节 */
void proto_put_u32(buf_t *b, uint32_t v);
void proto_put_blob(buf_t *b, const void *p, size_t n);
int proto_get_u32(const uint8_t *p, size_t n, size_t *off, uint32_t *v);
int proto_get_blob(const uint8_t *p, size_t n, size_t *off,
                   const uint8_t **out, size_t *outlen);

/* 解析。成功返回 0，失败返回 -1。 */
int msg_parse_hello(const msg_t *m, char *name, size_t namesz,
                    uint16_t *rows, uint16_t *cols);
int msg_parse_welcome(const msg_t *m, uint32_t *self_id, uint32_t *exec_id,
                      char *exec_name, size_t execsz);
int msg_parse_text(const msg_t *m, char *text, size_t textsz);
int msg_parse_listresp(const msg_t *m, char *session, size_t ssz,
                       user_info_t *users, size_t cap, size_t *n);

/* M1：终端共享 */
void msg_make_out(msg_t *m, const void *data, size_t len);
void msg_make_cmd_echo(msg_t *m, const void *data, size_t len);
void msg_make_run_begin(msg_t *m);
void msg_make_run_end(msg_t *m, int status);
void msg_make_role(msg_t *m, uint32_t exec_id);
void msg_make_line(msg_t *m, const void *data, size_t len);
void msg_make_raw(msg_t *m, const void *data, size_t len);
void msg_make_winch(msg_t *m, uint16_t rows, uint16_t cols);

/* payload 直接作为字节串返回（不拷贝） */
int msg_parse_bytes(const msg_t *m, const uint8_t **data, size_t *len);
int msg_parse_run_end(const msg_t *m, int *status);
int msg_parse_role(const msg_t *m, uint32_t *exec_id);
int msg_parse_winch(const msg_t *m, uint16_t *rows, uint16_t *cols);

#endif
