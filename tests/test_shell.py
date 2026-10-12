"""shell 执行面：多行/复合命令、heredoc、状态持久、重定向、管道、作业控制。"""

from harness import *  # noqa: F401,F403


def test_multiline_exit_not_intercepted():
    """多行命令续行（PS2）中的 exit 行属该命令本体，不应被会话层拦截"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        # if 分支为假：体内的 exit 不执行；关键是不被会话层当作离开指令
        alice.send("if false; then\nexit\nfi\necho alive\n")
        alice.assert_contains("alive\r\n", timeout=5)
        time.sleep(0.3)
        assert alice.p.poll() is None, "续行中的 exit 不应导致离开会话"
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_multiline_abandon_aborts():
    """执行者停在多行命令中途离开：半成品被放弃，不交给新执行者完成"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))

        alice.send("if true; then\n")   # 停在 PS2 续行
        alice.assert_contains("> ")
        alice.kill()                    # 掉线离开（非 ^D）
        # 执行权移交 bob 且回到 PS1（半成品 if 已被放弃）
        bob.assert_contains(ps1("bob"), timeout=5)
        bob.send("echo standalone\n")
        # 命令独立执行（若被并入未完成的 if，则不会有输出）
        bob.assert_contains("standalone\r\n", timeout=5)
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_heredoc_abort_no_leak():
    """heredoc(PS2) 续行中执行者 ^D 离开：半行不得被新执行者执行，
    也不出现旧执行者名的过渡提示符"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("cat - << EOF\n")     # 进入 heredoc 续行
        time.sleep(0.3)
        bob = join("work", "bob")        # 在 PS2 期间加入（follower）
        time.sleep(0.2)
        alice.send("str")                # heredoc 体半行（无换行）
        time.sleep(0.3)
        alice.send(b"\x04")              # 执行者离开：放弃半成品
        assert alice.wait() == 0

        bob.assert_contains(ps1("bob"), timeout=5)
        time.sleep(0.4)
        bob.send("\n")                   # 回车也不应把 str 带出来执行
        time.sleep(0.4)
        assert not bob.contains("not found"), \
            "残留半行不应被执行: %r" % bob.text()
        assert not bob.contains(ps1("alice")), \
            "不应出现旧执行者名的过渡提示符: %r" % bob.text()
        bob.send("echo ok-after\n")
        bob.assert_contains("ok-after\r\n", timeout=5)
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_compound_command_aborted_on_drop():
    """执行者运行复合命令（for）时掉线：终止**整个**命令，不只当前 pipeline"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))

        alice.send("for i in 1 2 3 4 5; do echo start-$i; sleep 1; done\n")
        bob.assert_contains("start-1\r\n", timeout=5)
        alice.kill()  # 掉线（前台任务的生命周期绑定在执行者上）
        bob.assert_contains(ps1("bob"), timeout=5)  # 回到提示符
        time.sleep(1.6)  # 若未整体终止，会继续输出 start-2/start-3
        assert not bob.contains("start-2"), \
            "掉线应终止整个复合命令: %r" % bob.text()
        # 会话仍可用
        bob.send("echo ok-after\n")
        bob.assert_contains("ok-after\r\n", timeout=5)
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_shell_state_persists():
    r"""会话状态在服务端：cd / 变量跨命令保留，\W 跟随当前目录"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))

        alice.send("cd /tmp\n")
        alice.assert_contains(ps1("alice").replace(W, "tmp"))

        submit(alice, "alice", "X=42")
        submit(alice, "alice", "echo val=$X", alice, "val=42\r\n")
        submit(alice, "alice", "pwd", alice, "/tmp\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_capabilities():
    """shell 解释层（dash）：内建 / 外部命令 / 引号 / 变量展开 / $? """
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        # 内建 echo 与外部命令
        submit(alice, "alice", "echo builtin-ok", bob, "builtin-ok\r\n")
        submit(alice, "alice", "/bin/echo external-ok", bob, "external-ok\r\n")

        # 引号与转义
        submit(alice, "alice", 'echo "a  b"', bob, "a  b\r\n")
        submit(alice, "alice", "echo 'raw $HOME'", bob, "raw $HOME\r\n")
        submit(alice, "alice", "echo a\\ b", bob, "a b\r\n")

        # 变量：赋值（会话级）与展开
        submit(alice, "alice", "X=42")
        submit(alice, "alice", "echo val=$X", bob, "val=42\r\n")
        submit(alice, "alice", "echo braces=${X}", bob, "braces=42\r\n")
        submit(alice, "alice", "export Y=exported")
        submit(alice, "alice", "echo y=$Y", bob, "y=exported\r\n")

        # $?：上一条命令状态
        submit(alice, "alice", "false")
        submit(alice, "alice", "echo status=$?", bob, "status=1\r\n")
        submit(alice, "alice", "echo st2=$?", bob, "st2=0\r\n")

        # 不存在的外部命令（dash 层的 not found 报错）
        submit(alice, "alice", "no-such-cmd-xyz", bob, "not found")
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_shell_control_ops():
    """; / && / || 与短路语义"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        submit(alice, "alice", "echo a; echo b", alice, "a\r\nb\r\n")
        # && 短路："no" 只出现在命令回显行一次，不应再有独立输出行
        submit(alice, "alice", "false && echo no", alice)
        assert alice.count("no\n") == 1, \
            "&& 短路失效（no 出现次数异常）: %r" % alice.text()
        submit(alice, "alice", "false || echo yes", alice, "yes\r\n")
        submit(alice, "alice", "true && echo ok", alice, "ok\r\n")
        submit(alice, "alice", "echo x; false; echo y", alice, "x\r\ny\r\n")
        submit(alice, "alice", "true || echo no2", alice)
        assert alice.count("no2\n") == 1, \
            "|| 短路失效（no2 出现次数异常）: %r" % alice.text()
        # 整行状态
        submit(alice, "alice", "false; echo after=$?", alice, "after=1\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_redirection():
    """> >> < 2> 2>&1 与内建命令的重定向"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        os.path.exists("/tmp/shsh_f1") and os.unlink("/tmp/shsh_f1")

        submit(alice, "alice", "echo hi > /tmp/shsh_f1")
        submit(alice, "alice", "cat /tmp/shsh_f1", alice, "hi\r\n")
        submit(alice, "alice", "echo two >> /tmp/shsh_f1")
        submit(alice, "alice", "cat < /tmp/shsh_f1", alice, "hi\r\ntwo\r\n")
        # 内建命令重定向（pwd 在服务端执行，输出需落到文件）
        submit(alice, "alice", "pwd > /tmp/shsh_f1")
        submit(alice, "alice", "cat /tmp/shsh_f1", alice, W + "\r\n")
        # 错误重定向：不得出现在共享输出里
        submit(alice, "alice", "no-such-cmd-xyz 2>/dev/null")
        assert "无法执行" not in alice.text(), "2>/dev/null 未生效"
        os.unlink("/tmp/shsh_f1")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_pipeline():
    """管道：内建在管道中走子进程、外部命令串联、状态取最后一段"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        submit(alice, "alice", 'echo abc | tr a-z A-Z', alice, "ABC\r\n")
        submit(alice, "alice",
               'echo abc | tr a-z A-Z | head -c 1; echo', alice, "A\r\n")
        submit(alice, "alice", "printf 'x\\ny\\n' | wc -l", alice, "2\r\n")
        submit(alice, "alice", "false | true; echo st=$?", alice, "st=0\r\n")
        submit(alice, "alice", "true | false; echo st=$?", alice, "st=1\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_pipeline_ctrl_c():
    """Ctrl+C 挂断整条管道（同一进程组）"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        n = alice.count("alice:")
        alice.send('sh -c "sleep 30" | cat\n')
        time.sleep(0.6)
        alice.send(b"\x03")
        assert alice.wait_count("alice:", n + 1, timeout=6), \
            "ctrl+c 后未回到提示符: %r" % alice.text()
        submit(alice, "alice", "echo alive-pipe", alice, "alive-pipe\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_bash_job_control():
    """pty 为控制终端：shsh 内可跑交互式 bash，且作业控制（-m）可用。
    回归 bug：无控制终端时 bash 报"无法设定终端进程组/此 shell 中无任务控制"。"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = TtyClient("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("bash --norc --noprofile\r")
        assert alice.wait_contains("bash-", timeout=8), \
            "bash 未启动: %r" % alice.norm()
        assert not alice.contains("无法设定终端进程组") and \
            not alice.contains("no job control"), \
            "bash 未取得作业控制: %r" % alice.norm()
        # ${BASH_VERSION:+hasbash} 回显命令行不含结果串，避免误判
        alice.send("echo VER=${BASH_VERSION:+hasbash}\r")
        assert alice.wait_contains("VER=hasbash", timeout=6), \
            "未进入交互式 bash: %r" % alice.norm()
        # 交互式 bash 的 $- 应含 m（monitor=作业控制）
        alice.send("case $- in *m*) echo JOBCTL_$((40+2));;"
                   " *) echo NOJOBC_$((40+2));; esac\r")
        assert alice.wait_contains("JOBCTL_42", timeout=6), \
            "bash 作业控制未开启: %r" % alice.norm()
        alice.send("exit\r")
        assert alice.wait_line(ps1("alice"), timeout=6), \
            "退出 bash 未回到 shsh 提示符: %r" % alice.norm()
    finally:
        kill_all([d])
        if alice:
            alice.kill()
        clear_sockdir()


def test_background_job_tostop():
    """TOSTOP：后台作业写共享终端被 SIGTTOU 停止（控制终端生效）"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        # 输出标记用 $((1+1)) 使回显/作业列表里是字面量，只有真正执行才出 OUT_2
        alice.send("sh -c 'echo OUT_$((1+1)); sleep 8' &\n")
        time.sleep(0.6)                 # 后台作业写终端、被 SIGTTOU 停止
        alice.send("sleep 0\n")         # 前台命令：令 shell reap 到已停止的后台作业
        time.sleep(0.4)
        alice.send("jobs\n")
        assert alice.wait_contains("Stopped", timeout=8), \
            "后台写终端应被 TOSTOP 停止: %r" % alice.text()
        assert not alice.contains("OUT_2"), \
            "后台作业的输出不应出现在共享流: %r" % alice.text()
        alice.send("kill -KILL %1\n")
        alice.send("echo after\n")
        alice.assert_contains("after\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_subshell_and_functions():
    """子 shell ()、函数定义与调用"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        submit(alice, "alice", "echo start; (echo inner); echo end",
               alice, "start\r\ninner\r\nend\r\n")
        submit(alice, "alice", "f() { echo FUNC-$1; }; f hi", alice, "FUNC-hi\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_arith_and_cmdsubst():
    """算术展开 $((...))、命令替换 $(...) 与反引号"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        submit(alice, "alice", "echo $((2+3*4))", alice, "14\r\n")
        submit(alice, "alice", "echo $(echo nested)", alice, "nested\r\n")
        submit(alice, "alice", "echo `echo bt`", alice, "bt\r\n")
        submit(alice, "alice", "X=$(printf ab); echo X=$X", alice, "X=ab\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_loops_and_case():
    """for / while / until 循环与 case 分支"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        submit(alice, "alice",
               "for i in 1 2 3; do printf '%s' $i; done; echo",
               alice, "123\r\n")
        submit(alice, "alice",
               "i=0; while [ $i -lt 3 ]; do i=$((i+1)); done; echo w=$i",
               alice, "w=3\r\n")
        submit(alice, "alice",
               "i=0; until [ $i -ge 2 ]; do i=$((i+1)); done; echo u=$i",
               alice, "u=2\r\n")
        submit(alice, "alice",
               "case abc in a*) echo CASE_OK;; *) echo no;; esac",
               alice, "CASE_OK\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_heredoc_normal():
    """heredoc 正常路径：多行体作为命令输入被正确解析执行"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("cat << EOF\nline1\nline2\nEOF\n")
        alice.assert_contains("line1\r\n", timeout=5)
        alice.assert_contains("line2\r\n", timeout=5)
        # 结束后回到提示符，可继续执行
        submit(alice, "alice", "echo hd-after", alice, "hd-after\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_set_errexit_in_subshell():
    """set -e 语义（在子 shell 中验证，避免终止会话）：失败即退出该子 shell"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("( set -e; false; echo NOT_$((6*7)) ); echo RC=$?\n")
        alice.assert_contains("RC=1", timeout=5)
        assert not alice.contains("NOT_42"), \
            "set -e 应使子 shell 在 false 后退出: %r" % alice.text()
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_set_nounset():
    """set -u 未定义变量报错但不终止会话"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("set -u\n")
        time.sleep(0.3)
        alice.send("echo ${UNDEFINED_VAR_XYZ}\n")
        time.sleep(0.5)
        submit(alice, "alice", "set +u; echo after-u", alice, "after-u\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_glob():
    """通配符展开"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    for p in ("/tmp/shsh_g1", "/tmp/shsh_g2"):
        open(p, "w").close()
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        submit(alice, "alice", "echo /tmp/shsh_g[12]",
               alice, "/tmp/shsh_g1 /tmp/shsh_g2\r\n")
    finally:
        kill_all([d, alice])
        for p in ("/tmp/shsh_g1", "/tmp/shsh_g2"):
            if os.path.exists(p):
                os.unlink(p)
        clear_sockdir()


def test_shell_read_builtin():
    """read 内建从 fd0（共享 pty，运行态由前台任务持有）读取一行。

    回归关注点：会话层在运行态不读 slave，read 能独占读到执行者键入的整行。"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("read X; echo GOT=$X\n")
        time.sleep(0.4)
        alice.send("hello-read\n")
        alice.assert_contains("GOT=hello-read", timeout=5)
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_shell_trap():
    """trap：捕获信号并在下一命令边界执行处理动作"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("trap 'echo TRAPPED' USR1; kill -USR1 $$; echo after-kill\n")
        assert alice.wait_contains("TRAPPED", timeout=5), \
            "trap 处理动作未执行: %r" % alice.text()
    finally:
        kill_all([d, alice])
        clear_sockdir()
