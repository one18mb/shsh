#!/usr/bin/env python3
"""shsh 自动化测试统一入口（仅用标准库）。

加载 tests/ 下所有 test_*.py 模块，收集其中的 test_* 用例并执行；
公共驱动在 tests/harness.py。

运行:
  make test
  python3 tests/run_tests.py                      # 跑全部
  python3 tests/run_tests.py test_help            # 按用例名过滤
  python3 tests/run_tests.py test_protocol::x     # 按 模块::用例名 过滤

BIN 目录可用环境变量 SHSH_BIN_DIR 覆盖（供 ASan 构建复用同一套测试）。
"""

import os
import sys
import glob
import importlib.util
import traceback

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TESTS_DIR)


def _load_modules():
    mods = []
    for path in sorted(glob.glob(os.path.join(TESTS_DIR, "test_*.py"))):
        name = os.path.splitext(os.path.basename(path))[0]
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mods.append(mod)
    return mods


def _collect():
    cases = []
    for mod in _load_modules():
        for attr in sorted(vars(mod)):
            fn = getattr(mod, attr)
            if attr.startswith("test_") and callable(fn):
                cases.append((mod.__name__, attr, fn))
    return cases


def main():
    only = sys.argv[1:]
    cases = _collect()
    if not cases:
        print("未发现任何测试用例")
        return 1
    ran = failed = 0
    for modname, name, fn in cases:
        if only and (name not in only and modname not in only
                     and ("%s::%s" % (modname, name)) not in only):
            continue
        ran += 1
        try:
            fn()
            print("PASS %s" % name)
        except Exception:
            failed += 1
            print("FAIL %s" % name)
            traceback.print_exc()
    print("\n%d/%d 通过" % (ran - failed, ran))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
