# shsh —— 多用户共享 shell（单进程整合 dash 0.5.12 解释层 + shsh 会话层）
#
# 单一 src/：dash 源文件与 shsh 会话层平级混编，本 Makefile 统一产出
# bin/shshd（会话 + shell 本体）与 bin/shsh（客户端）。
# dash 的构建期生成物（token/builtins/nodes/syntax/init/signames）由
# 保留的生成工具在 build/ 下现产，config.h 随源码提供（Linux/glibc、SMALL）。

CC      ?= gcc
CFLAGS  ?= -O2 -Wall
CPPFLAGS += -include src/config.h -DBSD=1 -DSHELL -DSHSH_EMBED -D_GNU_SOURCE
INCS     = -Isrc -Ibuild

BUILD    = build

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

BIN = bin/shshd bin/shsh

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

bin/shshd: src/shshd.c $(SRV_OBJS) $(DASH_OBJS) $(GEN_HDRS)
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(INCS) $(CFLAGS) -o $@ src/shshd.c \
		$(SRV_OBJS) $(DASH_OBJS)

bin/shsh: src/shsh.c $(CLI_OBJS)
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(INCS) $(CFLAGS) -o $@ src/shsh.c $(CLI_OBJS)

debug: CFLAGS = -g -O0 -Wall
debug: all

test: all
	python3 tests/run_tests.py

clean:
	rm -rf bin $(BUILD)

.PHONY: all debug test clean