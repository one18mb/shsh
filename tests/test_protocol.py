"""统一文本协议（OSC 633）：首行握手、纯字节流、OSC W 尺寸标记、
633 相位标记（A/C/D/E/u=）、list 一次性文本查询。

直接以 RawClient 在 AF_UNIX 上注入字节，验证服务端不崩溃、行为符合预期。
"""

from harness import *  # noqa: F401,F403


# ---- 首行文本握手 ----

def test_protocol_handshake_partial_line():
    """首行拆包：只发半截名字（无换行）不转正、不断开；补全行后收到 D+PS1。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.send_raw(b"ali")
        time.sleep(0.3)
        assert not c.closed(timeout=0.5), "半截首行不应导致断开"
        c.send_raw(b"ce\n")
        m = c.wait_mark("D", fields={"u": "alice"}, timeout=5)
        assert m is not None, "握手完成后应收到带 u=alice 的 D 标记"
        assert c.wait_visible(ps1("alice"), timeout=5), c.visible()
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_handshake_crlf():
    """首行容忍 CRLF 行尾。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.send_raw(b"carol\r\n")
        m = c.wait_mark("D", fields={"u": "carol"}, timeout=5)
        assert m is not None
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_duplicate_name_rejected():
    """第二个连接用同名首行 → E 标记 "<name> 已存在" 并 EOF。"""
    clear_sockdir()
    d = start_server("work")
    a = b = None
    try:
        a = RawClient("work")
        a.hello("dup")
        assert a.wait_mark("D", fields={"u": "dup"}) is not None
        b = RawClient("work")
        b.send_raw(b"dup\n")
        mk = b.wait_mark("E", timeout=5)
        assert mk is not None, "重名应收到 E 标记"
        _, fields = mk
        assert "已存在" in fields.get("reason", ""), fields
        assert b.closed(timeout=2), "E 后应关闭连接"
    finally:
        if a:
            a.close()
        if b:
            b.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_name_length_boundary():
    """名字 64B 通过；65B / 非法字符 / 空名 → E 标记拒绝 + EOF。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.send_raw(b"a" * 64 + b"\n")
        assert c.wait_mark("D") is not None
        c.close()

        c = RawClient("work")
        c.send_raw(b"b" * 65 + b"\n")
        mk = c.wait_mark("E", timeout=5)
        assert mk is not None and "非法" in mk[1].get("reason", ""), mk
        c.close()

        c = RawClient("work")
        c.send_raw(b"bad/name\n")
        mk = c.wait_mark("E", timeout=5)
        assert mk is not None and "非法" in mk[1].get("reason", ""), mk
        c.close()

        c = RawClient("work")
        c.send_raw(b"\n")
        mk = c.wait_mark("E", timeout=5)
        assert mk is not None and "非法" in mk[1].get("reason", ""), mk
        c.close()

        c = RawClient("work")
        c.send_raw(b"shshd\n")
        mk = c.wait_mark("E", timeout=5)
        assert mk is not None and "已存在" in mk[1].get("reason", ""), mk
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_overlong_first_line_rejected():
    """迟迟不发换行且首行超长 → E 拒绝，不崩。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.send_raw(b"x" * 100)  # 无换行
        mk = c.wait_mark("E", timeout=5)
        assert mk is not None and "非法" in mk[1].get("reason", ""), mk
        assert c.closed(timeout=2)
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_bytes_split_across_packets():
    """命名后的键入拆成任意小片段发送，命令照常执行（纯文本流，可任意切分）。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("dave")
        assert c.wait_mark("D", fields={"u": "dave"}) is not None
        raw = b"echo SPLIT_OK\n"
        for i in range(0, len(raw), 3):
            c.send_raw(raw[i:i + 3])
            time.sleep(0.03)
        assert c.wait_visible("SPLIT_OK", timeout=5), c.visible()
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_arbitrary_bytes_are_input():
    """命名后发来的任意字节（含 0x00/0x01）一律按键入处理：无换行只是半行，
    连接保持不中断，补换行后继续执行后续命令。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("erin")
        assert c.wait_mark("D", fields={"u": "erin"}) is not None
        c.send_raw(b"\x00\x01\x02garbage-ish bytes")  # 无换行：只是半行
        time.sleep(0.3)
        assert not c.closed(timeout=0.5)
        c.send_raw(b"\n")                      # 提交（命令失败也无所谓）
        c.send_raw(b"echo STILL_ALIVE\n")
        assert c.wait_visible("STILL_ALIVE", timeout=5), c.visible()
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_buffered_input_without_newline():
    """已命名连接发来无换行的半截键入：缓冲等待，不断连；补换行后作为命令执行。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("gina")
        assert c.wait_mark("D", fields={"u": "gina"}) is not None
        c.send_raw(b"echo PEND")
        time.sleep(0.3)
        assert not c.closed(timeout=0.5)
        c.send_raw(b"ING_OK\n")
        assert c.wait_visible("PENDING_OK", timeout=5), c.visible()
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


# ---- list 一次性纯文本查询 ----

def test_protocol_list_query():
    """发 list 行 → 纯文本块到 EOF（无任何 633 标记）。"""
    clear_sockdir()
    d = start_server("work")
    a = None
    try:
        a = RawClient("work")
        a.hello("alice")
        assert a.wait_mark("D", fields={"u": "alice"}) is not None
        time.sleep(0.2)

        c = RawClient("work")
        c.list()
        data = c.recv_until_eof(timeout=5)
        assert data is not None, "list 应在应答后 EOF"
        text = data.decode()
        assert text.startswith("work\n活动用户:\nalice\n"), repr(text)
        assert "所有用户:\nshshd alice\n" in text, repr(text)
        assert b"\x1b]" not in data, "list 响应不应含转义序列"
        c.close()
    finally:
        if a:
            a.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_list_idempotent():
    """list 不改变会话成员：查询连接关闭后执行者仍可执行命令。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("alice")
        assert c.wait_mark("D", fields={"u": "alice"}) is not None
        q = RawClient("work")
        q.list()
        assert q.recv_until_eof(timeout=5) is not None
        q.close()
        c.send_raw(b"echo AFTER_LIST\n")
        assert c.wait_visible("AFTER_LIST", timeout=5), c.visible()
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


# ---- OSC 633 相位标记 ----

def test_osc633_command_phase_markers():
    """命令流标记顺序：A → 输出 → C;0 → D;s=0;u=<执行者>。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("alice")
        assert c.wait_mark("D", fields={"u": "alice"}) is not None
        c.send_raw(b"echo HI_633\n")
        assert c.wait_visible("HI_633", timeout=5), c.visible()

        def have_sequence():
            raw = bytes(c.buf)
            ia = raw.find(mark("A"))
            # A 之前的 "HI_633" 是键入回显（echo HI_633）；从 A 之后找的才是输出
            ih = raw.find(b"HI_633", ia + len(mark("A"))) if ia >= 0 else -1
            ic = raw.find(OSC + b"C;0" + ST)
            # 命令前的首枚 D 也是 s=0：D 从 C 之后找
            idp = raw.find(OSC + b"D;s=0;u=alice;", ic if ic >= 0 else 0)
            return ia >= 0 and ih >= 0 and ic >= 0 and idp >= 0 and \
                ia < ih < ic < idp

        deadline = time.time() + 5
        while time.time() < deadline and not have_sequence():
            time.sleep(0.05)
        assert have_sequence(), repr(bytes(c.buf))
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_osc633_exit_code_in_c_and_d():
    """false 后 C;1 与 D;s=1 携带退出码；成功命令回 0。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("alice")
        assert c.wait_mark("D", fields={"u": "alice"}) is not None
        c.send_raw(b"false\n")
        # C 标记的退出码是裸参数：ESC]633;C;1ST
        assert c.wait_mark("C", fields={"": "1"}, timeout=5) is not None
        assert c.wait_mark("D", fields={"s": "1", "u": "alice"}, timeout=5) is not None
        c.send_raw(b"true\n")
        assert c.wait_mark("C", fields={"": "0"}, timeout=5) is not None
        assert c.wait_mark("D", fields={"s": "0", "u": "alice"}, timeout=5) is not None
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_osc633_executor_change_via_to():
    """执行权移交靠 D 标记的 u=：跟随者加入见 u=alice；to bob 后见 u=bob。"""
    clear_sockdir()
    d = start_server("work")
    a = b = None
    try:
        a = RawClient("work")
        a.hello("alice")
        assert a.wait_mark("D", fields={"u": "alice"}) is not None
        b = RawClient("work")
        b.hello("bob")
        # 跟随者零历史单播：D 里 u=alice
        assert b.wait_mark("D", fields={"u": "alice"}, timeout=5) is not None

        a.send_raw(b"to bob\n")
        assert b.wait_mark("D", fields={"u": "bob"}, timeout=5) is not None
        assert a.wait_mark("D", fields={"u": "bob"}, timeout=5) is not None

        # bob 现在可执行
        b.send_raw(b"echo BOB_RULES\n")
        assert b.wait_visible("BOB_RULES", timeout=5), b.visible()
        # alice 的键入被丢弃，不进共享行
        a.send_raw(b"echo ALICE_MUTED\n")
        time.sleep(0.6)
        assert not b.wait_visible("ALICE_MUTED", timeout=0.5), b.visible()
    finally:
        if a:
            a.close()
        if b:
            b.close()
        kill_all([d])
        clear_sockdir()


def test_osc633_e_on_shutdown():
    """SIGTERM 关停：所有客户端先收到 E;reason=会话已结束，再读到 EOF。"""
    clear_sockdir()
    d = start_server("work")
    a = b = None
    try:
        a = RawClient("work")
        a.hello("alice")
        assert a.wait_mark("D") is not None
        b = RawClient("work")
        b.hello("bob")
        assert b.wait_mark("D") is not None

        d.kill(signal.SIGTERM)
        for c in (a, b):
            mk = c.wait_mark("E", timeout=5)
            assert mk is not None, "应收到 E 标记"
            assert "会话已结束" in mk[1].get("reason", ""), mk[1]
            assert c.closed(timeout=3), "E 后应 EOF"
        assert d.wait() == 0
        assert not os.path.exists(os.path.join(SOCKDIR, "work.sock"))
    finally:
        kill_all([d])
        clear_sockdir()


# ---- OSC W 尺寸标记 ----

def test_osc633_winch_marker():
    """执行者发 W 标记：共享 pty 改尺寸；标记字节绝不进 pty 输入/回显。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("alice", rows=24, cols=80)
        assert c.wait_mark("D", fields={"u": "alice"}) is not None
        c.winch(33, 100)
        time.sleep(0.3)
        c.send_raw(b"stty size\n")
        assert c.wait_visible("33 100", timeout=5), c.visible()
        assert b"633;W;" not in bytes(c.buf), "W 标记泄漏进输出流"
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_osc633_winch_split_across_reads():
    """W 标记拆在任意边界（含 ESC 与 ST 半截）仍被正确剥离识别。"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = RawClient("work")
        c.hello("alice")
        assert c.wait_mark("D", fields={"u": "alice"}) is not None
        w = wmark(28, 82)
        # 从不同切点拆三段
        for cut in (1, 2, len(w) - 1):
            pass
        c.send_raw(w[:1])
        time.sleep(0.1)
        c.send_raw(w[1:4])
        time.sleep(0.1)
        c.send_raw(w[4:-2])
        time.sleep(0.1)
        c.send_raw(w[-2:])
        time.sleep(0.3)
        c.send_raw(b"stty size\n")
        assert c.wait_visible("28 82", timeout=5), c.visible()
        assert b"633;W;" not in bytes(c.buf)
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_osc633_plain_client_strips_markers():
    """shsh --plain：剥除全部 633 标记，只留可见文本。"""
    clear_sockdir()
    d = start_server("work")
    p = None
    try:
        p = Proc([bin_path("shsh"), "login", "-n", "alice", "-s", "work",
                  "--plain"], "plain")
        assert p.wait_contains(ps1("alice"), timeout=5), p.text()
        p.send("echo PLAIN_OK\n")
        assert p.wait_contains("PLAIN_OK", timeout=5), p.text()
        time.sleep(0.3)
        assert b"\x1b]633" not in p.outbytes(), p.outbytes()
    finally:
        if p:
            p.kill()
        kill_all([d])
        clear_sockdir()
