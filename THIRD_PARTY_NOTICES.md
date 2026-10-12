# 第三方许可声明（Third-Party Notices）

shsh 的原创部分以 **MIT License** 发布（见根目录 [LICENSE](LICENSE)）。
shsh 在单进程内整合了下列第三方软件；其版权与许可声明在再分发时必须保留。

## dash（shell 执行层）

`src/` 下源自 dash 的文件（parser/eval/exec/jobs/expand/trap/input/redir、
各内建、构建期生成工具等，文件头均带版权声明）来自
**dash — Debian Almquist Shell (0.5.12)**：

- Copyright © 1989-1994 The Regents of the University of California
- Copyright © 1997 Christos Zoulas
- Copyright © 1997-2005 Herbert Xu
- 衍生自 Kenneth Almquist 贡献给 Berkeley 的软件

许可：**BSD 3-Clause License**（与 MIT 兼容）。完整条款见
[licenses/dash-COPYING](licenses/dash-COPYING)。再分发须保留上述版权声明、
许可条款与免责声明，且不得用贡献者名义为衍生产品背书。

## test 内建（src/bltin/test.c）

`test(1)` 内建由 Erik Baalbergen、Eric Gisin、Arnold Robbins、
J.T. Conklin 等贡献，声明为 **Public Domain**（见该文件头）。

## mksignames（构建期工具，src/mksignames.c）

该文件源自 GNU Bash，为 **GNU General Public License v2 或更高版本 (GPL-2.0+)**。
它**仅在构建期运行、不与 shsh 链接进二进制**；其输出（信号名表 `signames.c`）
被 dash 上游明确视为可自由使用。说明随 dash 许可文本一并收录于
[licenses/dash-COPYING](licenses/dash-COPYING) 末尾。

---

除上述组件外，shsh 的会话层、客户端、测试与构建脚本（`src/server.c`、
`src/client.c`、`src/proto.*`、`src/pty.*`、`src/buf.*`、`src/common.*`、
`src/shsh*.c`、`tests/`、`Makefile` 等）均为 shsh 原创代码，适用 MIT License。
