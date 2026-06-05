# 分布式键值存储系统的研究与实现

## 目录结构

- README.md
- CMakeLists.txt
- src
  - lekv.cpp 节点的运行入口
  - lekv_cli.cpp 客户端代码
  - CMakeLists.txt
  - storage 存储引擎层
    - storage_engine.h/cpp
    - CMakeLists.txt
  - rpc 通信层
    - binary_protocol.h/cpp 二进制通信协议定义
    - text_protocol.h/cpp 文本通信协议定义
    - rpc_client.h/cpp 发送端
    - rpc_server.h/cpp 接收端
    - CMakeLists.txt
  - kv 节点运行层
    - raft_types.h 数据结构定义
    - raft_node.h/cpp 节点的运行逻辑代码
    - CMakeLists.txt





## 运行方式

**目前的运行一定要 Follower 节点先运行**

```
mkdir build && cd build
cmake ..
make

cd build/bin

// 运行 Follower 节点
./lekv 9002
./lekv 9003

// 运行 Leader 节点
./lekv
```

系统就运行起来了
使用 lekv_cli 客户端程序进行交互



## 通信协议

### 通用帧格式

[**4B** FrameLen] [**1B** Magic] [**1B** Version] [**4B** RequestID ] [**NB** PayLoad]

- FrameLen：整个帧的字节数（含自身 4 字节），用于 TCP 流式拆包
- Magic：固定 `0x4C`（'L'），非法包直接断开连接
- Version：固定 `0x01`
- RequestID：客户端自增序号，响应必须原样带回，用于异步请求匹配
- PayLoad：数据

**粘包处理**：接收方先读 4 字节 FrameLen，再读剩余 `FrameLen - 4` 字节，得到一个完整帧。



### 路由查询（GET_ROUTE）

#### 请求帧

[**1B** Opcode = 0x01] [**2B** KeyLen] [**KeyLen B** KeyData]

- 只有 Opcode + Key，**没有 ValueLen 和 ValueData**
- Proxy 收到 `0x01` 后，**绝不触碰 StorageEngine**，只做纯计算和查表

#### 响应帧

[**1B** Status] [**4B** ValueLen] [**1B** TabletID] [**4B** Epoch]  [**2B** RouteLen] [**RouteLen** RouteInfo]

- Status：返回状态
  - 0x00：**OK**，路由查询成功，客户端继续第二次 RTT
  - 0x01：**ERR_NO_SHARD**，无可用分片（所有 DataNode 离线）
  - 0x02：**ERR_KEY_INVALID**，Key 为空或超长（> 4KB）
- ShardID：分片ID
- Epoch：本地路由表版本号
- RouteLen：分片地址的长度
- RouteInfo：分片地址 ，字符串格式（UTF-8，无 `\r\n`）<IP>:<Port>



### SHARDS 请求 

#### 请求帧

[**1B** Opcode = 0x06] [**2B** KeyLen = 0(空)] [**4 B** ValueLen = 0(空)]

### 响应帧

[**1B** Status] [**4B** ValueLen] [**ValueLen B** ]



### KV操作（GET / PUT / DELETE）

#### 请求帧

[**1B** Opcode] [**2B** KeyLen] [**4B** ValueLen] [**KeyLen** KeyData] [**ValueLen** ValueData]

**各操作约束：**

| 操作       | Opcode | KeyLen | ValueLen     | ValueData |
| ---------- | ------ | ------ | ------------ | --------- |
| **GET**    | `0x02` | > 0    | **必须为 0** | 不存在    |
| **PUT**    | `0x03` | > 0    | > 0          | 必须存在  |
| **DELETE** | `0x04` | > 0    | **必须为 0** | 不存在    |

**注意**：DataNode 收到 `0x02 GET` 时，如果 ValueLen ≠ 0，直接返回 `0x03 BAD_REQUEST`。

#### 响应帧

[**1B** Status] [**4B** ValueLen] [**ValueLen** value data]



### TABLET_STATS 请求

#### 请求帧

[**1B** Opcode = 0x07] [**2B** StartLen] [**StartLen B** start] [**2B** EndLen] [**EndLen B**  end] 

#### 响应帧

[**1B** Status] [**4B** ValueLe] [**4B** KeyCount] [**2B** MedianLen] [Median]



### SCAN_RANGE 数据迁移

#### 请求帧（Proxy -》 源 DataNode）

[**1B** Opcode = 0x08] [**2B** StartKeyLen] [StartKey] [**2B** EndKeyLen ] [EndKey]

| 字段    | 长度 | 说明                                 |
| ------- | ---- | ------------------------------------ |
| `Op`    | 1B   | 固定 `0x08`                          |
| `slen`  | 2B   | start\_key 长度（`htonl`），空串为 0 |
| `start` | slen | 区间起始键（包含），空串表示 `-∞`    |
| `elen`  | 2B   | end\_key 长度（`htonl`），空串为 0   |
| `end`   | elen | 区间结束键（不包含），空串表示 `+∞`  |

#### 响应帧

[**1B** Status] [**4B** ValueLen] [**4B** Count] [count * (2B keylen + key + 4B valuelen + value)]

| 字段      | 长度 | 说明                             |
| --------- | ---- | -------------------------------- |
| `Status`  | 1B   | `0x00` OK                        |
| `vlen`    | 4B   | 后续 payload 总字节数（`htonl`） |
| `count`   | 4B   | KV 对的数量（`htonl`）           |
| `keylen`  | 2B   | 第 i 个 key 的长度（`htonl`）    |
| `key`     | klen | key 数据                         |
| `valulen` | 4B   | 第 i 个 value 的长度（`htonl`）  |
| `value`   | vlen | value 数据                       |



### DELETE_RANGE 范围删除（迁移后清理源节点）

#### 请求帧（Proxy -> 源 DataNode）

[**1B** Opcode = 0x09] [**2B** StartKeyLen] [StartKey] [**2B** EndKeyLen] [EndKey]

| 字段    | 长度 | 说明                                |
| ------- | ---- | ----------------------------------- |
| `Op`    | 1B   | 固定 `0x09`                         |
| `slen`  | 2B   | start_key 长度（`htons`），空串为 0 |
| `start` | slen | 区间起始键（包含），空串表示 `-∞`   |
| `elen`  | 2B   | end_key 长度（`htons`），空串为 0   |
| `end`   | elen | 区间结束键（不包含），空串表示 `+∞` |

#### 响应帧

[**1B** Status]

| 字段     | 长度 | 说明      |
| -------- | ---- | --------- |
| `Status` | 1B   | `0x00` OK |

**说明**：`OP_DELETE_RANGE` 由 Master 在 Tablet 迁移或分裂完成后发送到**源 DataNode**，用于删除已迁移/分裂出去的数据。这确保客户端即使因缓存过期而请求到旧节点，也不会读到过期的数据。



### 帧类型表

| 操作码     | 名称               | 发起方       | 处理方       | 用途                              |
| ---------- | ------------------ | ------------ | ------------ | --------------------------------- |
| `0x01`     | `GET_ROUTE`        | 客户端       | Proxy        | 查 key 路由到哪个 DataNode        |
| `0x02`     | `GET`              | 客户端       | DataNode     | 读单个 key                        |
| `0x03`     | `PUT`              | 客户端/Proxy | DataNode     | 写单个 key                        |
| `0x04`     | `DELETE`           | 客户端       | DataNode     | 删除单个 key                      |
| `0x06`     | `SHARDS`           | 客户端       | Proxy        | 拉取全量 Tablet 路由表            |
| `0x07`     | `TABLET_STATS`     | Proxy        | DataNode     | 查区间 key 数量 + 中位数          |
| **`0x08`** | **`SCAN_RANGE`**   | **Proxy**    | **DataNode** | **扫描区间全部 KV（迁移用）**     |
| **`0x09`** | **`DELETE_RANGE`** | **Proxy**    | **DataNode** | **删除区间全部 KV（迁移清理用）** |



### 状态码全集

| 值     | 名称                 | 使用场景                                             | 客户端行为                             |
| ------ | -------------------- | ---------------------------------------------------- | -------------------------------------- |
| `0x00` | **OK**               | 操作成功                                             | 正常处理返回数据                       |
| `0x01` | **NOT\_FOUND**       | GET/DELETE 时 key 不存在                             | 向上层返回空                           |
| `0x02` | **NOT\_MY\_SHARD**   | DataNode 发现 key 不属于自己（路由表过期或正在迁移） | **必须刷新路由缓存**，重新走第一次 RTT |
| `0x03` | **BAD\_REQUEST**     | 协议格式错误（如 GET 带 ValueLen>0）                 | 打印日志，不重试                       |
| `0x04` | **TIMEOUT**          | DataNode 处理超时                                    | 可重试（幂等操作）                     |
| `0x05` | **NO\_SHARD**        | Proxy 无可用分片                                     | 等待后重试或报错                       |
| `0x06` | **KEY\_TOO\_LONG**   | Key 超过 4KB                                         | 拒绝该 key                             |
| `0x07` | **VALUE\_TOO\_LONG** | Value 超过 16MB                                      | 拒绝该 value                           |
