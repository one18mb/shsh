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
    r = run(bin_path("shsh"), "-s", "nosuch", "-n", "a")
    assert r.returncode == 1
    assert "不存在" in r.stderr


def test_shshd_help():
    r = run(bin_path("shshd"), "-h")
    assert r.returncode == 0, r
    assert "用法" in r.stdout and "会话" in r.stdout
    r2 = run(bin_path("shshd"))
    assert r2.returncode == 2
