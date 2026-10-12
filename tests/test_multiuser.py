"""多用户仲裁：执行权、转交、掉线移交、控制键。"""

from harness import *  # noqa: F401,F403


def test_prompt_and_no_join_broadcast():
    """新加入者看到执行者提示符；加入/离开不广播（不干扰输出流）"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))

        alice_before = alice.text()
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))  # 提示符即表明当前执行者，无额外提示
        time.sleep(0.3)
        assert alice.text() == alice_before, "加入事件不应广播: %r" % alice.text()

        # bob 离开同样不广播
        bob.kill()
        time.sleep(0.4)
        assert alice.text() == alice_before, "离开事件不应广播: %r" % alice.text()
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_shared_output_and_echo():
    """输入过程实时共享；提交后回显与输出都在共享流中"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send("echo hidden")  # 未提交：键入过程已实时广播
        time.sleep(0.3)
        bob.assert_contains(ps1("alice") + "echo hidden")
        alice.assert_contains("echo hidden")

        alice.send("\n")  # 提交
        bob.assert_contains("hidden\r\n")
        alice.assert_contains("hidden\r\n")
        # 命令结束后新提示符回到两个终端
        bob.assert_contains("hidden\r\n" + ps1("alice"))
        alice.assert_contains("hidden\r\n" + ps1("alice"))
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_follower_input_ignored():
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        before = alice.text()
        bob.send("echo evil\n")
        time.sleep(0.5)
        assert alice.text() == before, "无执行权用户的输入不应影响会话"
        assert not bob.contains("evil")
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_to_builtin():
    """to <name>：显式转交（命令由本人发出，显示为本人命令行）"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send("to bob\n")
        bob.assert_contains(ps1("alice") + "to bob")
        bob.assert_contains(ps1("bob"))
        alice.assert_contains(ps1("bob"))

        # bob 获得执行权
        submit(bob, "bob", "echo from-bob", alice, "from-bob\r\n")
        alice.assert_contains(ps1("bob") + "echo from-bob")

        # alice 已无执行权
        before = bob.text()
        alice.send("echo nope\n")
        time.sleep(0.4)
        assert not bob.contains("nope"), "失去执行权后输入应被忽略"
        assert bob.text() != before or True  # bob 自身输出不受影响
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_to_shshd_passthrough():
    """to shshd：执行权交给 shshd，shshd 立即 to 给另一个用户"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send("to shshd\n")
        bob.assert_contains(ps1("alice") + "to shshd")
        bob.assert_contains("shshd:%s$ to bob\r\n" % W)
        bob.assert_contains(ps1("bob"))
        alice.assert_contains("shshd:%s$ to bob\r\n" % W)
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_exit_builtin_transfers():
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send("exit\n")
        assert alice.wait() == 0, "exit 后客户端应正常退出"
        bob.assert_contains("shshd:%s$ to bob\r\n" % W)
        bob.assert_contains(ps1("bob"))
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_auto_transfer_on_drop():
    """执行者掉线：自动移交给最早加入的在线用户（shshd 执行 to）"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = carol = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        carol = join("work", "carol")
        carol.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.kill()
        # 最早加入者 bob 接管
        bob.assert_contains("shshd:%s$ to bob\r\n" % W)
        bob.assert_contains(ps1("bob"))
        carol.assert_contains("shshd:%s$ to bob\r\n" % W)
        carol.assert_contains(ps1("bob"))

        bob.send("echo ok\n")
        carol.assert_contains("ok\r\n")
    finally:
        kill_all([d, alice, bob, carol])
        clear_sockdir()


def test_drop_mid_run_kills_command():
    """执行者中途掉线：命令被挂断，执行权移交"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send('sh -c "echo st; sleep 30"\n')
        bob.assert_contains("st\r\n")
        time.sleep(0.3)

        alice.kill()
        bob.assert_contains("shshd:%s$ to bob\r\n" % W, timeout=6)
        bob.assert_contains(ps1("bob"), timeout=6)

        bob.send("echo alive\n")
        bob.assert_contains("alive\r\n")
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_ctrl_c_interrupts_command():
    """命令运行中 ctrl+c 中断命令本身，而不是退出会话"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send('sh -c "echo st; sleep 30"\n')
        bob.assert_contains("st\r\n")
        time.sleep(0.3)
        n = bob.count(ps1("alice"))

        alice.send(b"\x03")  # 直通 pty，由 pty 行规程产生 SIGINT
        assert bob.wait_count(ps1("alice"), n + 1, timeout=6), \
            "ctrl+c 后应回到提示符: %r" % bob.text()

        alice.send("echo after\n")
        bob.assert_contains("after\r\n")
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_ctrl_d_forwarded_to_running_task():
    """运行态执行者 ^D 交给前台任务自定：canonical 前台得到 EOF 后退出，
    执行者留在会话（不离开）"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send("cat\n")            # canonical 前台，读 fd0=stdin
        time.sleep(0.4)
        n = alice.count(ps1("alice"))
        alice.send(b"\x04")            # ^D -> 转发 -> VEOF -> cat 结束
        assert alice.wait_count(ps1("alice"), n + 1, timeout=5), alice.text()
        assert alice.p.poll() is None, "运行态 ^D 不应离开会话"
        alice.send("echo back\n")
        alice.assert_contains("back\r\n", timeout=5)
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_follower_ctrl_keys_leave():
    """非执行者：其前台即 shsh 自身，^C/^D 均结束其客户端（离开会话）"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        before = alice.text()
        bob.send(b"\x04")              # follower ^D -> 离开
        assert bob.wait() == 0, "follower ^D 应离开"
        time.sleep(0.3)
        assert alice.text() == before, "follower 离开不影响执行者"

        # 换一个 follower，用 ^C 离开
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)
        bob.send(b"\x03")              # follower ^C -> 结束本客户端
        assert bob.wait() == 0, "follower ^C 应结束客户端"
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_follower_exits_when_shshd_stuck():
    """shshd 无响应（纯内建死循环，从不回调会话层）时，follower 仍能本地退出"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = carol = None
    try:
        alice = TtyClient("work", "alice")
        alice.assert_contains(ps1("alice"))
        # 两个 follower 先加入（此刻 shshd 尚响应）
        bob = TtyClient("work", "bob")
        bob.assert_contains(ps1("alice"))
        carol = TtyClient("work", "carol")
        carol.assert_contains(ps1("alice"))
        time.sleep(0.3)

        # 纯内建死循环：shell 不再回到 hook，shshd 不再响应任何请求
        alice.send("while true; do :; done\r")
        time.sleep(0.6)

        # follower 本地 ^D / ^C：等不到 BYE 也能退出
        bob.send("\x04")
        carol.send("\x03")
        deadline = time.time() + 3
        while time.time() < deadline and (
                bob.p.poll() is None or carol.p.poll() is None):
            time.sleep(0.05)
        assert bob.p.poll() is not None, "shshd 卡死时 follower ^D 也应退出"
        assert carol.p.poll() is not None, "shshd 卡死时 follower ^C 也应退出"
    finally:
        for p in (alice, bob, carol):
            if p:
                p.kill()
        kill_all([d])
        clear_sockdir()


def test_multiuser_to_nonexistent():
    """to 不存在的用户：报错且执行权不变"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("to ghost\n")
        alice.assert_contains("不在会话中", timeout=5)
        # 执行权仍在 alice：命令可执行
        submit(alice, "alice", "echo still-alice", alice, "still-alice\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_multiuser_three_way_handover():
    """三名用户按 to 链式转交：alice→bob→carol"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = carol = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        carol = join("work", "carol")
        carol.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send("to bob\n")
        bob.assert_contains(ps1("bob"))
        submit(bob, "bob", "echo from-bob", carol, "from-bob\r\n")

        bob.send("to carol\n")
        carol.assert_contains(ps1("carol"))
        submit(carol, "carol", "echo from-carol", alice, "from-carol\r\n")

        # 失去执行权的 alice 输入无效
        before = carol.text()
        alice.send("echo nope\n")
        time.sleep(0.4)
        assert not carol.contains("nope"), "失去执行权后输入应被忽略"
    finally:
        kill_all([d, alice, bob, carol])
        clear_sockdir()


def test_multiuser_rejoin_same_name():
    """离开后用同名重新加入应成功"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("exit\n")
        assert alice.wait() == 0
        time.sleep(0.3)

        alice = join("work", "alice")   # 同名重入
        alice.assert_contains(ps1("alice"), timeout=5)
        submit(alice, "alice", "echo rejoin-ok", alice, "rejoin-ok\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()
