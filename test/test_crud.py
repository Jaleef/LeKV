#!/usr/bin/env python3
"""
CRUD 正确性测试
覆盖: PUT / GET / DELETE / 空key读取 / 重复PUT / 删除后再GET
"""

import sys
import time
import random
from lekv_client import LekvClient

PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"


def gen_key(prefix: str, idx: int) -> str:
    """生成一个有序的 key, 前缀 + 序号"""
    # 用 a-z 前缀确保 key 分布在不同 Tablet
    return f"{prefix}{idx:04d}"


def test_basic_put_get():
    """测试基本 PUT 和 GET"""
    print("\n[TEST] Basic PUT/GET...")
    c = LekvClient()

    c.put("hello", "world")
    val = c.get("hello")
    assert val == "world", f"Expected 'world', got '{val}'"
    print(f"  {PASS} put('hello', 'world') -> get('hello') = 'world'")

    # 覆盖写入
    c.put("hello", "world2")
    val = c.get("hello")
    assert val == "world2", f"Expected 'world2', got '{val}'"
    print(f"  {PASS} Overwrite put('hello', 'world2') -> get('hello') = 'world2'")

    # 空值 value
    c.put("empty_val", "")
    val = c.get("empty_val")
    assert val == "", f"Expected empty string, got '{val}'"
    print(f"  {PASS} put('empty_val', '') -> get = ''")

    c.delete("hello")
    c.delete("empty_val")


def test_get_not_found():
    """测试 GET 不存在的 key"""
    print("\n[TEST] GET non-existent key...")
    c = LekvClient()

    val = c.get("nonexistent_key_12345")
    assert val is None, f"Expected None, got '{val}'"
    print(f"  {PASS} get('nonexistent_key_12345') = None")


def test_delete():
    """测试 DELETE"""
    print("\n[TEST] DELETE...")
    c = LekvClient()

    c.put("del_test", "to_be_deleted")
    assert c.get("del_test") == "to_be_deleted"

    ok = c.delete("del_test")
    assert ok, "DELETE should return OK"
    val = c.get("del_test")
    assert val is None, f"After delete, get should return None, got '{val}'"
    print(f"  {PASS} delete('del_test') -> get = None")

    # 删除不存在的 key
    ok = c.delete("del_nonexistent")
    assert ok, "DELETE non-existent key should still return OK (idempotent)"
    print(f"  {PASS} delete non-existent key returns OK")


def test_cross_tablet_keys():
    """
    测试 key 分布在不同 Tablet 上。
    初始 Tablet: ["", "m") 和 ["m", "").
    """
    print("\n[TEST] Keys across Tablets...")
    c = LekvClient()

    # key < "m" -> Tablet 1
    c.put("aaa", "val_aaa")
    c.put("hello", "val_hello")
    c.put("lzz", "val_lzz")  # lzz < m

    # key >= "m" -> Tablet 2
    c.put("m000", "val_m000")
    c.put("zzz", "val_zzz")
    c.put("zzz999", "val_zzz999")

    # 验证所有 key 都能正确读取
    assert c.get("aaa") == "val_aaa"
    assert c.get("hello") == "val_hello"
    assert c.get("lzz") == "val_lzz"
    assert c.get("m000") == "val_m000"
    assert c.get("zzz") == "val_zzz"
    assert c.get("zzz999") == "val_zzz999"
    print(f"  {PASS} Keys on both sides of 'm' boundary correct")

    # 清理
    for k in ["aaa", "hello", "lzz", "m000", "zzz", "zzz999"]:
        c.delete(k)


def test_multiple_puts_batch():
    """批量写入并验证"""
    print("\n[TEST] Batch PUT/GET (20 keys)...")
    c = LekvClient()

    written = {}
    for i in range(20):
        prefix = "a" if i < 10 else "z"  # 确保分布在两个 Tablet
        key = gen_key(prefix, i)
        val = f"value_{i}_{random.randint(1000,9999)}"
        c.put(key, val)
        written[key] = val

    # 验证
    errors = 0
    for key, expected in written.items():
        actual = c.get(key)
        if actual != expected:
            print(f"  MISMATCH: key={key} expected={expected} actual={actual}")
            errors += 1

    if errors == 0:
        print(f"  {PASS} All 20 keys verified")
    else:
        print(f"  {FAIL} {errors} mismatches")
        return False

    # 清理
    for key in written:
        c.delete(key)
    return True


def test_large_value():
    """测试大 value"""
    print("\n[TEST] Large value (4KB)...")
    c = LekvClient()

    large = "x" * (4 * 1024)
    c.put("big_key", large)
    val = c.get("big_key")
    assert val == large, f"Large value mismatch: len(expected)={len(large)}, len(actual)={len(val)}"
    print(f"  {PASS} 4KB value round-trip OK")
    c.delete("big_key")


def test_special_chars_key():
    """测试特殊字符 key"""
    print("\n[TEST] Special characters in key/value...")
    c = LekvClient()

    special_keys = [
        ("key_with_underscore", "val"),
        ("key123", "val456"),
        ("a", "single_char"),
        ("key-with-dash", "val"),
    ]
    for k, v in special_keys:
        c.put(k, v)
        assert c.get(k) == v, f"Mismatch for key '{k}'"
        c.delete(k)
    print(f"  {PASS} Special characters OK")


def run_all():
    print("=" * 50)
    print("  CRUD Correctness Tests")
    print("=" * 50)

    # 等待集群就绪
    time.sleep(1)

    tests = [
        test_basic_put_get,
        test_get_not_found,
        test_delete,
        test_cross_tablet_keys,
        test_multiple_puts_batch,
        test_large_value,
        test_special_chars_key,
    ]

    passed = 0
    failed = 0
    for test in tests:
        try:
            result = test()
            if result is False:
                failed += 1
            else:
                passed += 1
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
