#!/usr/bin/env python3
"""
LEKV 统一测试入口
用法:
  ./run_all_tests.py         # 运行全部测试
  ./run_all_tests.py crud    # 只运行 CRUD 测试
  ./run_all_tests.py split   # 只运行分裂/迁移测试
  ./run_all_tests.py perf    # 只运行性能测试
"""

import sys
import os
import subprocess
import time

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(SCRIPTS_DIR)


def run_test(name: str, cmd: list) -> bool:
    """运行一个测试脚本, 返回是否成功"""
    print("\n" + "=" * 60)
    print(f"  Running: {name}")
    print("=" * 60)
    start = time.time()
    try:
        result = subprocess.run(
            cmd, cwd=SCRIPTS_DIR, capture_output=False, text=True, timeout=120
        )
        elapsed = time.time() - start
        if result.returncode == 0:
            print(f"\n  [{name}] PASSED in {elapsed:.1f}s")
            return True
        else:
            print(f"\n  [{name}] FAILED (exit code {result.returncode}) in {elapsed:.1f}s")
            return False
    except subprocess.TimeoutExpired:
        print(f"\n  [{name}] TIMEOUT after 120s")
        return False
    except FileNotFoundError:
        print(f"\n  [{name}] SCRIPT NOT FOUND: {cmd[0]}")
        return False


def wait_for_cluster(timeout: float = 10.0) -> bool:
    """等待集群就绪（检查 9001 端口可连接）"""
    import socket
    start = time.time()
    while time.time() - start < timeout:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(1.0)
            sock.connect(("121.89.83.240", 9001))
            sock.close()
            return True
        except Exception:
            time.sleep(0.5)
    return False


def main():
    args = sys.argv[1:]

    # 默认运行全部
    run_crud = not args or "crud" in args
    run_split = not args or "split" in args
    run_perf = not args or "perf" in args

    print("=" * 60)
    print("  LEKV Test Suite")
    print("=" * 60)

    # 检查集群是否已运行
    if not wait_for_cluster(timeout=2.0):
        print("\n  ERROR: Cluster not running on 127.0.0.1:9001")
        print("  Please start the cluster first:")
        print("    ./start_cluster.sh")
        sys.exit(1)

    print(f"  Cluster detected at 127.0.0.1:9001")

    results = []

    if run_crud:
        ok = run_test("CRUD Test", [sys.executable, "test_crud.py"])
        results.append(("CRUD", ok))

    if run_split:
        ok = run_test("Split & Migrate Test", [sys.executable, "test_split_migrate.py"])
        results.append(("Split/Migrate", ok))

    if run_perf:
        ok = run_test("Performance Test", [sys.executable, "test_perf.py"])
        results.append(("Performance", ok))

    # 汇总
    print("\n" + "=" * 60)
    print("  Test Summary")
    print("=" * 60)
    total_pass = 0
    total_fail = 0
    for name, ok in results:
        status = "\033[92mPASS\033[0m" if ok else "\033[91mFAIL\033[0m"
        print(f"  {name:<20} {status}")
        if ok:
            total_pass += 1
        else:
            total_fail += 1

    print("-" * 60)
    print(f"  Total: {total_pass} passed, {total_fail} failed")
    print("=" * 60)

    sys.exit(0 if total_fail == 0 else 1)


if __name__ == "__main__":
    main()
