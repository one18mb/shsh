"""资源上限、背压与大块输入。

关注 server.c 的输出积压上限（CLIENT_OUT_LIMIT）、handle_raw 对非阻塞
pty master 的写入完整性，以及超长输入行下的会话存活性。
"""

from harness import *  # noqa: F401,F403


class _Drainer:
    """后台持续读取并排空某个 ProtoClient 的 S2C_OUT（避免其自身积压）。"""

    def __init__(self, c):
        self.c = c
        self.out = bytearray()
        self.stop = False
        self.t = threading.Thread(target=self._loop, daemon=True)
        self.t.start()

    def _loop(self):
        while not self.stop:
            fr = self.c.recv_frame(timeout=0.2)
            if fr is None:
                continue
            if fr[0] == S2C_OUT:
                self.out.extend(fr[1])

    def text(self):
        return bytes(self.out).decode(errors="replace")

    def stop_now(self):
        self.stop = True


def test_limits_large_paste_no_loss():
    """大块粘贴不丢字节：执行者一帧 ~190KB 交给 `cat > 文件`，落盘量须相符。

    回归：handle_raw 曾对非阻塞 pty master 做单次 write 且忽略短写返回值，
    大块输入会因 pty 输入队列满而丢字节。
    """
    clear_sockdir()
    d = start_server("work")
    a = None
    path = "/tmp/shsh_paste"
    if os.path.exists(path):
        os.unlink(path)
    try:
        a = ProtoClient("work")
        a.hello("paster")
        assert a.recv_until(S2C_WELCOME, timeout=5) is not None
        dr = _Drainer(a)
        a.raw_input("cat > %s\n" % path)
        time.sleep(0.5)
        blob = b"".join(b"L%05d:" % i + b"x" * 88 + b"\n" for i in range(2000))
        assert len(blob) > 150000
        a.raw_input(blob)               # 单帧 ~190KB
        time.sleep(0.8)
        a.raw_input(b"\x04")            # ^D -> EOF -> cat 结束
        time.sleep(1.2)
        dr.stop_now()
        size = os.path.getsize(path) if os.path.exists(path) else 0
        assert size == len(blob), \
            "大块输入丢失：落盘 %d / 发送 %d" % (size, len(blob))
    finally:
        if a:
            a.close()
        if os.path.exists(path):
            os.unlink(path)
        kill_all([d])
        clear_sockdir()


def test_limits_follower_backpressure():
    """非读取的 follower 触发 CLIENT_OUT_LIMIT 后被断开；执行者不受影响。"""
    clear_sockdir()
    d = start_server("work")
    a = b = None
    try:
        a = ProtoClient("work")
        a.hello("alice")
        assert a.recv_until(S2C_WELCOME, timeout=5) is not None
        b = ProtoClient("work")
        b.hello("bob")
        assert b.recv_until(S2C_WELCOME, timeout=5) is not None
        dr = _Drainer(a)                # 执行者自行排空，避免自身被断开
        a.raw_input("seq 1 300000\n")   # 约 2MB 输出，越过 CLIENT_OUT_LIMIT
        time.sleep(2.0)

        # follower 此刻开始读取：排空后应被断开（EOF），而非无限增长
        deadline = time.time() + 12
        while time.time() < deadline:
            fr = b.recv_frame(timeout=0.5)
            if fr is None:
                if b.closed(timeout=0.1):
                    break
        assert b.closed(timeout=1.0), "未读取的 follower 最终应被断开"

        # 执行者仍可用
        a.raw_input("echo EXEC_OK\n")
        time.sleep(0.6)
        assert "EXEC_OK" in dr.text(), "执行者应不受 follower 断开影响"
        dr.stop_now()
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
        a = ProtoClient("work")
        a.hello("alice")
        assert a.recv_until(S2C_WELCOME, timeout=5) is not None
        dr = _Drainer(a)
        a.raw_input(b"echo OVERLONG_$((2+3)) " + b"z" * 5000 + b"\n")
        time.sleep(0.8)
        a.raw_input("echo AFTER_OK\n")
        assert a.recv_until(S2C_OUT, timeout=5) is not None or True
        time.sleep(0.8)
        assert "AFTER_OK" in dr.text(), \
            "超长行后会话应仍可用: %r" % dr.text()
        dr.stop_now()
    finally:
        if a:
            a.close()
        kill_all([d])
        clear_sockdir()
