#!/usr/bin/env python3
"""
LEKV Python Client - 纯 socket 实现二进制协议
支持: PUT / GET / DELETE / SHARDS(路由表查询)
"""

import socket
import struct
import time
from typing import Optional, List, Tuple

# ---- 协议常量 ----
MAGIC = 0x4C
VERSION = 0x01

OP_GET_ROUTE = 0x01
OP_GET = 0x02
OP_PUT = 0x03
OP_DELETE = 0x04
OP_SHARDS = 0x06

ST_OK = 0x00
ST_NOT_FOUND = 0x01
ST_NOT_MY_SHARD = 0x02


class TabletInfo:
    """路由表中的 Tablet 信息"""
    def __init__(self, tid: int, start: str, end: str, addr: str):
        self.id = tid
        self.start = start
        self.end = end
        self.addr = addr

    def __repr__(self):
        return f"T{self.id}[{self.start},{self.end})->{self.addr}"


class LekvClient:
    """
    LEKV 分布式键值存储客户端。
    每次操作前实时从 Proxy 拉取路由表（无本地缓存）。
    """

    def __init__(self, proxy_host: str = "127.0.0.1", proxy_port: int = 9001):
        self.proxy_host = proxy_host
        self.proxy_port = proxy_port
        self._req_id = 1

    # ---- 内部工具 ----

    def _next_req_id(self) -> int:
        rid = self._req_id
        self._req_id += 1
        return rid

    @staticmethod
    def _encode_frame(req_id: int, payload: bytes) -> bytes:
        """编码一帧: [4B FrameLen][1B Magic][1B Version][4B ReqID][NB Payload]"""
        frame_len = 4 + 1 + 1 + 4 + len(payload)
        return struct.pack(">I", frame_len) + bytes([MAGIC, VERSION]) + struct.pack(">I", req_id) + payload

    @staticmethod
    def _decode_frame(buf: bytes) -> Optional[Tuple[int, bytes]]:
        """从缓冲区解码一帧，返回 (req_id, payload) 或 None"""
        if len(buf) < 10:
            return None
        frame_len = struct.unpack(">I", buf[:4])[0]
        if len(buf) < frame_len:
            return None
        if buf[4] != MAGIC or buf[5] != VERSION:
            return None
        req_id = struct.unpack(">I", buf[6:10])[0]
        payload = buf[10:frame_len]
        return req_id, payload

    def _recv_frame(self, sock: socket.socket, timeout: float = 5.0) -> Tuple[int, bytes]:
        """从 socket 接收完整的一帧"""
        sock.settimeout(timeout)
        buf = b""
        while True:
            result = self._decode_frame(buf)
            if result is not None:
                return result
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("Socket closed")
            buf += chunk

    def _send_and_recv(self, sock: socket.socket, payload: bytes, timeout: float = 5.0) -> bytes:
        """发送 payload 并接收响应 payload"""
        req_id = self._next_req_id()
        frame = self._encode_frame(req_id, payload)
        sock.sendall(frame)
        resp_req_id, resp_payload = self._recv_frame(sock, timeout)
        if resp_req_id != req_id:
            raise RuntimeError(f"ReqID mismatch: sent {req_id}, got {resp_req_id}")
        return resp_payload

    # ---- 低层协议 ----

    def _encode_kv_request(self, opcode: int, key: str, value: str = "") -> bytes:
        """编码 PUT/GET/DELETE 请求 payload"""
        key_bytes = key.encode("utf-8")
        value_bytes = value.encode("utf-8")
        return (bytes([opcode])
                + struct.pack(">H", len(key_bytes))
                + struct.pack(">I", len(value_bytes))
                + key_bytes
                + value_bytes)

    @staticmethod
    def _parse_kv_response(payload: bytes) -> Tuple[int, str]:
        """解析 PUT/GET/DELETE 响应, 返回 (status, value)"""
        if len(payload) < 5:
            return payload[0] if payload else -1, ""
        status = payload[0]
        val_len = struct.unpack(">I", payload[1:5])[0]
        value = payload[5:5 + val_len].decode("utf-8", errors="replace") if val_len > 0 else ""
        return status, value

    def _connect(self, host: str, port: int) -> socket.socket:
        """建立 TCP 连接"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect((host, port))
        return sock

    # ---- 路由表拉取 ----

    def fetch_route_table(self) -> List[TabletInfo]:
        """从 Proxy 实时拉取全量路由表"""
        sock = self._connect(self.proxy_host, self.proxy_port)
        try:
            payload = self._encode_kv_request(OP_SHARDS, "")
            resp = self._send_and_recv(sock, payload)
            status, body = self._parse_kv_response(resp)
            if status != ST_OK:
                raise RuntimeError(f"SHARDS failed: status={status}")
            return self._parse_shards_body(body)
        finally:
            sock.close()

    @staticmethod
    def _parse_shards_body(body: str) -> List[TabletInfo]:
        """解析 SHARDS 响应 body: '2 T1::m:127.0.0.1:9002 T2:m::127.0.0.1:9003'"""
        tablets = []
        parts = body.strip().split()
        if not parts:
            return tablets
        count = int(parts[0])
        for seg in parts[1:]:
            # seg: "id:start:end:addr"
            fields = seg.split(":")
            if len(fields) >= 5:
                tid = int(fields[0])
                start = fields[1]
                end = fields[2]
                addr = fields[3] + ":" + fields[4]
                tablets.append(TabletInfo(tid, start, end, addr))
        tablets.sort(key=lambda t: t.start)
        return tablets

    def find_route(self, key: str, tablets: List[TabletInfo]) -> Optional[str]:
        """在路由表中二分查找 key 对应的 DataNode 地址"""
        left, right = 0, len(tablets)
        while left < right:
            mid = (left + right) // 2
            if tablets[mid].start <= key:
                left = mid + 1
            else:
                right = mid
        if left == 0:
            return None
        t = tablets[left - 1]
        if t.end and key >= t.end:
            return None
        return t.addr

    # ---- KV 操作 ----

    def put(self, key: str, value: str) -> bool:
        """写入 key-value, 返回是否成功"""
        return self._do_kv(OP_PUT, key, value)[0] == ST_OK

    def get(self, key: str) -> Optional[str]:
        """读取 key, 返回 value 或 None(key不存在)"""
        status, value = self._do_kv(OP_GET, key)
        if status == ST_OK:
            return value
        elif status == ST_NOT_FOUND:
            return None
        else:
            raise RuntimeError(f"GET failed: status={status:#x}")

    def delete(self, key: str) -> bool:
        """删除 key, 返回是否成功"""
        status, _ = self._do_kv(OP_DELETE, key)
        return status == ST_OK

    def _do_kv(self, opcode: int, key: str, value: str = "") -> Tuple[int, str]:
        """执行一次 KV 操作: 拉路由表 -> 找 DataNode -> 发送请求"""
        tablets = self.fetch_route_table()
        addr = self.find_route(key, tablets)
        if addr is None:
            raise RuntimeError(f"No route found for key: {key}")

        host, port_str = addr.split(":")
        port = int(port_str)

        sock = self._connect(host, port)
        try:
            payload = self._encode_kv_request(opcode, key, value)
            resp = self._send_and_recv(sock, payload)
            status, result = self._parse_kv_response(resp)

            if status == ST_NOT_MY_SHARD:
                # 路由刚变更，重试一次（重新拉取路由表）
                time.sleep(0.1)
                tablets = self.fetch_route_table()
                addr2 = self.find_route(key, tablets)
                if addr2 is None:
                    raise RuntimeError(f"No route found for key on retry: {key}")
                sock2 = self._connect(addr2.split(":")[0], int(addr2.split(":")[1]))
                try:
                    resp2 = self._send_and_recv(sock2, payload)
                    status, result = self._parse_kv_response(resp2)
                finally:
                    sock2.close()

            return status, result
        finally:
            sock.close()

    def get_tablets_info(self) -> List[TabletInfo]:
        """获取当前路由表（用于测试验证）"""
        return self.fetch_route_table()
