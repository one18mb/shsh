#!/usr/bin/env python3
"""shsh 测试公共层（仅用标准库）。

提供驱动真实 shshd / shsh 进程的类与工具：
  Proc        —— 子进程 + 后台 drain 线程（避免管道死锁）
  TtyClient   —— stdin/stdout 接到 pty（真实终端语义）
  Screen      —— 极简终端模拟（行编辑渲染断言，跳过 CSI/OSC）
  RawClient   —— 裸 AF_UNIX 文本客户端（首行握手 + OSC 633 扫描），
                 用于统一文本协议的健壮性用例
以及 start_server / join / submit / wait_socket / kill_all / run 等助手。

BIN 目录可用环境变量 SHSH_BIN_DIR 覆盖（供 ASan 构建复用同一套测试）。
"""

import os
import re
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

# ---- 统一文本协议：OSC 633 带内标记（见 src/termcode.h） ----
OSC = b"\x1b]633;"
ST = b"\x1b\\"
BEL = b"\x07"

# 完整 633 标记：ESC ] 633 ; body (ST|BEL)；body 内不含 ESC/BEL
MARK_RE = re.compile(rb"\x1b\]633;([^\x07\x1b]*)(?:\x1b\\|\x07)")
# 任意 OSC 序列（含非 633，如窗口标题），用于剥成可见文本
OSC_ANY_RE = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x1b\\|\x07)")


def mark(code, **fields):
    """构造 OSC 633 标记，如 mark('D', s=0, u='alice', w='shsh')。"""
    body = code
    for k, v in fields.items():
        body += ";%s=%s" % (k, v)
    return OSC + body.encode() + ST


def wmark(rows, cols):
    """客户端→服务端的终端尺寸标记。"""
    return OSC + b"W;%d;x%d" % (rows, cols) + ST


def parse_marks(data):
    """提取字节流里全部完整 633 标记：[(code, {字段: 字符串值})]。
    跨读的半截标记天然匹配不到，等下一块拼齐即可。"""
    out = []
    for m in MARK_RE.finditer(data):
        body = m.group(1).decode(errors="replace")
        code = body[0] if body else ""
        fields = {}
        positional = []
        for seg in body[1:].split(";"):
            if not seg:
                continue  # 起始分号/空段
            if "=" in seg:
                k, v = seg.split("=", 1)
                fields[k] = v
            else:
                positional.append(seg)
        # C 等标记的裸参数（如 C;1 的退出码）放在 fields[""] 里
        if positional:
            fields[""] = positional[0]
        out.append((code, fields))
    return out


def strip_marks(data):
    """剥除所有 OSC 序列，返回可见字节（CSI 等其它转义仍保留）。"""
    return OSC_ANY_RE.sub(b"", data)

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

    def vtext(self):
        """剥除 OSC（633 标记等）后的可见文本（CR 也归一化去掉），
        供断言"命令输出与下一枚提示符相邻"这类可见终端行为。"""
        with self.lock:
            return self._norm(strip_marks(bytes(self.out))).decode(
                errors="replace")

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
                if s[i + 1:i + 2] == "]":
                    # OSC（含 OSC 633 标记）：跳到 ST(ESC \) 或 BEL
                    j = i + 2
                    while j < len(s) and s[j] != "\x07":
                        if s[j] == "\x1b" and s[j + 1:j + 2] == "\\":
                            j += 1
                            break
                        j += 1
                    i = j + 1
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
            [bin_path("shsh"), "login", "-n", name, "-s", session],
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


class RawClient:
    """统一文本协议的裸 AF_UNIX 客户端：首行 name/list 握手，之后是纯键入
    字节流；OSC 633 W 标记上报尺寸，下行流用 parse_marks 扫描。
    用于注入半截握手/畸形名/半截标记等健壮性用例。"""

    def __init__(self, session, timeout=5.0):
        self.path = os.path.join(SOCKDIR, session + ".sock")
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(self.path)
        self.buf = bytearray()
        # 唯一读套接字的后台线程（多线程并发 recv/setsockopt 会竞态）
        self._reader_on = False
        self._eof = False

    def drain_start(self):
        """启动后台持续排空（大流量用例避免客户端积压）。hello 会自动启动。"""
        if self._reader_on:
            return
        self._reader_on = True
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            r = self._recv_some(1.0)
            if r is None:
                self._eof = True
                return

    def wait_closed(self, timeout=5.0):
        """等待服务端关闭到 EOF。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._eof:
                return True
            if not self._reader_on:
                self._recv_some(0.05)
            else:
                time.sleep(0.05)
        return self._eof

    def send_raw(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.sock.sendall(data)

    def hello(self, name, rows=24, cols=80, drain=True):
        """首行文本握手 + 初始尺寸标记。
        drain=True（默认）启动后台持续读取；drain=False 时保持"不读"状态，
        供背压用例制造客户端积压（wait_* 仍会按需短暂读取）。"""
        self.send_raw(name.encode() + b"\n")
        self.winch(rows, cols)
        if drain:
            self.drain_start()

    def raw_input(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.send_raw(data)

    def winch(self, rows, cols):
        self.send_raw(wmark(rows, cols))

    def list(self):
        self.send_raw(b"list\n")

    def shutdown_write(self):
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    def _recv_some(self, timeout):
        """读一块进 buf；False=超时，None=EOF/连接已关闭。"""
        try:
            self.sock.settimeout(timeout)
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return False
        except OSError:
            return None
        if not chunk:
            return None
        self.buf.extend(chunk)
        return True

    def drain(self, seconds):
        """持续等待 seconds 秒，返回收到的全部可见字节（EOF 提前结束）。"""
        deadline = time.time() + seconds
        while time.time() < deadline and not self._eof:
            if self._reader_on:
                time.sleep(0.05)
            elif self._recv_some(0.05) is None:
                break
        return self.visible()

    def gather(self, seconds=1.5, stop=None):
        """等待若干秒；stop（字节或字符串）出现在可见输出中即提前结束。"""
        if isinstance(stop, str):
            stop = stop.encode()
        deadline = time.time() + seconds
        while time.time() < deadline and not self._eof:
            if stop and stop in self.visible():
                break
            if self._reader_on:
                time.sleep(0.05)
            elif self._recv_some(0.05) is None:
                break
        return self.visible()

    def visible(self):
        """已收字节剥除全部 OSC 序列后的可见文本。"""
        return strip_marks(bytes(self.buf))

    def marks(self):
        return parse_marks(bytes(self.buf))

    def wait_mark(self, code=None, fields=None, timeout=5.0):
        """等到匹配的 633 标记返回其字段字典；code 可传字符串/集合；
        fields 为要求的字段子集。超时返回 None。"""
        if isinstance(code, str):
            codes = {code}
        elif code is None:
            codes = None
        else:
            codes = set(code)
        deadline = time.time() + timeout
        while time.time() < deadline:
            for c, f in self.marks():
                if codes is not None and c not in codes:
                    continue
                if fields and any(f.get(k) != v for k, v in fields.items()):
                    continue
                return f if fields is not None else (c, f)
            if self._eof:
                return None
            if self._reader_on:
                time.sleep(0.02)
            else:
                self._recv_some(0.05)
        return None

    def wait_visible(self, text, timeout=5.0):
        """等待可见文本（剥除 OSC 后）包含 text。"""
        if isinstance(text, str):
            text = text.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in self.visible():
                return True
            if self._eof:
                return False
            if self._reader_on:
                time.sleep(0.02)
            else:
                self._recv_some(0.05)
        return False

    def recv_until_eof(self, timeout=5.0):
        """读到 EOF，返回全部收到字节；超时返回 None。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            r = self._recv_some(max(0.05, deadline - time.time()))
            if r is None:
                return bytes(self.buf)
        return None

    def closed(self, timeout=2.0):
        """连接是否已被对端关闭（EOF）。后台 reader 开启时只轮询其 EOF 标志，
        绝不与 reader 并发 recv（setsockopt/recv 竞态）。"""
        if self._reader_on:
            return self.wait_closed(timeout)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.sock.settimeout(0.2)
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
        self._eof = True   # 通知后台 reader 收尾，避免 EBADF 噪声
        try:
            self.sock.close()
        except OSError:
            pass


def start_server(session="work"):
    d = Proc([bin_path("shshd"), "-s", session], "shshd")
    wait_socket(session)
    return d


def join(session, name):
    return Proc([bin_path("shsh"), "login", "-n", name, "-s", session], name)


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
