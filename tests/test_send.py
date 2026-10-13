"""send（C 版同步命令通道）端到端测试。

模型：shsh 是唯一的 shshd 客户端，同时是 send 的本地端点。测试用
`shsh login -d` 起后台端点，用 `send` 注入命令读增量；另一方用户用
harness.RawClient 扮演。端点文件在 ~/.shsh/<会话>__<用户>.*
"""

import os
import subprocess
import time

from harness import *  # noqa: F401,F403


def _send_bin():
    return bin_path("send")


def _shsh_bin():
    return bin_path("shsh")


def _env():
    e = dict(os.environ)
    e.pop("SHSH_NAME", None)
    e.pop("SHSH_SESSION", None)
    return e


def endpoint(session, name, wait=6.0):
    """起后台端点，返回 Popen；等端点文件就绪。"""
    p = subprocess.Popen([_shsh_bin(), "login", "-n", name, "-s", session,
                          "-d"], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    pidf = os.path.expanduser("~/.shsh/%s__%s.pid" % (session, name))
    deadline = time.time() + wait
    while time.time() < deadline:
        if os.path.exists(pidf):
            return p
        time.sleep(0.1)
    raise AssertionError("端点未就绪: %s" % pidf)


def send(args, session=None, name=None, timeout=20):
    """同步调用 send；session/name 给出则加 -n/-s。"""
    argv = [_send_bin()]
    if name:
        argv += ["-n", name]
    if session:
        argv += ["-s", session]
    argv += args
    return subprocess.run(argv, env=_env(), capture_output=True, text=True,
                          timeout=timeout)


def send_popen(args, session, name):
    argv = [_send_bin(), "-n", name, "-s", session] + args
    return subprocess.Popen(argv, env=_env(), stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)


def stop_endpoint(session, name):
    subprocess.run([_shsh_bin(), "logout", "-n", name, "-s", session],
                   env=_env(), capture_output=True, text=True, timeout=10)


def test_send_basic_and_incremental():
    """run 有输出、默认吞掉自身回显；第二次 run 只返回增量。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        r1 = send(["echo SEND_ONE"], sess, "bot")
        assert r1.returncode == 0, r1.stderr
        assert "SEND_ONE" in r1.stdout, r1.stdout
        assert "echo SEND_ONE" not in r1.stdout, "默认应吞掉自身回显: %r" % r1.stdout
        r2 = send(["echo SEND_TWO"], sess, "bot")
        assert r2.returncode == 0, r2.stderr
        assert "SEND_TWO" in r2.stdout
        assert "SEND_ONE" not in r2.stdout, "第二次不应带出旧增量"
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_dash_l_keeps_echo():
    """-l：连自身命令回显一起返回。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        r = send(["-l", "echo WITH_ECHO"], sess, "bot")
        assert r.returncode == 0, r.stderr
        assert "WITH_ECHO" in r.stdout
        assert "echo WITH_ECHO" in r.stdout, "-l 应保留回显: %r" % r.stdout
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_multiline_script():
    """多行命令（含换行）整段注入，逐行执行，全部结束后返回。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        r = send(["echo L1\necho L2\necho L3"], sess, "bot")
        assert r.returncode == 0, r.stderr
        for n in ("L1", "L2", "L3"):
            assert n in r.stdout, r.stdout
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_multiline_compound():
    """单条跨行复合命令（行数 ≠ 顶层命令数）正确结束（P 标记驱动），
    且默认抑制续行提示符 PS2（'> '），只留命令输出。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        r = send(["if true; then\necho COMPOUND_OK\nfi"], sess, "bot")
        assert r.returncode == 0, r.stderr
        assert "COMPOUND_OK" in r.stdout, r.stdout
        # 默认省 token：续行提示符 "> " 不应出现
        assert "> " not in r.stdout, "PS2 未被抑制: %r" % r.stdout
        # -l 保留完整回显（含 PS2）
        r2 = send(["-l", "if true; then\necho ECHO_OK\nfi"], sess, "bot")
        assert r2.returncode == 0, r2.stderr
        assert "ECHO_OK" in r2.stdout
        assert "> " in r2.stdout, "-l 应保留 PS2: %r" % r2.stdout
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_stdin_dash():
    """-：从 stdin 读多行脚本。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        argv = [_send_bin(), "-n", "bot", "-s", sess, "-"]
        r = subprocess.run(argv, env=_env(), input="echo SS1\necho SS2\n",
                           capture_output=True, text=True, timeout=20)
        assert r.returncode == 0, r.stderr
        assert "SS1" in r.stdout and "SS2" in r.stdout, r.stdout
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_large_stdin_backpressure():
    """大于旧 1MiB 输出阀门的批量输入：靠端到端背压平滑跑完，端点不被断开。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        # 约 2MB 的真实输出命令（seq 到 ~20 万行，超 1MiB）
        script = "seq 1 200000\necho BIG_DONE\n"
        argv = [_send_bin(), "-n", "bot", "-s", sess, "-t", "60", "-"]
        r = subprocess.run(argv, env=_env(), input=script,
                           capture_output=True, text=True, timeout=90)
        assert r.returncode == 0, "rc=%s err=%r" % (r.returncode, r.stderr)
        assert "BIG_DONE" in r.stdout, r.stdout[-200:]
        # 端点仍存活（背压没有把它踢掉）
        assert os.path.exists(
            os.path.expanduser("~/.shsh/%s__bot.pid" % sess))
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_argv_kernel_limit_clean_error():
    """argv 超过内核单参数上限（~128KiB）：shell 直接 E2BIG，send 不应崩溃，
    返回非 0 且有错误（内核硬限，正确处理是"报错而非挂死/被杀"）。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        huge = "true " + ("x" * 200000)
        try:
            r = subprocess.run([_send_bin(), "-n", "bot", "-s", sess, huge],
                               env=_env(), capture_output=True, text=True,
                               timeout=15)
        except OSError as e:
            # 父 shell execve 即 E2BIG：send 根本没启动，属"内核硬限"范畴
            assert "Argument list too long" in str(e)
            return
        assert r.returncode != 0, "超长 argv 不应成功"
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_to_handoff():
    """to <对方>：完成条件是 D;u=对方。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    carol = None
    try:
        ep = endpoint(sess, "bot")
        carol = RawClient(sess)
        carol.hello("carol")
        assert carol.wait_mark("D", fields={"u": "bot"}) is not None

        r = send(["to carol"], sess, "bot")
        assert r.returncode == 0, r.stderr
        assert carol.wait_mark("D", fields={"u": "carol"}) is not None
    finally:
        if carol:
            carol.close()
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_waits_for_executor():
    """follower 身份发命令：阻塞等交权，拿到后自动投递。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    carol = None
    p = None
    try:
        ep = endpoint(sess, "bot")
        carol = RawClient(sess)
        carol.hello("carol")
        assert carol.wait_mark("D", fields={"u": "bot"}) is not None

        assert send(["to carol"], sess, "bot").returncode == 0
        assert carol.wait_mark("D", fields={"u": "carol"}) is not None

        p = send_popen(["echo WAITED_OK"], sess, "bot")  # bot 现为 follower
        time.sleep(1.0)
        assert p.poll() is None, "拿到执行权前不应返回"
        carol.send_raw(b"to bot\n")
        try:
            out, err = p.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            raise AssertionError("交权后 send 未返回")
        assert p.returncode == 0, err
        assert "WAITED_OK" in out
    finally:
        if p and p.poll() is None:
            p.kill()
        if carol:
            carol.close()
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_no_endpoint_errors():
    """端点不存在：send 报错，且不启动任何 shsh。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    try:
        r = send(["echo X"], sess, "ghost")
        assert r.returncode != 0
        assert "端点" in r.stderr
    finally:
        kill_all([d])
        clear_sockdir()


def test_send_default_binding_written():
    """成功后写回 ~/.shsh/current；随后不带 -n/-s 也能用。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        cur = os.path.expanduser("~/.shsh/current")
        try:
            os.remove(cur)
        except OSError:
            pass
        assert send(["echo BIND1"], sess, "bot").returncode == 0
        assert os.path.exists(cur)
        # 不带 -n/-s（且无环境变量）→ 读 current
        r = send(["echo BIND2"])
        assert r.returncode == 0, r.stderr
        assert "BIND2" in r.stdout
    finally:
        stop_endpoint(sess, "bot")
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_shsh_logout_stops_endpoint():
    """shsh logout：停端点、清文件。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        base = os.path.expanduser("~/.shsh/%s__%s" % (sess, "bot"))
        assert os.path.exists(base + ".pid")
        stop_endpoint(sess, "bot")
        time.sleep(0.3)
        for suf in (".in", ".out", ".cursor", ".pid", ".log"):
            assert not os.path.exists(base + suf), suf
    finally:
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()


def test_send_session_shutdown():
    """会话关停后端点消失：send 报错退出。"""
    clear_sockdir()
    sess = unique_session("send")
    d = start_server(sess)
    ep = None
    try:
        ep = endpoint(sess, "bot")
        d.kill(signal.SIGTERM)
        time.sleep(0.5)
        r = send(["echo X"], sess, "bot")
        assert r.returncode != 0
    finally:
        if ep:
            ep.kill()
        kill_all([d])
        clear_sockdir()