#!/usr/bin/env python3
"""LEKV 系统测试脚本

用法:
    python3 test_lekv.py [all|basic|split|stats|perf|persist|route]
    python3 test_lekv.py --auto-start [all|basic|split|stats|perf|persist|route]
"""

import socket
import struct
import time
import random
import os
import sys

PASS = 0
FAIL = 0

# ==================== 进程管理 ====================
def wait_for_port(port, timeout=10):
    for i in range(timeout * 2):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(1)
            sock.connect(("127.0.0.1", port))
            sock.close()
            return True
        except:
            time.sleep(0.5)
    return False

# ==================== 帧协议工具 ====================
def make_frame(payload):
    frame_len = 10 + len(payload)
    frame = struct.pack('>I', frame_len)
    frame += b'\x4C\x01'
    frame += struct.pack('>I', 1)
    frame += payload
    return frame

def make_put(key, value):
    payload = bytes([0x03])
    payload += struct.pack('>H', len(key))
    payload += struct.pack('>I', len(value))
    payload += key.encode()
    payload += value.encode()
    return make_frame(payload)

def make_get(key):
    payload = bytes([0x02])
    payload += struct.pack('>H', len(key))
    payload += struct.pack('>I', 0)
    payload += key.encode()
    return make_frame(payload)

def make_delete(key):
    payload = bytes([0x04])
    payload += struct.pack('>H', len(key))
    payload += struct.pack('>I', 0)
    payload += key.encode()
    return make_frame(payload)

def make_get_route(key):
    payload = bytes([0x01])
    payload += struct.pack('>H', len(key))
    payload += key.encode()
    return make_frame(payload)

def make_shards():
    return make_frame(bytes([0x06]))

def recv_all(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("连接断开")
        data += chunk
    return data

def send_recv(sock, frame):
    sock.sendall(frame)
    header = recv_all(sock, 4)
    frame_len = struct.unpack('>I', header)[0]
    rest = recv_all(sock, frame_len - 4)
    magic = rest[0]
    assert magic == 0x4C, f"Magic 错误: {magic:02x}"
    payload = rest[6:]
    if len(payload) < 5:
        return (payload[0] if payload else 0xFF), b''
    status = payload[0]
    val_len = struct.unpack('>I', payload[1:5])[0]
    value = payload[5:5 + val_len] if val_len > 0 else b''
    return status, value

# 辅助：通过 Proxy 查路由
def query_route(key, proxy_port=9001):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", proxy_port))
    status, value = send_recv(sock, make_get_route(key))
    sock.close()
    if status != 0:
        return None
    tablet_id = value[0]
    epoch = struct.unpack('>I', value[1:5])[0]
    addr_len = struct.unpack('>H', value[5:7])[0]
    addr = value[7:7 + addr_len].decode()
    host, port = addr.split(':')
    return host, int(port), tablet_id, epoch

# 辅助：直连 DataNode 操作
def put_to(key, value, host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    status, _ = send_recv(sock, make_put(key, value))
    sock.close()
    return status

def get_from(key, host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    status, val = send_recv(sock, make_get(key))
    sock.close()
    return status, val.decode() if val else None

def delete_from(key, host, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    status, _ = send_recv(sock, make_delete(key))
    sock.close()
    return status

# 辅助：拉取路由表，返回 [(id, start, end, node_addr, key_count), ...]
def get_shards(proxy_port=9001):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", proxy_port))
    status, value = send_recv(sock, make_shards())
    sock.close()
    if status != 0:
        return []
    body = value.decode()
    parts = body.split()
    if not parts:
        return []
    tablets = []
    for t in parts[1:]:
        tp = t.split(':')
        if len(tp) >= 4:
            tablets.append({
                'id': tp[0],
                'start': tp[1],
                'end': tp[2],
                'addr': tp[3],
                'key_count': 0
            })
    return tablets

# ==================== 打印工具 ====================
def check(name, cond):
    global PASS, FAIL
    if cond:
        print(f"  [PASS] {name}")
        PASS += 1
    else:
        print(f"  [FAIL] {name}")
        FAIL += 1

# ==================== 测试用例 ====================
def test_route_detailed(proxy_port=9001):
    """测试1: 详细路由查询（调试用）"""
    print("\n" + "=" * 60)
    print("测试1: 路由查询详细测试")
    print("=" * 60)
    
    test_keys = ["apple", "mango", "a_key", "m_key", "zebra"]
    for key in test_keys:
        route = query_route(key)
        if route:
            host, port, tid, epoch = route
            print(f"        {key:12s} -> Tablet{tid} [{host}:{port}], epoch={epoch}")
        else:
            print(f"        {key:12s} -> 路由失败")

def test_basic_crud(proxy_port=9001):
    """测试2: 基本 CRUD"""
    print("\n" + "=" * 60)
    print("测试2: 基本功能测试 (CRUD)")
    print("=" * 60)
    
    test_data = {
        "apple":  "val_apple",
        "banana": "val_banana",
        "mango":  "val_mango",
        "peach":  "val_peach",
        "zebra":  "val_zebra",
    }
    
    print("\n[2.1] 写入 5 个 key-value")
    for key, val in test_data.items():
        route = query_route(key)
        if route is None:
            check(f"PUT {key} (路由查询)", False)
            continue
        host, port, tid, _ = route
        status = put_to(key, val, host, port)
        check(f"PUT {key} -> Tablet{tid}@{port}", status == 0)
    
    print("\n[2.2] 验证读取 GET")
    for key, expected_val in test_data.items():
        route = query_route(key)
        if route is None:
            check(f"GET {key}", False)
            continue
        host, port, _, _ = route
        status, actual_val = get_from(key, host, port)
        check(f"GET {key} = {actual_val}", status == 0 and actual_val == expected_val)
    
    print("\n[2.3] 验证删除 DELETE")
    key = "zebra"
    route = query_route(key)
    if route:
        host, port, _, _ = route
        status = delete_from(key, host, port)
        check(f"DELETE {key}", status == 0)
        
        status, val = get_from(key, host, port)
        check(f"GET {key} 返回 NOT_FOUND", status == 1)
    
    print("\n[2.4] 验证 SHARDS 全量路由表")
    tablets = get_shards(proxy_port)
    check(f"SHARDS 返回 {len(tablets)} 个 Tablet", len(tablets) >= 2)
    for t in tablets:
        print(f"        Tablet {t['id']}: [{t['start'] or '\"\"'}, {t['end'] or '\"\"'}) -> {t['addr']}")


def test_performance(proxy_port=9001, n=1000):
    """测试3: 性能基准测试"""
    print("\n" + "=" * 60)
    print(f"测试3: 性能测试 (PUT/GET 各 {n} 次)")
    print("=" * 60)
    
    route = query_route("perf_0000")
    if route is None:
        print("        路由查询失败，性能测试跳过")
        return
    host, port, _, _ = route
    print(f"        目标 DataNode: {host}:{port}")
    
    keys = [f"perf_{i:04d}" for i in range(n)]
    
    print(f"\n[3.1] PUT 写入 {n} 条...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    t0 = time.time()
    for key in keys:
        status, _ = send_recv(sock, make_put(key, "x" * 100))
    t1 = time.time()
    put_qps = n / (t1 - t0)
    put_lat = (t1 - t0) * 1000 / n
    print(f"        PUT: {put_qps:.1f} QPS, {put_lat:.3f} ms/op")
    sock.close()
    
    print(f"\n[3.2] GET 读取 {n} 条...")
    random.shuffle(keys)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    t0 = time.time()
    for key in keys:
        status, _ = send_recv(sock, make_get(key))
    t1 = time.time()
    get_qps = n / (t1 - t0)
    get_lat = (t1 - t0) * 1000 / n
    print(f"        GET: {get_qps:.1f} QPS, {get_lat:.3f} ms/op")
    sock.close()
    
    with open("perf_result.txt", "w") as f:
        f.write(f"PUT_QPS={put_qps:.1f}\nPUT_LAT={put_lat:.3f}\n")
        f.write(f"GET_QPS={get_qps:.1f}\nGET_LAT={get_lat:.3f}\n")
    print("\n        性能数据已保存到 perf_result.txt")


def test_persistence():
    """测试4: 持久化测试"""
    print("\n" + "=" * 60)
    print("测试4: 持久化测试 (LevelDB 数据文件)")
    print("=" * 60)
    
    for node_id in [2, 3]:
        db_path = f"db_{node_id}"
        if os.path.exists(db_path):
            files = os.listdir(db_path)
            data_files = [f for f in files if f.endswith('.ldb') or f.endswith('.log')]
            check(f"DataNode {node_id} 数据文件 ({len(data_files)} 个)", len(data_files) > 0)
            print(f"        {db_path}/: {files[:6]}")
        else:
            check(f"DataNode {node_id} 数据目录存在", False)

# ==================== 主入口 ====================

def main():
    args = sys.argv[1:]
    auto_start = "--auto-start" in args
    if auto_start:
        args.remove("--auto-start")
    
    if "--help" in args or "-h" in args or not args:
        print("用法: python3 test_lekv.py [--auto-start] [测试名]")
        print("")
        print("测试名:")
        print("  all      - 全部测试")
        print("  route    - 路由查询（调试）")
        print("  basic    - 基本 CRUD")
        print("  perf     - 性能测试")
        print("  persist  - 持久化检查")
        print("")
        print("示例:")
        print("  python3 test_lekv.py --auto-start all")
        print("  python3 test_lekv.py route           # 只查路由")
        return
    
    test_name = args[0]
    
    print("=" * 60)
    print("LEKV 系统测试套件")
    print("=" * 60)
    
    if auto_start:
        if not wait_for_port(9001):
            print("[错误] Proxy 9001 未就绪")
            return
        print("[就绪] 开始测试\n")
    else:
        print("请确保服务端已启动: ./lekv 9002 &  ./lekv 9003 &  ./lekv &\n")
    
    global PASS, FAIL
    PASS = FAIL = 0
    
    try:
        if test_name in ("route", "all"):
            test_route_detailed()
        if test_name in ("basic", "all"):
            test_basic_crud()
        if test_name in ("perf", "all"):
            test_performance(n=1000)
        if test_name in ("persist", "all"):
            test_persistence()
    except Exception as e:
        print(f"\n测试异常: {e}")
        import traceback
        traceback.print_exc()
    
    print("\n" + "=" * 60)
    print(f"测试结果汇总: PASS={PASS}, FAIL={FAIL}")
    print("=" * 60)
    
    
    sys.exit(0 if FAIL == 0 else 1)

if __name__ == "__main__":
    main()
