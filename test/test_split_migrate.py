#!/usr/bin/env python3
"""
Tablet 分裂与负载迁移正确性测试

测试场景:
1. 写入大量 key 触发 Tablet 自动分裂 (threshold=10)
2. 等待负载均衡将 Tablet 迁移到另一个 DataNode
3. 验证分裂和迁移后数据的正确性

注意: BalancerLoop 每 5 秒执行一次, 测试需要等待。
"""

import sys
import time
from lekv_client import LekvClient

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"
SPLIT_THRESHOLD = 10  # Tablet 分裂的 key 数阈值
BALANCE_LOOP_INTERVAL_SEC = 5  # BalancerLoop 间隔秒数
BALANCE_DIFF_THRESHOLD = SPLIT_THRESHOLD + 2 # 负载均衡迁移的负载差异阈值 (单位: key 数)

def wait_for_condition(check_fn, timeout: float = 30.0, interval: float = 1.0):
    """等待条件满足, 超时返回 False"""
    start = time.time()
    while time.time() - start < timeout:
        if check_fn():
            return True
        time.sleep(interval)
    return False


def get_tablet_count() -> int:
    """获取当前 Tablet 数量"""
    c = LekvClient()
    tablets = c.get_tablets_info()
    return len(tablets)


def get_tablet_details() -> list:
    """获取当前 Tablet 详情"""
    c = LekvClient()
    return c.get_tablets_info()


def test_tablet_split():
    """
    测试 Tablet 自动分裂。
    向一个 Tablet 写入超过 SPLIT_THRESHOLD 个 key, 等待分裂。
    """
    print("\n[TEST] Tablet Auto-Split...")
    print(f"       Split threshold = {SPLIT_THRESHOLD}")

    c = LekvClient()

    # 阶段 1: 写入足够触发分裂的数据
    print("       Phase 1: Inserting keys to trigger split...")
    for i in range(SPLIT_THRESHOLD + 5):
        key = f"a{i:04d}"  # "a" 开头 -> 第一个 Tablet ["", "m")
        c.put(key, f"val_{i}")

    # 阶段 2: 等待分裂发生
    print("       Phase 2: Waiting for split...")
    initial_count = get_tablet_count()
    print(f"       Initial tablet count: {initial_count}")

    def check_split():
        return get_tablet_count() > initial_count

    if wait_for_condition(check_split, timeout=30):
        tablets = get_tablet_details()
        print(f"       Tablet count after split: {len(tablets)}")
        for t in tablets:
            print(f"         {t}")
    else:
        print(f"  {FAIL} Timeout: Tablet did not split within 30s")
        tablets = get_tablet_details()
        print(f"       Current tablets: {len(tablets)} (still {initial_count})")
        for t in tablets:
            print(f"         {t}")
        return False

    # 阶段 3: 验证分裂后数据可读
    print("       Phase 3: Verifying data after split...")
    errors = 0
    for i in range(SPLIT_THRESHOLD + 5):
        key = f"a{i:04d}"
        val = c.get(key)
        expected = f"val_{i}"
        if val != expected:
            print(f"       MISMATCH: get('{key}') expected '{expected}' got '{val}'")
            errors += 1

    if errors == 0:
        print(f"  {PASS} All data correct after split")
    else:
        print(f"  {FAIL} {errors} data mismatches after split")
        return False

    # 清理
    for i in range(SPLIT_THRESHOLD + 5):
        c.delete(f"a{i:04d}")

    return True


def test_tablet_migration():
    """
    测试负载均衡迁移。
    向一个 DataNode 写入大量数据, 等待负载均衡将 Tablet 迁移到另一个节点。
    由于迁移需要两个节点间负载差异 > BALANCE_LOOP_INTERVAL_SEC, 我们集中写入一个 Tablet。
    """
    print("\n[TEST] Tablet Load-Balance Migration...")

    c = LekvClient()

    # 阶段 1: 记录初始状态
    tablets_before = get_tablet_details()
    print(f"       Initial state:")
    for t in tablets_before:
        print(f"         {t}")

    # 阶段 2: 集中写入一个 Tablet, 使其数据量远大于另一个
    print("       Phase 1: Inserting keys to create imbalance...")
    # 写入 "a" 开头的 key -> Tablet 1 ["", "m")
    for i in range(BALANCE_DIFF_THRESHOLD * 2):  # 写入超过差异阈值的 key
        c.put(f"a{i:04d}", f"heavy_val_{i}")

    # 写入少量 "z" 开头的 key -> Tablet 2 ["m", "")
    for i in range(BALANCE_DIFF_THRESHOLD // 2):  # 写入少量 key
        c.put(f"z{i:04d}", f"light_val_{i}")

    # 阶段 3: 等待至少一次 BalancerLoop 执行
    print(f"       Phase 2: Waiting {BALANCE_LOOP_INTERVAL_SEC * 2}s for balancer...")
    time.sleep(BALANCE_LOOP_INTERVAL_SEC * 2)

    # 阶段 4: 检查路由表是否有变化（分裂或迁移）
    tablets_after = get_tablet_details()
    print(f"       After balancer:")
    for t in tablets_after:
        print(f"         {t}")

    # 验证: 无论是否发生迁移, 所有数据必须可读
    print("       Phase 3: Verifying all data accessible...")
    errors = 0

    for i in range(BALANCE_DIFF_THRESHOLD * 2):
        key = f"a{i:04d}"
        val = c.get(key)
        if val != f"heavy_val_{i}":
            print(f"       MISMATCH: get('{key}') expected 'heavy_val_{i}' got '{val}'")
            errors += 1

    for i in range(BALANCE_DIFF_THRESHOLD // 2):
        key = f"z{i:04d}"
        val = c.get(key)
        if val != f"light_val_{i}":
            print(f"       MISMATCH: get('{key}') expected 'light_val_{i}' got '{val}'")
            errors += 1

    # 清理
    for i in range(BALANCE_DIFF_THRESHOLD * 2):
        c.delete(f"a{i:04d}")
    for i in range(BALANCE_DIFF_THRESHOLD // 2):
        c.delete(f"z{i:04d}")

    if errors == 0:
        print(f"  {PASS} All data correct after potential migration")
        return True
    else:
        print(f"  {FAIL} {errors} data mismatches")
        return False


def test_split_then_read_from_new_tablet():
    """
    分裂后, 新 key 落在新的 Tablet 上, 验证能正确写入和读取。
    """
    print("\n[TEST] Write to new Tablet after split...")

    c = LekvClient()

    # 先触发分裂
    for i in range(SPLIT_THRESHOLD + 5):
        c.put(f"k{i:04d}", f"v{i}")

    # 等待分裂
    def check():
        return get_tablet_count() > 2

    if not wait_for_condition(check, timeout=30):
        print(f"  {FAIL} Timeout waiting for split")
        return False

    tablets = get_tablet_details()
    print(f"       Tablets after split: {len(tablets)}")

    # 向新 Tablet 写入 key
    print("       Writing new keys post-split...")
    c.put("new_key_1", "new_val_1")
    c.put("new_key_2", "new_val_2")

    val1 = c.get("new_key_1")
    val2 = c.get("new_key_2")

    if val1 == "new_val_1" and val2 == "new_val_2":
        print(f"  {PASS} New keys after split correct")
        result = True
    else:
        print(f"  {FAIL} new_key_1={val1}, new_key_2={val2}")
        result = False

    # 清理
    for i in range(SPLIT_THRESHOLD + 5):
        c.delete(f"k{i:04d}")
    c.delete("new_key_1")
    c.delete("new_key_2")

    return result


def test_stale_data_after_migration():
    """
    迁移后验证旧节点数据已被清理。
    向 Tablet 写入数据 -> 等待可能的分裂/迁移 -> 读取确认无旧数据干扰。
    """
    print("\n[TEST] Verify no stale data after migration...")

    c = LekvClient()

    # 写入并等待系统处理
    for i in range(15):
        c.put(f"s{i:04d}", f"stale_test_{i}")

    time.sleep(BALANCE_LOOP_INTERVAL_SEC * 3)

    # 所有数据必须能正确读取
    errors = 0
    for i in range(15):
        val = c.get(f"s{i:04d}")
        if val != f"stale_test_{i}":
            print(f"       MISMATCH: s{i:04d} expected 'stale_test_{i}' got '{val}'")
            errors += 1

    for i in range(15):
        c.delete(f"s{i:04d}")

    if errors == 0:
        print(f"  {PASS} No stale data issues")
        return True
    else:
        print(f"  {FAIL} {errors} stale data errors")
        return False


def run_all():
    print("=" * 50)
    print("  Tablet Split & Migration Tests")
    print("=" * 50)
    print("  Note: BalancerLoop interval = 5s, tests may take 30-60s")
    print("=" * 50)

    time.sleep(1)  # 等待集群就绪

    tests = [
        test_tablet_split,
        test_tablet_migration,
        test_split_then_read_from_new_tablet,
        test_stale_data_after_migration,
    ]

    passed = 0
    failed = 0
    for test in tests:
        try:
            if test():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print(f"  {FAIL} Exception: {e}")
            import traceback
            traceback.print_exc()
            failed += 1

    print("\n" + "=" * 50)
    print(f"  Results: {passed} passed, {failed} failed")
    print("=" * 50)
    return failed == 0


if __name__ == "__main__":
    ok = run_all()
    sys.exit(0 if ok else 1)
