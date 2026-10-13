"""CLI 与用法（shsh / shshd 命令行）。"""

from harness import *  # noqa: F401,F403


def test_help():
    r = run(bin_path("shsh"), "-h")
    assert r.returncode == 0, "shsh -h 应返回 0"
    assert "用法" in r.stdout


def test_empty_list():
    clear_sockdir()
    r = run(bin_path("shsh"), "-l")
    assert r.returncode == 0
    assert "无活动会话" in r.stdout


def test_missing_session():
    r = run(bin_path("shsh"), "login", "-n", "a", "-s", "nosuch")
    assert r.returncode == 1
    assert "不存在" in r.stderr


def test_no_args_usage():
    """shsh 无参数：用法错误，返回 2"""
    r = run(bin_path("shsh"))
    assert r.returncode == 2
    assert "用法" in r.stderr


def test_bad_option_usage():
    """未知选项：返回 2（shsh 与 shshd 一致）"""
    assert run(bin_path("shsh"), "--bad").returncode == 2
    assert run(bin_path("shshd"), "--bad").returncode == 2


def test_shshd_help():
    r = run(bin_path("shshd"), "-h")
    assert r.returncode == 0, r
    assert "用法" in r.stdout and "会话" in r.stdout
    # 帮助文本声明了默认会话
    assert "session" in r.stdout


def test_default_session():
    """shshd 省略会话名 → 默认会话 "session"；shsh login 省略会话名也加入它"""
    clear_sockdir()
    d = None
    alice = None
    try:
        d = Proc([bin_path("shshd")], "shshd")   # 默认会话
        wait_socket("session")
        alice = join("session", "alice")
        alice.assert_contains(ps1("alice"))
        alice.send("echo default-ok\n")
        alice.assert_contains("default-ok\r\n")
    finally:
        kill_all([d, alice])
        clear_sockdir()


def test_login_requires_name():
    """login 缺 -n → 用法错误 rc=2"""
    assert run(bin_path("shsh"), "login").returncode == 2


def test_logout_no_endpoint():
    """logout 无端点也应正常结束（rc=0）"""
    r = run(bin_path("shsh"), "logout", "-n", "ghost", "-s", "nosuch")
    assert r.returncode == 0, r.stderr
