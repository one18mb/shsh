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

# 在 ASan 构建上跑同一套测试（SHSH_BIN_DIR 覆盖 harness 里的 BIN 目录）
test-asan: asan
	SHSH_BIN_DIR=$(abspath bin-asan) \
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 \
	python3 tests/run_tests.py

clean:
	rm -rf $(BIN_DIR) $(BUILD)

clean-asan:
	rm -rf build-asan bin-asan

.PHONY: all debug asan test test-asan clean clean-asan