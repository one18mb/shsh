# shsh (shared shell)

多用户共享的 shell：`shshd [session]` 启动一个会话（省略会话名时为默认会话
`session`），其他用户用 `shsh <name> [session]` 加入，所有人看到同一个终端、
同一条命令输出，但同一时刻只有一个人能输入。

shsh 解决的问题是「**共享**」——会话、执行权、广播、终端角色都围绕多用户
设计；命令本身由一个普通 shell 执行层处理，语义与普通 shell 一致。

## 构建与安装

```bash
make                  # 产物在 bin/{shsh,shshd,send}
make test             # 运行测试（可选）

make install          # 安装到 /usr/local/bin（需相应权限）
make uninstall        # 卸载
```

安装位置可用标准变量调整：

```bash
make install PREFIX=$HOME/.local                     # 用户级安装
make install DESTDIR=/tmp/stage PREFIX=/usr          # 打包/暂存到指定根
make uninstall PREFIX=$HOME/.local
```

其它目标：`make debug`（无优化带调试信息）、`make asan` / `make test-asan`
（AddressSanitizer/UBSan 构建与测试，产物在独立的 `bin-asan/`）、
`make clean`。

发布预编译二进制可用 `make bindist`，生成 `dist/shsh-<version>-linux-x86_64.tar.gz`
（内含 `bin/` 与全部许可材料，解压即用；版本号取 git 标签，可用
`make bindist VERSION=x.y.z` 覆盖）。

## 快速开始

```bash
# 终端 A：启动会话（省略会话名时为默认 "session"；后台守护运行）
shshd

# 终端 B：alice 加入
shsh login -n alice

# 终端 C：bob 加入（只看不能输入，执行权在 alice）
shsh login -n bob
```

## 使用

```bash
shshd [-s 会话名]                         # 启动会话（省略时默认 "session"；后台守护，kill 结束整个会话）
shsh -l [-s 会话名]                       # 列出所有活动会话，或指定会话的在线用户
shsh login -n 用户名 [-s 会话名] [--plain] [-d]
                                          # 加入会话；-d 为后台端点（供 send 使用）
shsh logout -n 用户名 [-s 会话名]          # 停止该用户的端点并清理文件
shsh -h                                   # 帮助
```

会话内：

```
to <name>     # 转交执行权
to shshd      # 交还系统（自动再转给最早加入的用户）
exit          # 离开会话（挂断当前前台作业；同 Ctrl+D）
```

`shsh -l` 输出示例：

```
session1
活动用户:
alice
所有用户:
shshd alice bob

session2
活动用户:
alice
所有用户:
shshd alice bob
```

- **活动用户**：当前持有执行权的用户（无人加入时为 `shshd`）。
- **所有用户**：shshd 系统用户 + 该会话内全部在线用户。
- 多个会话之间空一行。

## 核心概念

以下七条是 shsh 独有的特性集合：没有它们就不是 shsh。

### 1. 命名会话与生命周期

- 会话由 `shshd [session]` 进程持有（省略会话名时为默认会话 `session`），
  socket 位于 `/tmp/shsh/<session>.sock`。
- 最后一名用户退出后会话继续存活（空闲等待新人加入）；只有 shshd 收到
  SIGTERM（`kill`）才结束会话：向共享流写入一枚 OSC 633 `E` 标记后关闭，
  客户端读到 EOF。
  shshd 是**会话首领**（自建 PTY 即其控制终端）；从交互 shell 启动时会
  自动 fork 脱离、作为后台守护运行。
- 同名会话拒绝重复启动；残留的陈旧 socket 会被自动重建。

### 2. 唯一执行权

- 同一时刻只有一名用户能输入命令。首位加入者获得执行权。
- 执行者离开或掉线时，执行权自动移交给**最早加入**的在线用户。
- `to <name>` 显式转交；`to shshd` 交还给系统，shshd 随即再转交给最早加入的
  另一个用户（没有其他用户则保持，等待新人接管）。
- **shshd 是系统虚拟用户**：启动即占用用户名 `shshd`（不能用该名加入），
  在 `shsh -l` 的用户列表中始终列在首位。无人执行时执行权归 shshd，会话
  冻结不可输入。

### 3. 单一共享输出流

- 会话只有一个 PTY，终端上出现的一切——提示符、输入回显、命令输出、
  程序全屏界面——都广播给所有人。
- **输入过程实时可见**：执行者键入、退格、改错的全过程（不只是提交后的
  命令行）其他用户同步看到，效果接近旁观同一个共享屏幕；程序自行关闭回显
  时（如密码输入），谁都看不到输入内容。
- 加入、离开、执行权切换本身**不打印任何提示**——提示符上的名字即表明
  当前执行者；唯一的系统广播是 shshd 执行 `to` 时共享流中出现的命令行：

  ```
  shshd:<当前文件夹名>$ to <name>
  ```
- 新加入者**看不到历史**，只能看到自己加入之后的内容。

### 4. 终端角色

所有客户端始终以原始模式（raw）在线，字节逐键发送给 shshd，由 shshd
按角色和相位仲裁是否写入共享 PTY：

| 角色 / 状态 | 输入处理 |
|---|---|
| 执行者（空闲） | 键入写入 PTY，经行规程回显并广播；本地行编辑（退格 / Ctrl+W / Ctrl+U 等 canonical 能力） |
| 执行者（命令运行中） | 按键直通前台程序（vim 等 TUI 完全可用），交互过程全员可见 |
| 无执行权用户 | 输入不写入 PTY、不回显、不生效；可随时 `^D` 离开 |

角色切换时清空终端输入队列，旧输入不会被当成新角色的命令。

### 5. 相位协议（OSC 633 带内标记）

shshd↔shsh 之间**只有一条文本字节流，没有私有二进制帧**：命令边界、退出码、
执行权、会话关闭全部编码进人眼不可见的 OSC 633 序列（终止符 ST = `ESC \`），
VS Code/iTerm2/kitty 等终端原生理解，其它终端忽略未知 OSC。标记顺序：

```
ESC ] 633 ; A ST                 命令已提交、即将执行
…命令回显/输出…
ESC ] 633 ; C ; <退出码> ST       命令结束（精确退出码）
ESC ] 633 ; P ST                 命令输入已全部消费、shell 空闲（批次结束信号）
ESC ] 633 ; Q ; <长度> ST        续行提示符(PS2)：其后为该长度的可见 PS2 文本
ESC ] 633 ; D ; s=<退出码>;u=<执行者>;w=<目录基名> ST   提示符边界（回到空闲）
<可见 PS1：name:dir$ >
```

- `A` 按"**每条交付的规范行**"发（含 heredoc/复合命令的续行），`C` 按"**每条
  顶层命令结束**"发；两者数量在跨行复合命令下不相等。`P` 在"命令输入排空、
  shell 空闲"时随 `C` 一起发出，`send` 据此精确判定一批输入已全部执行完
  （仅按换行数凑 `C` 会对不上）。`Q` 携带 PS2 长度，`send` 据此精确抑制
  续行提示符（`> `），只留命令输出。

- 执行权变化后 shell 重打的提示符自带新 `u=`，取代独立的角色推送。
- 会话关闭为 `ESC ] 633 ; E ; reason=<原因> ST`，随后即 EOF。
- 机器/管道客户端从同一流解析这些标记即可切分命令；`shsh --plain` 可剥除
  标记只留纯文本。

### 6. 会话级共享状态

会话的工作目录、环境变量、`$?`、作业由 shell 执行层统一持有，对所有用户
和后续命令可见：任何一个人 `cd`、`export`，所有人共享同一份状态，
提示符里的目录名随之变化。后台作业（`jobs`/`fg`/`bg`）同样属于整个会话。

shsh 的 shell 是**终端会话首领**，PTY 为其控制终端，作业控制完整可用：
交互式子 shell（`bash`）、全屏程序（`vim`）、`^Z`/`fg`/`bg`、`^C` 均按
标准终端语义工作（`bash` 进入后 `$-` 含 `m`，不再提示"无任务控制"）。

### 7. 异常治理与单流所有权

**共享流任何时刻只有一个所有者**（空闲=执行者，运行=前台命令）；
非所有者的输入与输出一律阻塞，绝不交错：

- PTY 启用 **TOSTOP**：后台作业向终端输出时会被内核以 SIGTTOU 停止，
  一个字节都不会插入共享流；需要其输出时用 `fg` 接入前台。
- 执行者**离开（Ctrl+D/exit）或网络掉线**：一律**结束整个前台任务**——
  前台进程组被 SIGHUP，同时放弃 shell 当前命令（复合命令不再继续下一轮，
  TUI/半程程序也不留给下一位）。前台任务的生命周期绑定在执行者上；被
  TOSTOP 停止的后台作业保留，新执行者可 `jobs`/`fg` 接管。
- shshd 关停：关闭 PTY，会话内所有进程随之终止。
- socket 无对端鉴权（0666），定位为**同机互信工具**。

## 控制键语义（shshd 统一接管，不经过 shell）

| 键 | 非执行者（follower） | 执行者 |
|---|---|---|
| **Ctrl+C** | 结束本客户端（离开会话） | 空闲/续行：无效果。运行中：把 `0x03` 写入共享 PTY，由内核按前台进程组投递 SIGINT 中断前台命令（前台 raw/TUI 则作按键送交）；**人留在会话** |
| **Ctrl+D** | 离开会话 | 空闲/续行：离开会话（续行时同时放弃半成品）；运行中：**交给前台任务**（canonical 得 EOF、raw 得按键），**人留在会话** |

两条模式源于 shsh 的同步共享原则：follower 的“前台”就是 shsh 客户端本身，
所以 ^C/^D 结束它；执行者的前台是 shshd 手上的共享命令，所以按相位裁决。
客户端据最新 D 标记里的 `u=<我?>` 切换本地终端：**follower 保持 cooked（关回显）**，让本地系统
处理 ^C/^D——因此 **shshd 卡死时 follower 也能正常退出**；**执行者用 raw**，
^C/^D 作为字节上送由 shshd 裁决。Ctrl+C/D 对 shshd 自身都不是信号（结束
shshd 从外部 `kill`）。运行态把 ^D 交给前台后，若前台不读 stdin（如 `sleep`），
该 ^D 无效——先 `^C` 再 `^D`；极端情形可用 `pkill shshd` 兜底。前台程序改过
终端模式后退出/被杀，会话层会把行规程复位回基线。

## 提示符

```
<name>:\W$
```

- `name`：当前执行者（系统持有时显示 `shshd`）
- `\W`：会话当前工作目录的文件夹名

## Shell 语义

除会话控制命令（`to` / `exit`）外，输入由会话内的 shell 执行层处理，
行为是普通 shell 子集，不与共享模型冲突：分词与引号、变量展开（`$VAR`、
`${VAR}`、`$?`）、`;`/`&&`/`||`、管道、重定向、`cd`/`pwd`/`echo`/
`export`/`unset` 等内建、外部命令执行。复杂语法可用 `sh -c '...'` 兜底。

## 协议（统一文本，OSC 633）

AF_UNIX 承载一条**双向纯文本字节流**，没有任何带外二进制帧；也可直接用
`nc -U /tmp/shsh/<session>.sock` 调试。

- 连接后客户端先发**首行文本握手**：`<name>\n` 加入会话；`list\n` 则做一次
  性纯文本列表查询（服务端应答到 EOF，不属于共享终端流）。
- 上行（shsh→shshd）：首行之后的全部字节即原始键入（含 ^C/^D），逐键透传；
  终端尺寸夹带在流中的 `ESC ] 633 ; W ; <rows> ; x <cols> ST` 标记里，
  shshd 剥离后转 `TIOCSWINSZ`，绝不落入 PTY 输入。
- 下行（shshd→shsh）：终端文本 + OSC 633 标记：

  | 序列 | 含义 |
  |---|---|
  | `ESC]633;A ST` | 命令已提交、即将执行 |
  | `ESC]633;C;<退出码> ST` | 命令结束，携带精确退出码 |
  | `ESC]633;P ST` | 命令输入已全部消费、shell 空闲 |
  | `ESC]633;Q;<长度> ST` | 续行提示符(PS2)：其后为该长度的可见文本 |
  | `ESC]633;D;s=<退出码>;u=<执行者>;w=<目录> ST` | 提示符边界/当前执行者 |
  | `ESC]633;E;reason=<原因> ST` | 会话即将关闭，随后 EOF |

  自身是否为执行者 = 比较最新 `D` 的 `u=` 与命令行给定的名字；终止信号即
  EOF（Unix 惯例）。标记对普通终端不可见，`shsh --plain` 可剥除。

## agent/脚本通道（send）

`send` 把共享会话封装成**同步命令调用**，供脚本/Agent 非交互使用。关键设计：
**shsh 是唯一的 shshd 客户端**，同时充当 send 的本地端点 —— 无论前台交互
还是后台（`shsh login -n X -d`），shsh 都会在 `~/.shsh/` 下建立
`<会话>__<用户>.{in,out,cursor,pid}` 通道：命令经 `.in`（FIFO，shsh 以
`O_RDWR` 自持写端、永不 EOF）注入，shshd 输出被 shsh 原样 tee 到 `.out`。
`send` 只做文件 I/O，不连 socket、不启动 shsh。

```bash
# 起后台端点（供 send 使用；前台交互 shsh login 同样可作为端点）
shsh login -n alice -s work -d

send -n alice -s work 'make && make test'   # 等执行权→投递→等命令结束，打印增量
send -n alice -s work 'to bob'              # 转交执行权；在 D;u=bob 处返回
send -n alice -s work - <<'EOF'             # 从 stdin 读多行脚本
echo one
echo two
EOF
shsh logout -n alice -s work                # 停止端点并清理文件
```

- **寻址**：`(会话, 用户名)` 是唯一键，文件即句柄；`-n/-s` 省略时取
  `$SHSH_NAME/$SHSH_SESSION`，再退到 `~/.shsh/current`（上次成功绑定）。
- **省 token**：默认只返回命令输出，吞掉自身提示符与命令回显；`-l` 连回显
  一起返回。**别人的活动始终返回。**
- **多行**：命令可含换行（多行脚本逐行执行、全部结束后返回）；参数为 `-`
  时从 stdin 读。
- 执行者不在自己时，`send <命令>` 会等待交权再投递；端点不存在则报错
  （send 不负责启动 shsh，shsh 生命周期由 shsh 自己管理）。
- 游标按 `.out` 绝对偏移推进，每次只返回上次结束后的增量，OSC 标记被剥除。
- **输入长度**：命令参数受内核单参数上限（`MAX_ARG_STRLEN`，约 **128 KiB**）
  限制，超出时 shell 直接报 `Argument list too long`（E2BIG）——这是内核硬限；
  大输入请走 stdin（`send - <<EOF`），stdin 路径**无固定上限**：client 非阻塞
  I/O + 上行缓冲、server 端以**背压**（输出积压达 1 MiB 软上限暂停读该客户端
  输入，排空后恢复；仅 64 MiB 硬上限防失控 OOM）替代断连，端到端限流，已实测
  4 MiB 输出平滑完成。单行仍受 pty canonical `MAX_CANON`（约 4096 字节）
  限制，超长单行会被截断，长内容请拆多行或 `sh -c` 包裹。

## 边界

- 输出是**线性共享流**，全屏 TUI 程序（vi/top 等）以"共享屏幕旁观"方式
  呈现；PTY 窗口尺寸跟随当前执行者。
- 无历史记录（↑↓）、无 Tab 补全、无宽字符光标处理：空闲态编辑为
  PTY canonical 能力（退格、Ctrl+W/U）。
- 回显经 shshd 往返（定位同机 AF_UNIX，延迟可忽略）。
- 后台作业输出被 TOSTOP 阻塞，需 `fg` 接管，不会与他人输入交错。
- socket 无鉴权，只适合同机互信环境。

## 开发

C11，仅依赖 Linux（epoll + PTY + AF_UNIX），三个二进制：

- `bin/shshd`：会话层（socket、执行权、广播、PTY 仲裁）+ shell 执行层
- `bin/shsh`：交互客户端，同时是 send 的本地端点（FIFO 注入 + 输出 tee + OSC 633 扫描）
- `bin/send`：非交互 agent/脚本通道（纯文件增量客户端）

```bash
make            # 发布版（-O2，自有层零警告）
make debug      # 无优化带调试信息
make asan       # AddressSanitizer/UBSan 构建（bin-asan/）
make test       # 自动化测试（87 项）
```

## License

shsh 原创部分以 **MIT License** 发布，详见 [LICENSE](LICENSE)。

shsh 在单进程内整合了 [dash](https://git.kernel.org/pub/scm/utils/dash/dash.git)
（BSD 3-Clause，与 MIT 兼容）与少量 Public Domain / GPL-2.0+ 构建期组件。
各第三方组件的版权归属、许可条款与再分发义务见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，dash 完整许可文本见
[licenses/dash-COPYING](licenses/dash-COPYING)。
