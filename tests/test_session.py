"""会话生命周期与命名、会话内建命令（help）。"""

from harness import *  # noqa: F401,F403


def test_conflict_start():
    clear_sockdir()
    d = start_server("work")
    try:
        r = run(bin_path("shshd"), "work")
        assert r.returncode == 1
        assert "已存在" in r.stderr
    finally:
        kill_all([d])
        clear_sockdir()


def test_stale_socket_rebuild():
    clear_sockdir()
    os.makedirs(SOCKDIR, exist_ok=True)
    path = os.path.join(SOCKDIR, "stale.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)  # 只 bind 不 listen -> 陈旧
    s.close()
    d = start_server("stale")
    try:
        r = run(bin_path("shsh"), "-l", "-s", "stale")
        assert r.returncode == 0
        assert "stale" in r.stdout
    finally:
        kill_all([d])
        clear_sockdir()


def test_shshd_name_reserved():
    """shshd 占用用户名 shshd，不能重名加入"""
    clear_sockdir()
    d = start_server("work")
    p = None
    try:
        p = Proc([bin_path("shsh"), "login", "-n", "shshd", "-s", "work"],
                 "dup")
        rc = p.wait()
        diag = "rc=%r stderr=%r server_rc=%r server_stderr=%r" % (
            rc, p.errtext(), d.p.poll(), d.errtext())
        assert rc == 1, "重名加入应返回 1；" + diag
        assert "已存在" in p.errtext(), diag
    finally:
        kill_all([d, p])
        clear_sockdir()


def test_list_users():
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        r = run(bin_path("shsh"), "-l")
        assert r.returncode == 0, r.stderr
        # 新列表格式：会话名 / 活动用户 / 所有用户
        assert "work\n活动用户:\nalice\n" in r.stdout, r.stdout
        assert "所有用户:\nshshd alice bob\n" in r.stdout, r.stdout

        r2 = run(bin_path("shsh"), "-l", "-s", "work")
        assert r2.returncode == 0
        assert "work\n活动用户:\nalice\n" in r2.stdout
        assert "所有用户:\nshshd alice bob\n" in r2.stdout
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_list_multiple_sessions_format():
    """shsh -l：每个会话一块（会话名/活动用户/所有用户），块间空一行"""
    clear_sockdir()
    da = db = None
    aa = bb = None
    try:
        da = start_server("s1")
        db = start_server("s2")
        aa = join("s1", "alice")
        aa.assert_contains(ps1("alice"))
        bb = join("s2", "bob")
        bb.assert_contains(ps1("bob"))
        time.sleep(0.2)

        r = run(bin_path("shsh"), "-l")
        assert r.returncode == 0, r.stderr
        # 按名字排序：s1 在前 s2 在后，中间恰一个空行
        expected = ("s1\n活动用户:\nalice\n所有用户:\nshshd alice\n"
                    "\n"
                    "s2\n活动用户:\nbob\n所有用户:\nshshd bob\n")
        assert expected in r.stdout, repr(r.stdout)
    finally:
        kill_all([da, db, aa, bb])
        clear_sockdir()


def test_shutdown_bye():
    """shshd 关停由 SIGTERM 触发（SIGINT 归 shell 层作中断合成，不关停，
    见 dash_decision.md 3.1）：广播 BYE、退出、清理 socket。"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))

        d.kill(signal.SIGTERM)
        assert d.wait() == 0
        assert alice.wait() == 0
        alice.assert_contains("会话已结束")
        assert bob.wait() == 0
        bob.assert_contains("会话已结束")
        assert not os.path.exists(os.path.join(SOCKDIR, "work.sock"))
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_help_builtin():
    """会话内 help：内容完整、可重定向、可经管道"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = TtyClient("work", "alice")
        assert alice.wait_line("alice:shsh$ ")

        alice.send("help\r")
        for want in ("shsh — 多用户共享 shell", "会话内建命令",
                     "to <用户>", "执行权", "快捷键", "会话外"):
            alice.assert_contains(want, timeout=5)

        # help 是会话层拦截的会话内命令（不经 shell 解释执行），
        # 故不可重定向/管道；执行后 shell 回到提示符。
        alice.assert_contains(ps1("alice") + "help")
    finally:
        kill_all([d])
        if alice:
            alice.kill()
        clear_sockdir()
