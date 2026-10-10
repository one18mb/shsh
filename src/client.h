#ifndef SH_CLIENT_H
#define SH_CLIENT_H

/* 加入会话；session 不存在/名字冲突等返回 1，成功退出返回 0 */
int client_join(const char *session, const char *name);
/* 列出会话；session 为 NULL 时列出全部 */
int client_list(const char *session);

#endif
