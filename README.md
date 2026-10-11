# shsh (shared shell)

多用户共享的 shell：`shshd <session>` 启动一个会话，其他用户用
`shsh -s <session> -n <name>` 加入，所有人看到同一个终端、同一条命令输出，
但同一时刻只有一个人能输入。

shsh 解决的问题是「**共享**」——会话、执行权、广播、终端角色都围绕多用户
设计；命令本身由自带的 shell 执行层处理，语义与普通 shell 一致。

## 核心概念

以下七条是 shsh 独有的特性集合：没有它们就不是 shsh。

### 1. 命名会话与生命周期

- 会话由 `shshd <session>` 进程持有，socket 位于 `/tmp/shsh/<session>.sock`。
- 最后一名用户退出后会话继续存活（空闲等待新人加入）；只有 shshd 收到
  Ctrl+C / SIGTERM 才结束会话，并向所有在线客户端广播 BYE。
- 同名会话拒绝重复启动；残留的陈旧 socket 会被自动重建。

### 2. 唯一执行权

- 同一时刻只有一名用户能输入命令。首位加入者获得执行权。
- 执行者离开、掉线或输入 `exit` 时，执行权自动移交给**最早加入**的在线用户。
- `to <name>` 显式转交；`to shshd` 交还给系统，shshd 随即再转交给最早加入的
  另一个用户（没有其他用户则保持，等待新人接管）。
- **shshd 是系统虚拟用户**：启动即占用用户名 `shshd`（不能用该名加入，
  `shsh -l` 中列为 `[系统]`）。无人执行时执行权归 shshd，会话冻结不可输入。

### 3. 单一共享输出流

- 会话只有一个 PTY，所有命令的 stdout/stderr 广播给所有人，包括执行者本人。
- **命令级共享**：其他人的输入过程不可见；只有执行者提交后的命令行被回显
  （CMD_ECHO），命令只在共享流中出现一次。
- 加入、离开、执行权切换本身**不打印任何提示**——提示符上的名字即表明
  当前执行者；唯一的系统广播是 shshd 执行 `to` 时共享流中出现的命令行：

  ```
  shshd:<当前文件夹名>$ to <name>
  ```

### 4. 终端角色三态

| 角色 / 状态 | 终端模式 | 行为 |
|---|---|---|
| 执行者（空闲） | 原始模式，客户端自绘 | 本地行编辑（退格 / ←→ / Ctrl+A·E·U / Ctrl+C·D 离开）；提交前输入不共享 |
| 执行者（命令运行中） | 原始模式 | 按键直通 PTY，由行规程回显给所有人；Ctrl+C 发 SIGINT 中断前台命令；程序可自行关闭回显（如密码输入） |
| 无执行权用户 | 规范模式，关闭回显 | 输入被完全丢弃、无任何提示；仅 Ctrl+C / Ctrl+D 退出会话 |

角色切换时清空终端输入队列，旧输入不会被当成新角色的命令执行。

### 5. 相位协议

每条提交的命令（含内建命令）都由 `RUN_BEGIN` … `RUN_END{status}` 括起，
客户端据此切换终端模式、门控输入；执行权变化由 `ROLE` 推送，会话结束由
`BYE` 通知。无状态的客户端靠这套相位帧呈现出一致的共享终端体验。

### 6. 会话级共享状态

会话的工作目录、环境变量、`$?` 由 shshd 统一持有，对所有用户和后续命令
可见：任何一个人 `cd`、`export`，所有人共享同一份状态，提示符里的目录名
随之变化。

### 7. 异常治理

- 命令运行期间执行者掉线：其前台进程组被 SIGHUP 一次挂断（`kill(-pgid)`），
  不会留下脱离会话的孤儿进程。
- 执行者在管道/命令运行中按 Ctrl+C：信号送达整条 pipeline 的统一进程组。
- socket 无对端鉴权（0666），定位为**同机互信工具**。

## 提示符

```
<name>:\W$
```

- `name`：当前执行者（系统持有时显示 `shshd`）
- `\W`：会话当前工作目录的文件夹名

## 使用

```bash
shshd <session>                # 启动会话（前台运行，Ctrl+C 结束整个会话）
shsh -l                        # 列出所有活动会话
shsh -l -s <session>           # 列出指定会话的在线用户
shsh -s <session> -n <name>    # 加入共享会话
shsh -h                        # 帮助
```

会话内：

```
to <name>     # 转交执行权
to shshd      # 交还系统（自动再转给最早加入的用户）
exit          # 离开会话（执行者离开会触发自动移交）
```

`shsh -l` 输出示例：

```
work (执行者: alice)
  shshd                [系统]
  alice                [执行者]
  bob
```

## Shell 语义

除会话控制命令（`to` / `exit`）外，输入行由 shshd 自带的 shell 执行层处理，
行为是普通 shell 的子集，不与共享模型冲突：

- 词法：空白分词、单/双引号、反斜杠转义
- 展开：`$VAR`、`${VAR}`、`$?`（执行到该语句时展开，同行先赋值后引用、
  `&&`/`||` 的 `$?` 均按普通 shell 语义）
- 组合：`;`、`&&`、`||`、管道 `|`
- 重定向：`>` `>>` `<` `2>` `2>>` `2>&1`
- 内建：`cd` `pwd` `echo` `export` `unset` `help`，以及 `NAME=value` 赋值；
  内建可参与管道与重定向
- 外部命令：fork 后在会话工作目录中 `execvp`，stdin/out/err 都是会话 PTY

当前未实现的普通 shell 语法（通配符、`$(...)`、前缀赋值、`&`/作业控制、
`if`/`for`/`while` 等复合语句）可随时用 `sh -c '...'` 兜底。

## 协议

AF_UNIX 字节流，帧 = u32 BE payload 长度 + u16 BE 类型 + payload（上限
1 MiB）。

- C→S：`HELLO{name,rows,cols}`、`LINE`、`RAW`、`WINCH`、`QUIT`、`LIST`
- S→C：`WELCOME{self,exec,exec_name}`、`OUT`、`CMD_ECHO`、`RUN_BEGIN`、
  `RUN_END{status}`、`ROLE{exec}`、`NOTICE`、`LISTRESP`、`BYE{reason}`

## 边界

- 输出是**线性共享流**，不承诺全屏 TUI 程序（vi/top 等）的显示效果；
  PTY 窗口尺寸跟随当前执行者。
- 无历史记录（↑↓）、无 Tab 补全、无宽字符光标处理：空闲态行编辑仅提供
  退格、←→、Ctrl+A/E/U。
- socket 无鉴权，只适合同机互信环境。
- 通知/提示符可能打断正在输入的命令行。

## 开发

C11，仅依赖 Linux（epoll + PTY + AF_UNIX），两个二进制：

- `bin/shshd`（[src/](src/) 中的服务端）：
  - 会话层（shsh 独有）：[server.c](src/server.c) 负责 socket、客户端表、
    执行权、广播、相位与终端语义；[proto.c](src/proto.c) 为帧编解码
  - shell 执行层（普通 shell 语义）：[parse.c](src/parse.c) 词法/展开，
    [exec.c](src/exec.c) 进程组/重定向/fork-exec
- `bin/shsh`（客户端）：[client.c](src/client.c) 实现三态终端与本地行编辑

```bash
make            # 发布版（-O2，零警告）
make debug      # ASAN/UBSAN 版
make test       # 自动化测试（27 个用例，含真实 PTY 场景）
```
