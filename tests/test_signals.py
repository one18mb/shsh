"""信号与生命周期：SIGTERM / SIGHUP 关停（含前台作业运行中）。"""

from harness import *  # noqa: F401,F403


def _shutdown_via(sig):
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))

        d.kill(sig)
        assert d.wait() == 0, "shshd 应优雅退出（rc=0）"
        assert alice.wait() == 0
        alice.assert_contains("会话已结束")
        assert bob.wait() == 0
        bob.assert_contains("会话已结束")
        assert not os.path.exists(os.path.join(SOCKDIR, "work.sock"))
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_sighup_shutdown():
    """SIGHUP 关停会话：广播 BYE、退出码 0、清理 socket"""
    _shutdown_via(signal.SIGHUP)


def test_sigterm_during_foreground_job():
    """前台作业运行中收到 SIGTERM：仍优雅关停（挂断前台组并广播 BYE）"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))

        alice.send('sh -c "echo RUN; sleep 30"\n')
        bob.assert_contains("RUN\r\n")
        time.sleep(0.3)

        d.kill(signal.SIGTERM)
        assert d.wait() == 0, "运行态关停也应优雅退出"
        assert alice.wait() == 0
        alice.assert_contains("会话已结束")
        assert bob.wait() == 0
        bob.assert_contains("会话已结束")
        assert not os.path.exists(os.path.join(SOCKDIR, "work.sock"))
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()
