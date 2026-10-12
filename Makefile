# shsh —— 多用户共享 shell（单进程整合 dash 0.5.12 解释层 + shsh 会话层）
#
# 单一 src/：dash 源文件与 shsh 会话层平级混编，本 Makefile 统一产出
# bin/shshd（会话 + shell 本体）与 bin/shsh（客户端）。
# dash 的构建期生成物（token/builtins/nodes/syntax/init/signames）由
# 保留的生成工具在 build/ 下现产，config.h 随源码提供（Linux/glibc、SMALL）。

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
CPPFLAGS += -include src/config.h -DBSD=1 -DSHELL -DSHSH_EMBED -D_GNU_SOURCE
INCS     = -Isrc -I$(BUILD)

BUILD    ?= build
BIN_DIR  ?= bin

# ---- 安装位置（标准 DESTDIR/PREFIX） ----
PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR   = $(DESTDIR)$(PREFIX)/bin
DOCDIR   = $(DESTDIR)$(PREFIX)/share/doc/shsh
INSTALL  ?= install

# 发布版本号（默认取 git 标签；无标签时回退 dev）
VERSION  ?= $(shell git describe --tags --always 2>/dev/null || echo dev)

# 随二进制一起分发的许可/文档材料（BSD-3-Clause 第 2 条要求二进制再分发
# 必须随附版权与许可声明）
LICENSE_DOCS = LICENSE THIRD_PARTY_NOTICES.md README.md

# ---- dash 层源码清单 ----
DASH_C = alias arith_yacc arith_yylex cd error eval exec expand histedit \
         input jobs mail main memalloc miscbltin mystring options output \
         parser redir show shsh_shell system trap var
DASH_BLTIN = printf test times

# ---- 构建期生成 ----
GEN_HDRS = $(BUILD)/token.h $(BUILD)/token_vars.h $(BUILD)/builtins.h \
           $(BUILD)/nodes.h $(BUILD)/syntax.h
GEN_C    = $(BUILD)/builtins.c $(BUILD)/init.c $(BUILD)/nodes.c \
           $(BUILD)/signames.c $(BUILD)/syntax.c
GEN_O    = $(GEN_C:.c=.o)

HELPERS  = $(BUILD)/mkinit $(BUILD)/mknodes $(BUILD)/mksyntax $(BUILD)/mksignames
HELPER_CPPFLAGS = -DBSD=1 -DSHELL -Isrc -I$(BUILD)

DASH_OBJS = $(addprefix $(BUILD)/,$(addsuffix .o,$(DASH_C) $(DASH_BLTIN))) $(GEN_O)
SRV_OBJS  = $(addprefix $(BUILD)/,$(addsuffix .o,common buf proto pty server))
CLI_OBJS  = $(addprefix $(BUILD)/,$(addsuffix .o,common buf proto client))

BIN = $(BIN_DIR)/shshd $(BIN_DIR)/shsh

all: $(BIN)

# ---- 生成物规则（对应 dash 原 Makefile.am） ----

$(BUILD)/token.h $(BUILD)/token_vars.h: src/mktokens
	@mkdir -p $(BUILD)
	cd $(BUILD) && sh ../$<

$(BUILD)/builtins.def: src/builtins.def.in src/config.h
	@mkdir -p $(BUILD)
	$(CC) -include src/config.h -DBSD=1 -DSHELL -E -x c -o $@ $<

$(BUILD)/builtins.c $(BUILD)/builtins.h: src/mkbuiltins $(BUILD)/builtins.def
	cd $(BUILD) && sh ../$< builtins.def

$(HELPERS): $(BUILD)/%: src/%.c | $(BUILD)/token.h $(BUILD)/token_vars.h
	@mkdir -p $(BUILD)
	$(CC) $(HELPER_CPPFLAGS) -g -O2 -o $@ $<

$(BUILD)/init.c: $(HELPERS) $(addprefix src/,$(addsuffix .c,$(DASH_C)))
	cd $(BUILD) && ./mkinit $(addprefix ../src/,$(addsuffix .c,$(DASH_C)))

$(BUILD)/nodes.c $(BUILD)/nodes.h: $(BUILD)/mknodes src/nodetypes src/nodes.c.pat
	cd $(BUILD) && ./mknodes ../src/nodetypes ../src/nodes.c.pat

$(BUILD)/syntax.c $(BUILD)/syntax.h: $(BUILD)/mksyntax $(BUILD)/token.h
	cd $(BUILD) && ./mksyntax

$(BUILD)/signames.c: $(BUILD)/mksignames
	cd $(BUILD) && ./mksignames

# ---- 编译规则 ----

$(BUILD)/%.o: src/%.c $(GEN_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(INCS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/bltin/%.c $(GEN_HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(INCS) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(BUILD)/%.c $(GEN_HDRS)
	$(CC) $(CPPFLAGS) $(INCS) $(CFLAGS) -c -o $@ $<

# ---- 链接 ----

$(BIN_DIR)/shshd: src/shshd.c $(SRV_OBJS) $(DASH_OBJS) $(GEN_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(INCS) $(CFLAGS) -o $@ src/shshd.c \
		$(SRV_OBJS) $(DASH_OBJS)

$(BIN_DIR)/shsh: src/shsh.c $(CLI_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(INCS) $(CFLAGS) -o $@ src/shsh.c $(CLI_OBJS)

debug: CFLAGS = -g -O0 -Wall
debug: all

# ---- ASan/UBSan 构建（完全独立目录，避免与常规 build/ 混用） ----
# 用递归 make 传入变量，使 $(BIN) 等以 asan 目录重新展开（目标专属变量
# 无法改变已在解析期展开的先决条件表）。
ASAN_CFLAGS = -O1 -g -Wall -fsanitize=address,undefined -fno-omit-frame-pointer

asan:
	$(MAKE) BUILD=build-asan BIN_DIR=bin-asan CFLAGS="$(ASAN_CFLAGS)" all

test: all
	python3 tests/run_tests.py

# ---- 安装 / 卸载 ----
# make install                  -> /usr/local/bin/{shsh,shshd} + 许可文档
# make install PREFIX=$HOME/.local
# make install DESTDIR=/tmp/stage PREFIX=/usr   (打包/暂存)
# 二进制整合了 BSD-3-Clause 的 dash，按其第 2 条随二进制一并安装许可声明。
install: all
	$(INSTALL) -d $(BINDIR) $(DOCDIR)/licenses
	$(INSTALL) -m 755 $(BIN_DIR)/shsh  $(BINDIR)/shsh
	$(INSTALL) -m 755 $(BIN_DIR)/shshd $(BINDIR)/shshd
	$(INSTALL) -m 644 $(LICENSE_DOCS) $(DOCDIR)/
	$(INSTALL) -m 644 licenses/dash-COPYING $(DOCDIR)/licenses/
	@echo "已安装: $(BINDIR)/{shsh,shshd}"
	@echo "许可文档: $(DOCDIR)/"

uninstall:
	rm -f $(BINDIR)/shsh $(BINDIR)/shshd
	rm -rf $(DOCDIR)
	@echo "已卸载: $(BINDIR)/shsh  $(BINDIR)/shshd  及 $(DOCDIR)"

# ---- 合规二进制分发包（供 GitHub Release 上传） ----
# 产物 dist/shsh-<ver>-linux-x86_64.tar.gz：内含 bin/ 与全部许可材料，
# 解压即用且满足 BSD/MIT 的二进制再分发随附声明要求。
BINTARBALL = dist/shsh-$(VERSION)-linux-x86_64.tar.gz

bindist: all
	@rm -rf dist/shsh-$(VERSION)
	@mkdir -p dist/shsh-$(VERSION)/bin dist/shsh-$(VERSION)/licenses
	cp $(BIN_DIR)/shsh $(BIN_DIR)/shshd dist/shsh-$(VERSION)/bin/
	cp $(LICENSE_DOCS) dist/shsh-$(VERSION)/
	cp licenses/dash-COPYING dist/shsh-$(VERSION)/licenses/
	tar -C dist -czf $(BINTARBALL) shsh-$(VERSION)
	@rm -rf dist/shsh-$(VERSION)
	@echo "==> $(BINTARBALL)"
	@sha256sum $(BINTARBALL)

# 在 ASan 构建上跑同一套测试（SHSH_BIN_DIR 覆盖 harness 里的 BIN 目录）
test-asan: asan
	SHSH_BIN_DIR=$(abspath bin-asan) \
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
	python3 tests/run_tests.py

clean:
	rm -rf $(BIN_DIR) $(BUILD)

clean-asan:
	rm -rf build-asan bin-asan

.PHONY: all debug asan test test-asan install uninstall bindist clean clean-asan