#!/usr/bin/env python3
"""
性能测试: 吞吐量 & 延迟

测试场景:
1. PUT 吞吐量: 单线程连续写入 N 个 key, 计算 ops/sec
2. GET 吞吐量: 单线程连续读取 N 个 key, 计算 ops/sec
3. 混合负载: 50% PUT + 50% GET, 计算 ops/sec
4. P99 延迟: 记录每次操作的延迟分布
"""

import sys
import time
import random
import statistics
from concurrent.futures import ThreadPoolExecutor
from lekv_client import LekvClient

PASS = "\033[92mPASS\033[0m"
WARN = "\033[93mWARN\033[0m"

# ---- 配置 ----
WARMUP_COUNT = 100
PUT_COUNT = 1000
GET_COUNT = 1000
MIX_COUNT = 1000
NUM_THREADS = 4  # 并发线程数


def gen_key(idx: int) -> str:
    """生成 key, 均匀分布在两个初始 Tablet"""
    prefix = chr(ord('a') + idx % 26)
    return f"{prefix}{idx:06d}"


def gen_value(idx: int) -> str:
    """生成 value"""
    return f"v{idx}_{random.randint(1000,9999)}"


def warmup():
    """预热: 建立连接"""
    print(f"\n[WARMUP] {WARMUP_COUNT} operations...")
    c = LekvClient()
    for i in range(WARMUP_COUNT):
        key = gen_key(i)
        c.put(key, gen_value(i))
        c.get(key)
    # 清理预热数据
    for i in range(WARMUP_COUNT):
        c.delete(gen_key(i))
    print("         Done")


def benchmark_put(count: int) -> dict:
    """PUT 基准测试"""
    print(f"\n[BENCH] PUT x{count} (single-thread)...")
    c = LekvClient()

    latencies = []
    start = time.perf_counter()
    for i in range(count):
        key = gen_key(i + WARMUP_COUNT)
        val = gen_value(i)

        t0 = time.perf_counter()
        ok = c.put(key, val)
        t1 = time.perf_counter()

        if not ok:
            print(f"  PUT failed at i={i}")
            break
        latencies.append((t1 - t0) * 1000)  # ms

    elapsed = time.perf_counter() - start
    ops = count / elapsed

    return {
        "ops": ops,
        "total_ms": elapsed * 1000,
        "avg_ms": statistics.mean(latencies),
        "p50_ms": statistics.median(latencies),
        "p99_ms": sorted(latencies)[int(len(latencies) * 0.99)],
        "latencies": latencies,
    }


def benchmark_get(count: int) -> dict:
    """GET 基准测试"""
    print(f"\n[BENCH] GET x{count} (single-thread)...")

    # 先写入数据
    print("        Pre-loading keys...")
    c = LekvClient()
    keys = [gen_key(i + WARMUP_COUNT) for i in range(count)]
    for i, key in enumerate(keys):
        c.put(key, gen_value(i))

    # 再读取
    print("        Reading...")
    latencies = []
    start = time.perf_counter()
    for key in keys:
        t0 = time.perf_counter()
        val = c.get(key)
        t1 = time.perf_counter()

        if val is None:
            print(f"  GET returned None for key={key}")
        latencies.append((t1 - t0) * 1000)

    elapsed = time.perf_counter() - start
    ops = count / elapsed

    # 清理
    for key in keys:
        c.delete(key)

    return {
        "ops": ops,
        "total_ms": elapsed * 1000,
        "avg_ms": statistics.mean(latencies),
        "p50_ms": statistics.median(latencies),
        "p99_ms": sorted(latencies)[int(len(latencies) * 0.99)],
        "latencies": latencies,
    }


def benchmark_mixed(count: int) -> dict:
    """混合负载: 50% PUT + 50% GET"""
    print(f"\n[BENCH] MIXED (50% PUT + 50% GET) x{count}...")

    # 先预写入一半数据
    print("        Pre-loading...")
    c = LekvClient()
    for i in range(count // 2):
        c.put(gen_key(i + WARMUP_COUNT), gen_value(i))

    # 混合操作
    print("        Running mixed workload...")
    latencies = []
    start = time.perf_counter()
    for i in range(count):
        key = gen_key((i // 2) + WARMUP_COUNT)
        is_put = (i % 2 == 0)

        t0 = time.perf_counter()
        if is_put:
            c.put(key, gen_value(i))
        else:
            c.get(key)
        t1 = time.perf_counter()

        latencies.append((t1 - t0) * 1000)

    elapsed = time.perf_counter() - start
    ops = count / elapsed

    # 清理
    for i in range(count // 2):
        c.delete(gen_key(i + WARMUP_COUNT))

    return {
        "ops": ops,
        "total_ms": elapsed * 1000,
        "avg_ms": statistics.mean(latencies),
        "p50_ms": statistics.median(latencies),
        "p99_ms": sorted(latencies)[int(len(latencies) * 0.99)],
        "latencies": latencies,
    }


def benchmark_concurrent_put(count: int, num_threads: int) -> dict:
    """并发 PUT 测试"""
    print(f"\n[BENCH] PUT x{count} ({num_threads} threads)...")

    def worker(tid: int, n: int):
        c = LekvClient()
        local_lats = []
        for i in range(n):
            key = f"t{tid}k{i:05d}"
            t0 = time.perf_counter()
            c.put(key, f"v{i}")
            t1 = time.perf_counter()
            local_lats.append((t1 - t0) * 1000)
        return local_lats

    per_thread = count // num_threads
    start = time.perf_counter()

    all_lats = []
    with ThreadPoolExecutor(max_workers=num_threads) as ex:
        futures = [ex.submit(worker, tid, per_thread) for tid in range(num_threads)]
        for f in futures:
            all_lats.extend(f.result())

    elapsed = time.perf_counter() - start
    ops = count / elapsed

    # 清理
    def cleanup(tid, n):
        c = LekvClient()
        for i in range(n):
            try:
                c.delete(f"t{tid}k{i:05d}")
            except Exception:
                pass

    with ThreadPoolExecutor(max_workers=num_threads) as ex:
        for tid in range(num_threads):
            ex.submit(cleanup, tid, per_thread)

    return {
        "ops": ops,
        "total_ms": elapsed * 1000,
        "avg_ms": statistics.mean(all_lats),
        "p50_ms": statistics.median(all_lats),
        "p99_ms": sorted(all_lats)[int(len(all_lats) * 0.99)],
        "latencies": all_lats,
    }


def print_result(name: str, r: dict):
    """打印测试结果"""
    print(f"\n  {'─' * 40}")
    print(f"  {name}")
    print(f"  {'─' * 40}")
    print(f"  Throughput: {r['ops']:>10.1f} ops/sec")
    print(f"  Total time: {r['total_ms']:>10.1f} ms")
    print(f"  Avg latency: {r['avg_ms']:>9.2f} ms")
    print(f"  P50 latency: {r['p50_ms']:>9.2f} ms")
    print(f"  P99 latency: {r['p99_ms']:>9.2f} ms")


def run_all():
    print("=" * 50)
    print("  Performance Tests")
    print("=" * 50)

    time.sleep(1)  # 等待集群就绪
    warmup()

    # 1. PUT 单线程
    r_put = benchmark_put(PUT_COUNT)
    print_result("PUT Single-Thread", r_put)

    # 2. GET 单线程
    r_get = benchmark_get(GET_COUNT)
    print_result("GET Single-Thread", r_get)

    # 3. 混合负载
    r_mix = benchmark_mixed(MIX_COUNT)
    print_result("MIXED 50/50 Single-Thread", r_mix)

    # 4. PUT 多线程
    r_conc = benchmark_concurrent_put(PUT_COUNT, NUM_THREADS)
    print_result(f"PUT Concurrent ({NUM_THREADS} threads)", r_conc)

    # 汇总
    print("\n" + "=" * 50)
    print("  Summary")
    print("=" * 50)
    print(f"  PUT (1T):     {r_put['ops']:>8.1f} ops/s  |  P99={r_put['p99_ms']:.2f}ms")
    print(f"  GET (1T):     {r_get['ops']:>8.1f} ops/s  |  P99={r_get['p99_ms']:.2f}ms")
    print(f"  MIXED (1T):   {r_mix['ops']:>8.1f} ops/s  |  P99={r_mix['p99_ms']:.2f}ms")
    print(f"  PUT ({NUM_THREADS}T):    {r_conc['ops']:>8.1f} ops/s  |  P99={r_conc['p99_ms']:.2f}ms")
    print("=" * 50)

    return True


if __name__ == "__main__":
    run_all()
    sys.exit(0)
