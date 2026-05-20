#!/usr/bin/env python3
"""LEKV 系统测试脚本 - 功能测试 + 性能测试 + 分裂触发"""

import socket
import struct
import time
import random
import string
import subprocess
import os
import sys

# ==================== 帧协议工具 ====================

def make_frame(payload):
    """构造完整二进制帧: [4B FrameLen][1B Magic][1B Version][4B ReqID][Payload]"""
    frame_len = 10 + len(payload)
    frame = struct.pack('>I', frame_len)   # 大端 FrameLen
    frame += b'\x4C\x01'                    # Magic + Version
    frame += struct.pack('>I', 1)           # RequestID = 1
    frame += payload
    return frame

def make_put(key, value):
    """OP_PUT: [0x03][2B key_len][4B val_len][key][value]"""
    payload = bytes([0x03])
    payload += struct.pack('>H', len(key))
    payload += struct.pack('>I', len(value))
    payload += key.encode()
    payload += value.encode()
    return make_frame(payload)

def make_get(key):
    """OP_GET: [0x02][2B key_len][4B 0][key]"""
    payload = bytes([0x02])
    payload += struct.pack('>H', len(key))
    payload += struct.pack('>I', 0)
    payload += key.encode()
    return make_frame(payload)

def make_delete(key):
    """OP_DELETE: [0x04][2B key_len][4B 0][key]"""
    payload = bytes([0x04])
    payload += struct.pack('>H', len(key))
    payload += struct.pack('>I', 0)
    payload += key.encode()
    return make_frame(payload)

def make_get_route(key):
    """OP_GET_ROUTE: [0x01][2B key_len][key]"""
    payload = bytes([0x01])
    payload += struct.pack('>H', len(key))
    payload += key.encode()
    return make_frame(payload)

def make_shards():
    """OP_SHARDS: [0x06]"""
    return make_frame(bytes([0x06]))

def recv_all(sock, n):
    """从 socket 精确读取 n 字节"""
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("连接断开")
        data += chunk
    return data

def send_recv(sock, frame):
    """发送一帧，接收并解析响应"""
    sock.sendall(frame)
    # 读 4 字节 FrameLen
    header = recv_all(sock, 4)
    frame_len = struct.unpack('>I', header)[0]
    # 读剩余部分
    rest = recv_all(sock, frame_len - 4)
    # 解析: [Magic][Version][ReqID][Payload]
    magic, version = rest[0], rest[1]
    assert magic == 0x4C, f"Magic 错误: {magic:02x}"
    payload = rest[6:]  # 跳过 Magic(1) + Version(1) + ReqID(4)
    # Payload: [Status][4B val_len][value]
    if len(payload) < 5:
        return payload[0], b''
    status = payload[0]
    val_len = struct.unpack('>I', payload[1:5])[0]
    value = payload[5:5 + val_len]
    return status, value

# ==================== 打印工具 ====================

def status_str(s):
    names = {0: "OK", 1: "NOT_FOUND", 2: "NOT_MY_SHARD", 3: "BAD_REQUEST",
             4: "TIMEOUT", 5: "NO_SHARD", 6: "KEY_TOO_LONG", 7: "VALUE_TOO_LONG"}
    return names.get(s, f"UNKNOWN({s})")

PASS = 0
FAIL = 0

def check(name, cond):
    global PASS, FAIL
    if cond:
        print(f"  [PASS] {name}")
        PASS += 1
    else:
        print(f"  [FAIL] {name}")
        FAIL += 1

# ==================== 测试用例 ====================

def test_basic_crud(proxy_host="127.0.0.1", proxy_port=9001,
                    dn2_port=9002, dn3_port=9003):
    """测试1: 基本 CRUD + 路由查询"""
    print("\n" + "=" * 60)
    print("测试1: 基本功能测试 (CRUD + 路由查询)")
    print("=" * 60)
    
    # 1.1 通过 Proxy 查路由
    print("\n[1.1] 路由查询 GET_ROUTE")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((proxy_host, proxy_port))
    status, value = send_recv(sock, make_get_route("apple"))
    check("apple 路由查询返回 OK", status == 0)
    if status == 0 and len(value) >= 6:
        tablet_id = value[0]
        epoch = struct.unpack('>I', value[1:5])[0]
        addr_len = struct.unpack('>H', value[5:7])[0]
        addr = value[7:7 + addr_len].decode()
        print(f"       -> Tablet {tablet_id}, Epoch {epoch}, Addr {addr}")
    sock.close()

    # 1.2 查 SHARDS
    print("\n[1.2] 全量路由表 SHARDS")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((proxy_host, proxy_port))
    status, value = send_recv(sock, make_shards())
    check("SHARDS 返回 OK", status == 0)
    if status == 0:
        print(f"       -> {value.decode()}")
    sock.close()

    # 1.3 PUT 到 DataNode
    print("\n[1.3] 写入测试 PUT")
    test_keys = ["apple", "banana", "mango", "peach", "zebra"]
    for key in test_keys:
        # 先查路由找到目标 DataNode
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((proxy_host, proxy_port))
        status, value = send_recv(sock, make_get_route(key))
        sock.close()
        if status != 0:
            check(f"{key} 路由失败", False)
            continue
        addr_len = struct.unpack('>H', value[5:7])[0]
        addr = value[7:7 + addr_len].decode()
        host, port = addr.split(':')
        
        # 直连 DataNode PUT
        dsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        dsock.connect((host, int(port)))
        status, _ = send_recv(dsock, make_put(key, f"value_of_{key}"))
        check(f"PUT {key} -> {addr}", status == 0)
        dsock.close()

    # 1.4 GET 验证
    print("\n[1.4] 读取测试 GET")
    for key in test_keys:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((proxy_host, proxy_port))
        status, value = send_recv(sock, make_get_route(key))
        sock.close()
        if status != 0: continue
        addr_len = struct.unpack('>H', value[5:7])[0]
        addr = value[7:7 + addr_len].decode()
        host, port = addr.split(':')
        
        dsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        dsock.connect((host, int(port)))
        status, val = send_recv(dsock, make_get(key))
        check(f"GET {key} = {val.decode() if val else 'NOT_FOUND'}", 
              status == 0 and val.decode() == f"value_of_{key}")
        dsock.close()

    # 1.5 DELETE 测试
    print("\n[1.5] 删除测试 DELETE")
    key = "zebra"
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((proxy_host, proxy_port))
    status, value = send_recv(sock, make_get_route(key))
    sock.close()
    addr_len = struct.unpack('>H', value[5:7])[0]
    addr = value[7:7 + addr_len].decode()
    host, port = addr.split(':')
    dsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    dsock.connect((host, int(port)))
    status, _ = send_recv(dsock, make_delete(key))
    check(f"DELETE {key}", status == 0)
    dsock.close()
    
    # 验证已删除
    dsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    dsock.connect((host, int(port)))
    status, _ = send_recv(dsock, make_get(key))
    check(f"GET {key} 返回 NOT_FOUND", status == 1)  # ST_NOT_FOUND = 0x01
    dsock.close()


def test_auto_split(proxy_host="127.0.0.1", proxy_port=9001):
    """测试2: 自动分裂 - 往 Tablet 1 (["", "m")) 写入超过阈值的数据"""
    print("\n" + "=" * 60)
    print("测试2: 自动分裂测试 (SPLIT_THRESHOLD = 10)")
    print("说明: 向 Tablet 1 写入 15 个 key，触发 Balancer 分裂")
    print("=" * 60)
    
    # 查询初始路由表
    print("\n[2.1] 初始路由表")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((proxy_host, proxy_port))
    status, value = send_recv(sock, make_shards())
    sock.close()
    if status == 0:
        print(f"       {value.decode()}")
    
    # 写入 15 个以 a-l 开头的 key（落在 ["", "m") 内）
    letters = list(string.ascii_lowercase[:15])  # a b c d e f g h i j k l m
    print(f"\n[2.2] 批量写入 {len(letters)} 个 key")
    for ch in letters:
        key = f"{ch}_key"
        # 查路由
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((proxy_host, proxy_port))
        status, value = send_recv(sock, make_get_route(key))
        sock.close()
        if status != 0: 
            print(f"       路由失败: {key}")
            continue
        addr_len = struct.unpack('>H', value[5:7])[0]
        addr = value[7:7 + addr_len].decode()
        host, port = addr.split(':')
        
        dsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        dsock.connect((host, int(port)))
        status, _ = send_recv(dsock, make_put(key, f"val_{ch}"))
        if status == 0:
            print(f"       PUT {key} -> {addr} OK")
        else:
            print(f"       PUT {key} -> FAILED (status={status_str(status)})")
        dsock.close()
    
    print(f"\n[2.3] 等待 Balancer 执行 (约 10-15 秒)...")
    for i in range(15, 0, -1):
        print(f"       倒计时 {i}s", end='\r')
        time.sleep(1)
    print()
    
    # 查询分裂后的路由表
    print("\n[2.4] 分裂后的路由表")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((proxy_host, proxy_port))
    status, value = send_recv(sock, make_shards())
    sock.close()
    if status == 0:
        tablets = value.decode().split()
        print(f"       Tablet 数量: {tablets[0]}")
        for t in tablets[1:]:
            parts = t.split(':')
            if len(parts) >= 4:
                print(f"       Tablet {parts[0]}: [{parts[1] or '\"\"'}, {parts[2] or '\"\"'}) -> {parts[3]}")
        check("Tablet 数量 > 2 (已分裂)", int(tablets[0]) > 2)


def test_tablet_stats(proxy_host="127.0.0.1", proxy_port=9001):
    """测试3: TABLET_STATS 统计查询"""
    print("\n" + "=" * 60)
    print("测试3: Tablet 统计查询 (TABLET_STATS)")
    print("=" * 60)
    
    # 查询 Tablet 1 的统计 (OP_TABLET_STATS)
    # [0x07][2B start_len][start][2B end_len][end]
    payload = bytes([0x07])
    payload += struct.pack('>H', 0)       # start_len = 0 (空串)
    payload += struct.pack('>H', 1)       # end_len = 1
    payload += b'm'                       # end = "m"
    
    # 需要通过 Proxy -> DataNode，这里直接发到 Proxy 转发
    # 注意: TABLET_STATS 是 Proxy 发给 DataNode 的内部接口
    # 我们直接发给 DataNode 9002 测试
    print("\n[3.1] 查询 DataNode 9002 的区间统计 [\"\", m)")
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(("127.0.0.1", 9002))
        status, value = send_recv(sock, make_frame(payload))
        sock.close()
        check("TABLET_STATS 返回 OK", status == 0)
        if status == 0 and len(value) >= 6:
            key_count = struct.unpack('>I', value[0:4])[0]
            med_len = struct.unpack('>H', value[4:6])[0]
            median = value[6:6 + med_len].decode() if med_len > 0 else ""
            print(f"       key_count = {key_count}, median_key = '{median}'")
            check("key_count 正确 (>0)", key_count > 0)
    except Exception as e:
        check(f"TABLET_STATS 测试异常: {e}", False)


def test_performance(proxy_host="127.0.0.1", proxy_port=9001, n=1000):
    """测试4: 性能基准测试"""
    print("\n" + "=" * 60)
    print(f"测试4: 性能测试 (PUT/GET 各 {n} 次)")
    print("=" * 60)
    
    # 先写入一批 key
    keys = [f"perf_{i:04d}" for i in range(n)]
    
    # PUT 性能
    print(f"\n[4.1] PUT 写入 {n} 条数据...")
    # 先查路由，找到目标 DataNode
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((proxy_host, proxy_port))
    status, value = send_recv(sock, make_get_route(keys[0]))
    sock.close()
    if status != 0:
        print("路由查询失败，性能测试跳过")
        return
    addr_len = struct.unpack('>H', value[5:7])[0]
    addr = value[7:7 + addr_len].decode()
    host, port = addr.split(':')
    
    dsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    dsock.connect((host, int(port)))
    dsock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    t0 = time.time()
    for key in keys:
        status, _ = send_recv(dsock, make_put(key, "x" * 100))
        if status != 0:
            print(f"PUT {key} failed")
    t1 = time.time()
    put_qps = n / (t1 - t0)
    put_lat = (t1 - t0) * 1000 / n
    print(f"       PUT: {put_qps:.1f} ops/sec, 平均延迟 {put_lat:.3f} ms")
    
    # GET 性能
    print(f"\n[4.2] GET 读取 {n} 条数据...")
    random.shuffle(keys)
    t0 = time.time()
    for key in keys:
        status, _ = send_recv(dsock, make_get(key))
        if status != 0:
            print(f"GET {key} failed")
    t1 = time.time()
    get_qps = n / (t1 - t0)
    get_lat = (t1 - t0) * 1000 / n
    print(f"       GET: {get_qps:.1f} ops/sec, 平均延迟 {get_lat:.3f} ms")
    
    dsock.close()
    
    # 保存结果到文件，论文用
    with open("perf_result.txt", "w") as f:
        f.write(f"PUT_QPS={put_qps:.1f}\nPUT_LAT={put_lat:.3f}\n")
        f.write(f"GET_QPS={get_qps:.1f}\nGET_LAT={get_lat:.3f}\n")
    print("\n       性能数据已保存到 perf_result.txt")


def test_persistence():
    """测试5: 持久化测试 - 检查 DataNode 重启后数据是否还在"""
    print("\n" + "=" * 60)
    print("测试5: 持久化测试")
    print("说明: 检查 db_2/ 和 db_3/ 目录是否存在 LevelDB 数据文件")
    print("=" * 60)
    
    for node_id in [2, 3]:
        db_path = f"db_{node_id}"
        if os.path.exists(db_path):
            files = os.listdir(db_path)
            ldb_files = [f for f in files if f.endswith('.ldb') or f.endswith('.log')]
            check(f"DataNode {node_id} LevelDB 数据文件存在 ({len(ldb_files)} 个)", 
                  len(ldb_files) > 0)
            print(f"       {db_path}/: {files[:5]}...")
        else:
            check(f"DataNode {node_id} 数据目录 {db_path} 存在", False)


# ==================== 主入口 ====================

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--help":
        print("用法: python3 test_lekv.py [测试名]")
        print("测试名: basic / split / stats / perf / persist / all")
        return
    
    test_name = sys.argv[1] if len(sys.argv) > 1 else "all"
    
    print("=" * 60)
    print("LEKV 系统测试套件")
    print("=" * 60)
    print("请确保服务端已启动:")
    print("  ./lekv 9002  (DataNode)")
    print("  ./lekv 9003  (DataNode)")  
    print("  ./lekv 9001  (Proxy 9001)")
    
    try:
        if test_name in ("basic", "all"):
            test_basic_crud()
        if test_name in ("split", "all"):
            test_auto_split()
        if test_name in ("stats", "all"):
            test_tablet_stats()
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
