"""资源上限、背压与大块输入。

关注 server.c 的输出积压上限（CLIENT_OUT_LIMIT）、非阻塞 pty master 的
写入完整性，以及超长输入行下的会话存活性。统一文本协议下没有帧，RawClient
读到的是纯字节流（hello 后内置后台 drain 持续排空）。
"""

from harness import *  # noqa: F401,F403


def _wait_file_size(path, want, timeout=15.0):
    """轮询直到文件大小等于 want（^D 经有序字节流到达，数据灌完即 EOF）。
    不用提示符子串判据——会话开头的首个 PS1 也在累积缓冲里，会立即误命中。
    返回 True；超时返回 False。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        size = os.path.getsize(path) if os.path.exists(path) else 0
        if size == want:
            return True
        time.sleep(0.1)
    return False


def test_limits_large_paste_no_loss():
    """大块粘贴不丢字节：执行者约 190KB 键入交给 `cat > 文件`，落盘量须相符。

    回归：pty master 非阻塞输入队列满时若不循环续写，大块输入会丢字节。
    """
    clear_sockdir()
    d = start_server("work")
    a = None
    path = "/tmp/shsh_paste"
    if os.path.exists(path):
        os.unlink(path)
    try:
        a = RawClient("work")
        a.hello("paster")
        assert a.wait_mark("D", fields={"u": "paster"}, timeout=5) is not None
        a.raw_input("cat > %s\n" % path)
        a.wait_visible("cat > " + path, timeout=5)
        blob = b"".join(b"L%05d:" % i + b"x" * 88 + b"\n" for i in range(2000))
        assert len(blob) > 150000
        a.raw_input(blob)               # 单次写入 ~190KB
        a.raw_input(b"\x04")            # ^D -> EOF -> cat 结束（排在数据之后）
        ok = _wait_file_size(path, len(blob))
        landed = os.path.getsize(path) if os.path.exists(path) else 0
        assert ok, "大块输入丢失：落盘 %d / 发送 %d" % (landed, len(blob))
    finally:
        if a:
            a.close()
        if os.path.exists(path):
            os.unlink(path)
        kill_all([d])
        clear_sockdir()


def test_limits_follower_backpressure():
    """暂不读的 follower 积压越过旧 1MiB 软上限时不再被断连：服务端只暂停
    读其输入（背压）；follower 排空后仍在线、能收到新提示符。执行者不受影响。"""
    clear_sockdir()
    d = start_server("work")
    a = b = None
    try:
        a = RawClient("work")
        a.hello("alice")
        assert a.wait_mark("D", fields={"u": "alice"}, timeout=5) is not None
        # follower 暂不排空（drain=False），让服务端单客户端积压超过 1MiB
        b = RawClient("work")
        b.hello("bob", drain=False)
        assert b.wait_mark("D", fields={"u": "alice"}, timeout=5) is not None
        a.raw_input("seq 1 300000\n")   # 约 2MB 输出，越过 1MiB 软上限
        time.sleep(2.0)
        # 新语义：不应断开（旧实现会在此断连）。排空后应恢复在线
        assert not b.closed(1.0), "背压下暂不读的 follower 不应被断开"
        b.drain_start()
        assert not b.wait_closed(timeout=3), "排空后 follower 应仍在线"

        # 执行者再跑一条，follower 排空后应能看到新输出（连接确实还活着）
        a.raw_input("echo EXEC_OK\n")
        assert a.wait_visible("EXEC_OK", timeout=5), a.visible()[-300:]
        assert b.wait_visible("EXEC_OK", timeout=8), \
            "follower 排空恢复后应能收到新输出"
    finally:
        if a:
            a.close()
        if b:
            b.close()
        kill_all([d])
        clear_sockdir()


def test_limits_overlong_line_session_alive():
    """超长输入行（> cmd_buf 4096）不使会话崩溃/卡死；会话继续可用。"""
    clear_sockdir()
    d = start_server("work")
    a = None
    try:
        a = RawClient("work")
        a.hello("alice")
        assert a.wait_mark("D", fields={"u": "alice"}, timeout=5) is not None
        a.raw_input(b"echo OVERLONG_$((2+3)) " + b"z" * 5000 + b"\n")
        time.sleep(0.8)
        a.raw_input("echo AFTER_OK\n")
        assert a.wait_visible("AFTER_OK", timeout=5), a.visible()[-300:]
    finally:
        if a:
            a.close()
        kill_all([d])
        clear_sockdir()
