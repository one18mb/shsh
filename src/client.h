#ifndef SH_CLIENT_H
#define SH_CLIENT_H

#include <stdbool.h>

/* 加入会话；session 不存在/名字冲突等返回 1，成功退出返回 0。
 * plain=true 时剥除 OSC 633 标记，只输出可见文本。
 * background=true 为后台端点：不读终端、不因 stdin EOF 离场（供 send 使用）。 */
int client_join(const char *session, const char *name, bool plain,
                bool background);
/* 列出会话；session 为 NULL 时列出全部 */
int client_list(const char *session);
/* 停止 (session,name) 端点：杀其 shsh 进程并清理端点文件 */
int client_logout(const char *session, const char *name);

#endif
