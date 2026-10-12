#!/usr/bin/env python3
"""shsh 测试公共层（仅用标准库）。

提供驱动真实 shshd / shsh 进程的类与工具：
  Proc        —— 子进程 + 后台 drain 线程（避免管道死锁）
  TtyClient   —— stdin/stdout 接到 pty（真实终端语义）
  Screen      —— 极简终端模拟（行编辑渲染断言）
  ProtoClient —— 裸 AF_UNIX 客户端，用于协议健壮性测试
以及 start_server / join / submit / wait_socket / kill_all / run 等助手。

BIN 目录可用环境变量 SHSH_BIN_DIR 覆盖（供 ASan 构建复用同一套测试）。
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
BIN = os.environ.get("SHSH_BIN_DIR") or os.path.join(ROOT, "bin")
SOCKDIR = "/tmp/shsh"

# 协议常量（见 src/proto.h）
C2S_HELLO = 1
C2S_RAW = 2
C2S_WINCH = 3
C2S_LIST = 4
S2C_WELCOME = 64
S2C_OUT = 65
S2C_RUN_BEGIN = 66
S2C_RUN_END = 67
S2C_ROLE = 68
S2C_NOTICE = 69
S2C_LISTRESP = 70
S2C_BYE = 71

SH_MSG_MAX = 1 << 20  # 单帧 payload 上限（src/common.h）

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


_sess_seq = [0]


def unique_session(tag="s"):
    """生成唯一且合法的会话名（避免各主题模块抢同一个 socket）。"""
    _sess_seq[0] += 1
    return "t-%s-%d-%d" % (tag, os.getpid(), _sess_seq[0])


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
        # 取 pty slave 为控制终端（新会话 + TIOCSCTTY），使 ISIG 生效：
        # 非执行者本地 ^C 才能由系统向其进程组投递 SIGINT（真实终端语义）
        def _ctty():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        self.p = subprocess.Popen(
            [bin_path("shsh"), name, session],
            stdin=slave, stdout=slave, stderr=slave, preexec_fn=_ctty)
        self.name = name
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
                                 % (self.name, text, self.norm()))

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


class ProtoClient:
    """裸协议客户端：直接读写 AF_UNIX 套接字与 6 字节帧（u32 BE 长度 +
    u16 BE 类型），用于注入畸形/半帧/超大帧等健壮性用例。"""

    HDR = 6

    def __init__(self, session, timeout=5.0):
        self.path = os.path.join(SOCKDIR, session + ".sock")
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(self.path)
        self.buf = bytearray()

    @staticmethod
    def frame(mtype, payload=b""):
        if isinstance(payload, str):
            payload = payload.encode()
        return struct.pack("!IH", len(payload), mtype) + payload

    def send_raw(self, data):
        self.sock.sendall(data)

    def send_frame(self, mtype, payload=b""):
        self.sock.sendall(self.frame(mtype, payload))

    def hello(self, name, rows=24, cols=80):
        nm = name.encode()
        payload = struct.pack("!B", len(nm)) + nm + struct.pack("!HH", rows, cols)
        self.send_frame(C2S_HELLO, payload)

    def raw_input(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.send_frame(C2S_RAW, data)

    def winch(self, rows, cols):
        self.send_frame(C2S_WINCH, struct.pack("!HH", rows, cols))

    def list(self):
        self.send_frame(C2S_LIST)

    def shutdown_write(self):
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    def recv_frame(self, timeout=None):
        """返回 (type, payload)，或 None（连接关闭 / 超时 / 出错）。"""
        if timeout is not None:
            self.sock.settimeout(timeout)
        while True:
            if len(self.buf) >= self.HDR:
                ln, typ = struct.unpack("!IH", bytes(self.buf[:self.HDR]))
                if len(self.buf) >= self.HDR + ln:
                    payload = bytes(self.buf[self.HDR:self.HDR + ln])
                    del self.buf[:self.HDR + ln]
                    return typ, payload
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, OSError):
                return None
            if not chunk:
                return None
            self.buf.extend(chunk)

    def recv_until(self, mtype, timeout=5.0):
        """读到指定类型帧返回其 payload，否则 None（超时/关闭）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            fr = self.recv_frame(timeout=max(0.05, deadline - time.time()))
            if fr is None:
                continue
            if fr[0] == mtype:
                return fr[1]
        return None

    def closed(self, timeout=2.0):
        """探测连接是否被对端关闭（读到 EOF/出错即视为已关闭）。"""
        self.sock.settimeout(timeout)
        try:
            chunk = self.sock.recv(4096)
        except socket.timeout:
            return False
        except OSError:
            return True
        if not chunk:
            return True
        self.buf.extend(chunk)
        return False

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def start_server(session="work"):
    d = Proc([bin_path("shshd"), session], "shshd")
    wait_socket(session)
    return d


def join(session, name):
    return Proc([bin_path("shsh"), name, session], name)


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
