"""真实终端的 termios / 行编辑行为。"""

from harness import *  # noqa: F401,F403


def test_tty_modes():
    """真实终端：执行者本地回显，只读用户输入完全静默"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = TtyClient("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = TtyClient("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.3)

        # 执行者：本地回显 + 服务端共享回显
        alice.send("echo tty\r")
        alice.assert_contains("echo tty")
        bob.assert_contains(ps1("alice") + "echo tty")
        bob.assert_contains("tty\n")

        # 只读用户：输入无回显、无效果
        bob.send("echo nope\r")
        time.sleep(0.5)
        assert not bob.contains("nope"), "只读用户输入不应有回显: %r" % bob.norm()
        assert not alice.contains("nope"), "只读用户输入不应进入会话"
    finally:
        kill_all([d])
        for p in (alice, bob):
            if p:
                p.kill()
        clear_sockdir()


def test_raw_program_termios_restored():
    """前台程序改过 termios（raw）后退出：会话层回空闲即复位行规程"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("stty -icanon -echo\n")
        time.sleep(0.4)
        alice.send("echo RESTOREME\n")
        # 复位后应恢复输入回显（"echo RESTOREME" 本身出现在共享流中）
        alice.assert_contains("echo RESTOREME", timeout=5)
        alice.assert_contains("RESTOREME\r\n", timeout=5)
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_tty_line_editing():
    """真实终端下行编辑：退格、Ctrl+U（canonical 行规程提供）"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = TtyClient("work", "alice")
        alice.assert_contains("alice:")

        # 退格："echo abX" + 退格 -> "echo ab"
        alice.send("echo abX\x7f\r")
        alice.assert_contains("ab\r\n")

        # Ctrl+U 清行后重输（VKILL 由 pty 行规程处理）
        alice.send("garbage\x15echo clean\r")
        alice.assert_contains("clean\r\n")
        assert not alice.contains("garbage\r\n"), "Ctrl+U 未清行"
    finally:
        kill_all([d])
        if alice:
            alice.kill()
        clear_sockdir()


def test_tty_winch_resize():
    """执行者终端 resize（SIGWINCH→OSC 633 W 标记）改变共享 pty 尺寸"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = TtyClient("work", "alice", cols=80, rows=24)
        alice.assert_contains(ps1("alice"))
        # 直接改客户端 pty 的窗口尺寸：内核向前台客户端发 SIGWINCH，
        # 客户端把尺寸编码进流中的 OSC 633 W 标记，shshd 剥离后
        # TIOCSWINSZ 共享 pty
        fcntl.ioctl(alice.master, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 33, 100, 0, 0))
        time.sleep(0.3)
        alice.send("stty size\r")
        alice.assert_contains("33 100", timeout=5)
    finally:
        kill_all([d])
        if alice:
            alice.kill()
        clear_sockdir()


def test_tty_utf8_passthrough():
    """UTF-8 多字节经 pty 行规程原样透传（输入回显与命令输出一致）"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = TtyClient("work", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("echo 中文-Ünïcödé-ok\r")
        alice.assert_contains("中文-Ünïcödé-ok", timeout=5)
    finally:
        kill_all([d])
        if alice:
            alice.kill()
        clear_sockdir()
