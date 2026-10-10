# shsh (shared shell)

多用户共享的 shell，基于**会话（session）**模型：`shshd <session>` 启动并持有会话，其他用户通过 `shsh -s <session> -n <name>` 加入。

## 会话模型

- 会话生命周期由 `shshd <session>` 进程决定：最后一名用户退出后会话继续存活（空闲等待），Ctrl+C / SIGTERM 结束 shshd 才结束会话。
- **执行权**在会话内唯一：首位加入者获得；执行者离开/掉线（或 `exit`）后自动移交给最早加入的在线用户。
- **shshd 是系统虚拟用户**：启动即占用用户名 `shshd`（不能用该名加入；`shsh -l` 中列为 `[系统]`）；无人执行时执行权归 shshd，此时会话冻结不可输入。
- `to shshd` 合法：执行权交给 shshd，shshd 随即执行 `to`，把执行权给最早加入的另一个在线用户（若无其他用户则保持，等待新用户加入时接管）。
- **唯一的系统广播**：shshd 执行 `to` 时，在共享输出流中以命令形式出现

  ```
  shshd:<当前文件夹名>$ to <name>
  ```

  随后广播新执行者的提示符。加入/离开等事件**不广播**，避免干扰命令输出。
- 命令级共享：其他人的输入不共享，也不显示提示；只有执行者**提交后**的命令行才被共享。

## PS1

```
<name>:\W$
```

- `name`：当前执行者（为 shshd 时显示 `shshd`）
- `\W`：当前文件夹名

## 终端语义

| 角色/状态 | 终端模式 | 行为 |
|---|---|---|
| 执行者（空闲） | 原始模式 + 客户端自绘 | 客户端行编辑（退格 / ←→ / Ctrl+A/E/U / Tab 补全 / Ctrl+C·D 离开）；提交后命令行才回显给其他人 |
| 执行者（命令运行中） | 原始模式 | 按键直通 pty，由行规程回显给所有人；Ctrl+C → SIGINT 前台命令；程序可自行关回显（如密码输入） |
| 无执行权用户 | 规范模式、**关回显** | 输入被完全丢弃、无任何提示；仅 Ctrl+C / Ctrl+D 退出会话 |

- stdout/stderr 共享：pty 输出广播给所有人（含执行者本人），命令只出现一次（命令行回显由 shsh 负责，命令不经 tty 注入）。
- 加入/离开不打印任何提示：提示符 `<执行者>:\W$` 本身即表明当前执行者；非执行者输入一律直接丢弃，且角色切换时清空终端输入队列，旧输入不会被当成命令行执行。
- **Tab 补全**（行为对齐 bash，实现自研；bash 的补全本体约 5–6k 行且耦合 readline 与可编程补全，不搬）：
  - 候选来自会话 cwd / PATH / 环境变量名，**应答只发给请求者**，清单不出现在其他用户输出里
  - 唯一候选追加空格；目录补到 `dir/` 停下，再按 Tab **逐级下探**；软链接指向目录按目录处理
  - 插入前按引号上下文转义（空格、`$`、引号等），**共同前缀在转义后计算**
  - 词边界对齐 bash 默认集合（空白与 `" ' > < = ; | & ( :`）；隐藏文件仅在前缀以 `.` 开头时补
  - 清单只显示**待选条目名**（不显示完整路径，便于逐级下探），按执行者终端宽度做**列对齐**（列优先）
  - 候选超过 200 个不再展开，只提示「候选过多」；**补全是同步计算**，另有令牌桶限速（突发 20、20/s）
    与客户端串行化，防连按 Tab 拖住会话
- **不支持 TUI（永久）**：shsh 不承诺全屏程序（vi/top 等）的显示，输出是线性共享流；pty 窗口尺寸跟随执行者。
- 命令同步阻塞执行：一条命令结束（SIGCHLD）后才回到空闲态。

## 使用

```bash
shshd <session>                # 启动会话（前台守护，Ctrl+C 结束会话）
shsh -l                        # 列出所有活动会话
shsh -l -s <session>           # 列出指定会话的用户
shsh -s <session> -n <name>    # 加入共享会话
shsh -h                        # 帮助
```

```
to <name>     # 转交执行权给 <name>
to shshd      # 交还 shshd，由 shshd 再转给其他在线用户
exit          # 执行者离开会话（自动移交执行权）
```

`shsh -l` 输出示例：

```
work (执行者: alice)
  shshd                [系统]
  alice                [执行者]
  bob
```

## 退出

- 执行者：`exit`、Ctrl+C 或 Ctrl+D 离开会话（触发自动移交）。
- 无执行权用户：Ctrl+C / Ctrl+D 退出会话。
- `shshd` 被 Ctrl+C / SIGTERM：广播会话结束，所有客户端退出。

## 开发

- C11，Linux（epoll + PTY + AF_UNIX），要求高性能。
- **执行引擎 = 自研最小 shell**（不依赖任何现有 shell，也不向 bash 看齐）：

  ```
  shshd ─┬─ PTY master ──▸ 每条外部命令：fork + setsid + TIOCSCTTY + chdir(会话 cwd) + execvp
         │                   └─ 命令的 stdin/out/err 都是 PTY，交互程序照常可用
         ├─ 会话状态在服务端：cwd / 变量 / $?
         ├─ 内建：cd、pwd、echo、export、unset、exit、to、赋值 NAME=value
         ├─ 语法：空白分词、单/双引号、反斜杠转义；`;` `&&` `||` `|`；`>` `>>` `<` `2>` `2>>` `2>&1`
         └─ 展开：$VAR / ${VAR} / $?，在“执行到该语句”时才展开（同行内先赋值后引用、&&/|| 的 $? 都正确）
  ```

  一行命令被解析为「语句链 → pipeline → 命令」：每条 pipeline 的进程组统一，
  Ctrl+C 与掉线挂断由服务端 `kill(-pgid)` 一次打全组（子进程不改变会话，
  pty 不产生信号）；内建命令在服务端执行，重定向用 fd 保存/恢复实现。

  fork 之后 pid 直接可用、SIGCHLD 即命令结束，因此没有哨兵解析、回显相位、
  跨会话进程组查询这类间接机制；命令行不经 tty 注入，运行期按键回显由行规程
  提供（程序可用 `stty -echo` 自行关闭，密码不外泄）。
- **行为对齐方式**：以 dash（ash 的现代维护版）作为行为规范做差分测试——
  同一命令分别喂给 `dash -c` 与本实现，逐条比较输出（见 `test_dash_parity_lexer`
  与 `test_shell_control_ops` / `test_shell_pipeline`）。
  只对齐行为，不复制其代码（dash 为 BSD 系许可），也不采用其架构：
  ash 的复杂度主要在 if/while/for/case/函数/参数替换全套，这些 shsh 不做
  （需要时用 `sh -c '...'` 兜底）。
- 里程碑：
  - **M0 会话骨架 ✅**：双入口、CLI、socket 生命周期、协议、同名冲突、陈旧 socket 重建
  - **M1 共享终端 ✅**：PTY、输出广播、PS1、规范/原始模式切换、Ctrl+C/D 语义、掉线自动移交、shshd 虚拟用户与 `to` 广播
  - **M2 自研最小 shell ✅**：词法（引号/转义/分词）、内建 cd/pwd/echo/export/unset/exit/to、外部命令 fork/exec/wait、会话状态（cwd/变量/`$?`）、相位协议
  - **M3.1 控制符/重定向/管道 ✅**：`;` `&&` `||`、`>` `>>` `<` `2>` `2>&1`、多段管道（含内建命令入管道）
  - **M3.2 规划**：通配符、`$(...)`、前缀赋值（`X=1 cmd`）、`if/for/while`（如需）
  - **M4 健壮性**：大输出压测、通知排队、错误路径、sanitizer 全绿
  - **M5.1 行编辑与补全 ✅**：客户端 raw 模式本地编辑（退格/←→/Ctrl+A·E·U/Enter）+ Tab 补全（命令名、路径、`$变量`，清单仅执行者可见）
  - **M5.2 规划**：历史（↑↓）、Ctrl+W/Ctrl+K、宽字符/超宽行的光标处理、可编程补全（如需）
- 构建与测试：

```bash
make            # 发布版（-O2，零警告）
make debug      # ASAN/UBSAN 版
make test       # 自动化测试（36 个用例，含真实 pty 场景与 dash 行为对照）
```

## 协议

AF_UNIX，帧 = u32 BE payload 长度 + u16 BE 类型 + payload（上限 1 MiB）。

- C→S：`HELLO{name,rows,cols}`、`LINE`、`RAW`、`COMPLETE{line,cursor}`、`WINCH`、`QUIT`、`LIST`
- S→C：`WELCOME{self,exec,exec_name}`、`OUT`、`CMD_ECHO`、`RUN_BEGIN`、`RUN_END{status}`、`ROLE{exec}`、`COMPLETE{prompt,from,text,listing}`、`NOTICE`、`LISTRESP`、`BYE{reason}`

每条提交的命令都由 `RUN_BEGIN`…`RUN_END` 括起（含内建命令），客户端据此做相位门控与终端模式切换。

## 已知限制

- 暂不支持：通配符、`$(...)`、前缀赋值（`X=1 cmd`）、`&`/`jobs`/`fg` 作业控制、`if/for/while` 等复合语句。需要时可用 `sh -c '...'`。
- 会话变量为会话级（`X=1` 与 `export` 都对后续命令与子进程可见），不做局部作用域。
- socket 位于 `/tmp/shsh/<session>.sock`（0666），无对端鉴权，定位为同机互信工具。
- 命令运行期间执行者掉线：其前台命令被 SIGHUP 挂断。
- 通知可能打断正在输入的命令行（后续做排队优化）。