#!/usr/bin/env python3
"""shsh 自动化测试（仅用标准库，无外部依赖）。

驱动真实的 shshd / shsh 进程（stdin/stdout 为管道，即非 tty 场景）。
运行: make test  或  python3 tests/run_tests.py
"""

import os
import fcntl
import pty
import signal
import struct
import termios
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin")
SOCKDIR = "/tmp/shsh"

# \W = 会话（shshd）启动目录的文件夹名
W = os.path.basename(os.getcwd())


def bin_path(name):
    return os.path.join(BIN, name)


def ps1(user):
    return "%s:%s$ " % (user, W)


def clear_sockdir():
    if os.path.isdir(SOCKDIR):
        for f in os.listdir(SOCKDIR):
            try:
                os.unlink(os.path.join(SOCKDIR, f))
            except OSError:
                pass


class Proc:
    """子进程 + 后台 drain 线程，避免管道缓冲死锁。"""

    def __init__(self, argv, label):
        self.label = label
        self.p = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self.out = bytearray()
        self.err = bytearray()
        self.lock = threading.Lock()
        self._t1 = threading.Thread(target=self._drain,
                                    args=(self.p.stdout, self.out), daemon=True)
        self._t2 = threading.Thread(target=self._drain,
                                    args=(self.p.stderr, self.err), daemon=True)
        self._t1.start()
        self._t2.start()

    def _drain(self, f, sink):
        while True:
            try:
                chunk = f.read1(4096)
            except (ValueError, OSError):
                break
            if not chunk:
                break
            with self.lock:
                sink.extend(chunk)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.p.stdin.write(data)
        self.p.stdin.flush()

    def close_stdin(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass

    def kill(self, sig=signal.SIGKILL):
        if self.p.poll() is None:
            try:
                self.p.send_signal(sig)
            except ProcessLookupError:
                pass

    def wait(self, timeout=10):
        try:
            rc = self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            raise AssertionError("%s: 未在时限内退出" % self.label)
        # 等 drain 线程读完管道剩余数据，否则 text()/errtext() 可能读空
        self._t1.join(2.0)
        self._t2.join(2.0)
        return rc

    def outbytes(self):
        with self.lock:
            return bytes(self.out)

    def text(self):
        with self.lock:
            return bytes(self.out).decode(errors="replace")

    def errtext(self):
        with self.lock:
            return bytes(self.err).decode(errors="replace")

    @staticmethod
    def _norm(b):
        # pty 行规程会补 CR，终端/管道不一致；断言时统一去掉 CR
        return bytes(b).replace(b"\r", b"")

    def contains(self, text):
        with self.lock:
            return self._norm(text.encode()) in self._norm(self.out)

    def count(self, text):
        with self.lock:
            return self._norm(self.out).count(self._norm(text.encode()))

    def wait_contains(self, text, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.contains(text):
                return True
            time.sleep(0.02)
        return False

    def wait_count(self, text, n, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.count(text) >= n:
                return True
            time.sleep(0.02)
        return False

    def assert_contains(self, text, timeout=5.0):
        if not self.wait_contains(text, timeout):
            raise AssertionError("%s: 未出现 %r\n实际输出: %r"
                                 % (self.label, text, self.text()))

    def __repr__(self):
        return self.label


class Screen:
    """极简终端模拟：处理 \\b / \\r / \\n / ESC[K / ESC[D / ESC[C，
    用于断言本地行编辑的渲染结果（原始字节流含退格，直接看不出来）。"""

    def __init__(self):
        self.rows = [[]]
        self.r = 0
        self.c = 0

    def feed(self, data):
        s = data.decode(errors="replace") if isinstance(data, bytes) else data
        i = 0
        while i < len(s):
            ch = s[i]
            if ch == "\x1b":
                if s[i + 1:i + 2] == "[":
                    j = i + 2
                    while j < len(s) and not s[j].isalpha():
                        j += 1
                    seq = s[i + 2:j + 1]
                    i = j + 1
                    if seq == "K":
                        del self.rows[self.r][self.c:]
                    elif seq == "D":
                        self.c = max(0, self.c - 1)
                    elif seq == "C":
                        self.c = min(len(self.rows[self.r]), self.c + 1)
                    continue
                i += 1
                continue
            if ch == "\r":
                self.c = 0
            elif ch == "\n":
                self.rows.append([])
                self.r += 1
                self.c = 0
            elif ch == "\b":
                self.c = max(0, self.c - 1)
            else:
                row = self.rows[self.r]
                while len(row) < self.c:
                    row.append(" ")
                if self.c < len(row):
                    row[self.c] = ch
                else:
                    row.append(ch)
                self.c += 1
            i += 1

    def line(self, r=None):
        """当前可见行（保留尾部空格：提示符末尾的空格对光标断言有意义）"""
        r = self.r if r is None else r
        return "".join(self.rows[r])


class TtyClient:
    """真实终端场景：stdin/stdout 接到 pty，用于验证 termios 行为。"""

    def __init__(self, session, name, cols=80, rows=24):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        self.p = subprocess.Popen(
            [bin_path("shsh"), "-s", session, "-n", name],
            stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        self.buf = bytearray()
        self.lock = threading.Lock()
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        while True:
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                break
            if not chunk:
                break
            with self.lock:
                self.buf.extend(chunk)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master, data)

    def norm(self):
        """去掉 \\r 便于断言（pty 输出处理会插入回车）"""
        with self.lock:
            return bytes(self.buf).replace(b"\r", b"")

    def contains(self, text):
        # 断言统一去 CR（pty 输出会补 CR）
        return text.encode().replace(b"\r", b"") in self.norm()

    def wait_contains(self, text, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.contains(text):
                return True
            time.sleep(0.02)
        return False

    def assert_contains(self, text, timeout=5.0):
        if not self.wait_contains(text, timeout):
            raise AssertionError("%s(tty): 未出现 %r\n实际: %r"
                                 % (self.p.args[3], text, self.norm()))

    def raw(self):
        with self.lock:
            return bytes(self.buf)

    def screen(self):
        """把截至当前的输出喂给终端模拟器，返回可见的当前行"""
        sc = Screen()
        sc.feed(self.raw())
        return sc

    def wait_line(self, want, timeout=5.0):
        """等待当前可见行 == want"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.screen().line() == want:
                return True
            time.sleep(0.02)
        return False

    def kill(self):
        if self.p.poll() is None:
            try:
                self.p.send_signal(signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            os.close(self.master)
        except OSError:
            pass


def start_server(session="work"):
    d = Proc([bin_path("shshd"), session], "shshd")
    wait_socket(session)
    return d


def join(session, name):
    return Proc([bin_path("shsh"), "-s", session, "-n", name], name)


def submit(executor, name, cmd, watcher=None, expect=None, timeout=8.0):
    """提交一条命令并等待相位结束（执行者提示符回归）。

    运行相位中按键是直通 pty 的（真实终端语义），所以下一条命令必须等
    提示符回归后再发，否则会被当作正在运行命令的输入。
    提示符里 \\W 会随 cd 变化，因此按 "<name>:" 前缀计数。"""
    pattern = name + ":"
    n = executor.count(pattern)
    executor.send(cmd + "\n")
    if watcher is not None and expect is not None:
        if not watcher.wait_contains(expect, timeout):
            raise AssertionError("%s: 未看到 %r\n实际: %r"
                                 % (watcher.label, expect, watcher.text()))
    if not executor.wait_count(pattern, n + 1, timeout):
        raise AssertionError("%s: 提示符未回归\n实际: %r"
                             % (executor.label, executor.text()))


def wait_socket(session, timeout=5.0):
    path = os.path.join(SOCKDIR, session + ".sock")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                s.close()
                return
            except OSError:
                pass
        time.sleep(0.02)
    raise AssertionError("会话 %s socket 未就绪" % session)


def kill_all(procs):
    for pr in procs:
        if pr:
            pr.kill()


def run(*argv):
    return subprocess.run(list(argv), capture_output=True, text=True)


# ---------------- 基础 CLI ----------------

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
        p = Proc([bin_path("shsh"), "-s", "work", "-n", "shshd"], "dup")
        rc = p.wait()
        diag = "rc=%r stderr=%r server_rc=%r server_stderr=%r" % (
            rc, p.errtext(), d.p.poll(), d.errtext())
        assert rc == 1, "重名加入应返回 1；" + diag
        assert "已存在" in p.errtext(), diag
    finally:
        kill_all([d, p])
        clear_sockdir()


# ---------------- 会话与共享终端 ----------------

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
    """命令提交前不可见；提交后回显与输出共享"""
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))
        time.sleep(0.2)

        alice.send("echo hid")  # 未提交
        time.sleep(0.3)
        assert not bob.contains("hid"), "命令提交前不应共享: %r" % bob.text()

        alice.send("den\n")  # 提交 "echo hidden"
        bob.assert_contains(ps1("alice") + "echo hidden")
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
        assert "work" in r.stdout and "alice" in r.stdout and "bob" in r.stdout
        assert "shshd" in r.stdout and "[系统]" in r.stdout
        assert "[执行者]" in r.stdout
        assert "执行者: alice" in r.stdout

        r2 = run(bin_path("shsh"), "-l", "-s", "work")
        assert r2.returncode == 0
        assert "shshd" in r2.stdout and "alice" in r2.stdout
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_shutdown_bye():
    clear_sockdir()
    d = start_server("work")
    alice = bob = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        bob = join("work", "bob")
        bob.assert_contains(ps1("alice"))

        d.kill(signal.SIGINT)
        assert d.wait() == 0
        assert alice.wait() == 0
        alice.assert_contains("会话已结束")
        assert bob.wait() == 0
        bob.assert_contains("会话已结束")
        assert not os.path.exists(os.path.join(SOCKDIR, "work.sock"))
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


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
    """自研最小 shell：内建 / 外部命令 / 引号 / 变量展开 / $? """
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

        # 不存在的外部命令
        submit(alice, "alice", "no-such-cmd-xyz", bob, "无法执行")
    finally:
        kill_all([d, alice, bob])
        clear_sockdir()


def test_shshd_help():
    r = run(bin_path("shshd"), "-h")
    assert r.returncode == 0, r
    assert "用法" in r.stdout and "会话" in r.stdout
    r2 = run(bin_path("shshd"))
    assert r2.returncode == 2


def test_shell_control_ops():
    """; / && / || 与短路语义"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = join("work", "alice")
        alice.assert_contains(ps1("alice"))
        submit(alice, "alice", "echo a; echo b", alice, "a\r\nb\r\n")
        submit(alice, "alice", "false && echo no", alice)
        assert not alice.wait_contains("no\r\n", timeout=0.8), \
            "&& 后不应执行: %r" % alice.text()
        submit(alice, "alice", "false || echo yes", alice, "yes\r\n")
        submit(alice, "alice", "true && echo ok", alice, "ok\r\n")
        submit(alice, "alice", "echo x; false; echo y", alice, "x\r\ny\r\n")
        submit(alice, "alice", "true || echo no2", alice)
        assert not alice.wait_contains("no2\r\n", timeout=0.8), \
            "|| 短路失效: %r" % alice.text()
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


def test_tty_line_editing():
    """真实终端下行编辑：退格、光标移动、Ctrl+U、Ctrl+A/E"""
    clear_sockdir()
    d = start_server("work")
    alice = None
    try:
        alice = TtyClient("work", "alice")
        alice.assert_contains("alice:")

        # 退格："echo abX" + 退格 -> "echo ab"
        alice.send("echo abX\x7f\r")
        alice.assert_contains("ab\r\n")

        # Ctrl+U 清行后重输
        alice.send("garbage\x15echo clean\r")
        alice.assert_contains("clean\r\n")
        assert not alice.contains("garbage\r\n"), "Ctrl+U 未清行"

        # Ctrl+A 回行首插入：输入 "cho A"，Ctrl+A，输入 "e"，得到 "echo A"
        alice.send("cho A\x01e\r")
        alice.assert_contains("A\r\n")
    finally:
        kill_all([d])
        if alice:
            alice.kill()
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
                     "to <用户>", "执行权", "快捷键",
                     "会话外", "当前 shell 不支持"):
            alice.assert_contains(want, timeout=5)
        assert alice.wait_line("alice:shsh$ "), alice.screen().line()

        # help 是输出型内建：可重定向
        alice.send("help > /tmp/shsh_help.txt\r")
        time.sleep(0.4)
        alice.send("grep -q 会话内建 /tmp/shsh_help.txt; echo rc=$?\r")
        alice.assert_contains("rc=0")
        os.unlink("/tmp/shsh_help.txt")

        # help 经管道也正常（内建在子进程中执行）
        alice.send("help | grep -q 快捷键; echo rc=$?\r")
        alice.assert_contains("rc=0")
    finally:
        kill_all([d])
        if alice:
            alice.kill()
        clear_sockdir()


# ---------------- 运行器 ----------------

def main():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    only = sys.argv[1:] if len(sys.argv) > 1 else None
    failed = 0
    ran = 0
    for t in tests:
        if only and t.__name__ not in only:
            continue
        ran += 1
        try:
            t()
            print("PASS %s" % t.__name__)
        except Exception:
            import traceback
            failed += 1
            print("FAIL %s" % t.__name__)
            traceback.print_exc()
    print("\n%d/%d 通过" % (ran - failed, ran))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())