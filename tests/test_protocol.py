"""协议健壮性：畸形/截断/超大/半帧/未知类型/状态机/名字边界。

直接以 ProtoClient 注入原始帧，验证服务端不崩溃、行为符合预期。
"""

from harness import *  # noqa: F401,F403


def _bye_text(payload):
    """S2C_BYE/NOTICE 的 payload = u8 长度 + 文本。"""
    if not payload:
        return ""
    n = payload[0]
    return payload[1:1 + n].decode(errors="replace")


def _gather(c, seconds=1.5, stop=None):
    """收集一段时间的 S2C_OUT 文本（用于跨帧搜索输出）。"""
    out = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        fr = c.recv_frame(timeout=max(0.05, end - time.time()))
        if fr is None:
            continue
        t, p = fr
        if t == S2C_OUT:
            out.extend(p)
        if t == S2C_BYE:
            out.extend(b"\n[BYE] " + _bye_text(p).encode())
        if stop and stop in out:
            break
    return bytes(out)


def test_protocol_truncated_header_then_close():
    """只发 3 字节（半个头）→ 不崩、不误判；补足后连接仍可用、服务端存活"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        c.send_raw(b"\x00\x00\x00")     # 半个头
        time.sleep(0.3)
        assert not c.closed(timeout=0.5), "半个头不应导致断开"
        c.close()
        c = None
        # 服务端仍能接受新连接并完成握手
        c2 = ProtoClient("work")
        c2.hello("alice")
        assert c2.recv_until(S2C_WELCOME, timeout=5) is not None
        c2.close()
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_oversized_frame_rejected():
    """声明长度 > SH_MSG_MAX → 服务端回 BYE "协议错误" 并关闭"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        # 长度 = SH_MSG_MAX+1（越过上限，无需真的发 payload）
        c.send_raw(struct.pack("!IH", SH_MSG_MAX + 1, C2S_HELLO))
        payload = c.recv_until(S2C_BYE, timeout=5)
        assert payload is not None, "超大帧应触发 BYE"
        assert "协议错误" in _bye_text(payload), _bye_text(payload)
        assert c.closed(timeout=2), "协议错误后应关闭连接"
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_partial_frame_reassembled():
    """帧拆在 3/6/6+len 边界分多次 write → 跨读拼帧正确、命令照常执行"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        # 先把 HELLO 拆成两段（头 3 字节 + 其余）
        h = ProtoClient.frame(C2S_HELLO, b"\x04dave" + struct.pack("!HH", 24, 80))
        c.send_raw(h[:3])
        time.sleep(0.15)
        c.send_raw(h[3:])
        assert c.recv_until(S2C_WELCOME, timeout=5) is not None

        # 再把一条 RAW 命令拆成多段发送
        raw = ProtoClient.frame(C2S_RAW, b"echo SPLIT_OK\n")
        for i in range(0, len(raw), 5):
            c.send_raw(raw[i:i + 5])
            time.sleep(0.05)
        out = _gather(c, 3.0, stop=b"SPLIT_OK")
        assert b"SPLIT_OK" in out, "跨读拼帧后命令未执行: %r" % out
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_unknown_type_ignored():
    """未知 type → 静默忽略、连接存活、后续命令可用"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        c.hello("erin")
        assert c.recv_until(S2C_WELCOME, timeout=5) is not None
        c.send_frame(0x7777, b"garbage")   # 未知类型
        c.raw_input("echo STILL_ALIVE\n")
        out = _gather(c, 3.0, stop=b"STILL_ALIVE")
        assert b"STILL_ALIVE" in out, "未知类型后连接应仍可用: %r" % out
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_list_without_hello():
    """LIST 无需 HELLO：返回 LISTRESP 后关闭"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        c.list()
        payload = c.recv_until(S2C_LISTRESP, timeout=5)
        assert payload is not None, "应收到 LISTRESP"
        slen = payload[0]
        sess = payload[1:1 + slen].decode()
        assert sess == "work", "LISTRESP 会话名不符: %r" % sess
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_hello_twice_rejected():
    """已命名后再 HELLO → BYE "无效的 HELLO" 并关闭"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        c.hello("frank")
        assert c.recv_until(S2C_WELCOME, timeout=5) is not None
        c.hello("frank")
        payload = c.recv_until(S2C_BYE, timeout=5)
        assert payload is not None, "重复 HELLO 应被拒绝"
        assert "无效的 HELLO" in _bye_text(payload), _bye_text(payload)
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_duplicate_name_rejected():
    """第二个连接用同名 HELLO → BYE "<name> 已存在" """
    clear_sockdir()
    d = start_server("work")
    a = b = None
    try:
        a = ProtoClient("work")
        a.hello("dup")
        assert a.recv_until(S2C_WELCOME, timeout=5) is not None
        b = ProtoClient("work")
        b.hello("dup")
        payload = b.recv_until(S2C_BYE, timeout=5)
        assert payload is not None, "重名应被拒绝"
        assert "已存在" in _bye_text(payload), _bye_text(payload)
    finally:
        if a:
            a.close()
        if b:
            b.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_name_length_boundary():
    """名字 64B 通过；65B / 非法字符 / 空名 → 拒绝"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        # 64B 合法
        c = ProtoClient("work")
        c.hello("a" * 64)
        assert c.recv_until(S2C_WELCOME, timeout=5) is not None
        c.close()

        # 65B：超出 SH_NAME_MAX+1 缓冲 → 解析失败 "无效的 HELLO"
        c = ProtoClient("work")
        c.hello("b" * 65)
        p = c.recv_until(S2C_BYE, timeout=5)
        assert p is not None and "无效的 HELLO" in _bye_text(p), _bye_text(p)
        c.close()

        # 非法字符 "bad/name" → "非法的用户名"
        c = ProtoClient("work")
        c.hello("bad/name")
        p = c.recv_until(S2C_BYE, timeout=5)
        assert p is not None and "非法的用户名" in _bye_text(p), _bye_text(p)
        c.close()

        # 空名 → "非法的用户名"
        c = ProtoClient("work")
        c.hello("")
        p = c.recv_until(S2C_BYE, timeout=5)
        assert p is not None and "非法的用户名" in _bye_text(p), _bye_text(p)
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_pending_partial_frame():
    """声明大 payload 只发一半：服务端缓冲等待、不断连、不崩；补全后正常"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        c.hello("gina")
        assert c.recv_until(S2C_WELCOME, timeout=5) is not None
        # RAW 声明 1000 字节，只发 10 字节
        hdr = struct.pack("!IH", 1000, C2S_RAW)
        c.send_raw(hdr + b"0123456789")
        time.sleep(0.3)
        assert not c.closed(timeout=0.5), "半帧不应导致断开"
        # 补全余下 990 字节（无换行，不触发命令）
        c.send_raw(b"x" * 990)
        time.sleep(0.2)
        # 连接仍可用：发一条命令应能执行
        c.raw_input("echo PENDING_OK\n")
        out = _gather(c, 3.0, stop=b"PENDING_OK")
        assert b"PENDING_OK" in out, "补全后连接应仍可用: %r" % out
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()


def test_protocol_raw_before_hello_ignored():
    """HELLO 前 RAW → 无输出、无异常；随后 HELLO 正常"""
    clear_sockdir()
    d = start_server("work")
    c = None
    try:
        c = ProtoClient("work")
        c.raw_input("echo EARLY\n")
        time.sleep(0.4)
        # 未命名客户端不应有输出（也不应断开）
        assert not c.closed(timeout=0.5), "HELLO 前 RAW 不应导致断开"
        c.hello("hank")
        assert c.recv_until(S2C_WELCOME, timeout=5) is not None
        out = _gather(c, 1.0)
        assert b"EARLY" not in out, "HELLO 前的 RAW 不应被执行: %r" % out
    finally:
        if c:
            c.close()
        kill_all([d])
        clear_sockdir()
