# 分布式键值存储系统的研究与实现

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
使用 telnet 工具进行交互



## 通信协议

### 通用帧格式

[**4B** FrameLen] [**1B** Magic] [**1B** Version] [**4B** RequestID ] [**NB** PayLoad]

- FrameLen：整个帧的字节数（含自身 4 字节），用于 TCP 流式拆包
- Magic：固定 `0x4C`（'L'），非法包直接断开连接
- Version：固定 `0x01`
- RequestID：客户端自增序号，响应必须原样带回，用于异步请求匹配
- PayLoad：数据

**粘包处理**：接收方先读 4 字节 FrameLen，再读剩余 `FrameLen - 4` 字节，得到一个完整帧。

### 操作码（OpCode） 定义

- 0x01：GET_ROUTE，Client -》Proxy：第一次请求，查询 key 所属分片地址
- 0x02：GET，Client -》DataNode：读取 key
- 0x03：PUT，Client -》DataNode：写入/更新 key
- 0x04：DELETE，Client -》DataNode：删除 key
- 0x05：PING，Proxy -》 DataNode：探测 DataNode 是否存活
- 0x06：SHARDS，Client -》DataNode：客户端启动时拉取全量路由表（目前留着）

### 第一次 RTT：路由查询（GET_ROUTE）

#### 请求报文（Client -> Proxy）PayLoad 结构

[**1B** Opcode = 0x01] [**2B** KeyLen] [**KeyLen B** KeyData]

- 只有 Opcode + Key，**没有 ValueLen 和 ValueData**
- Proxy 收到 `0x01` 后，**绝不触碰 StorageEngine**，只做纯计算和查表

#### 响应报文（Proxy -> Client）PayLoad 结构

[**1B** Status] [**1B** ShardID] [**4B** Epoch] [**ValueLen** RouteInfo]

- Status：返回状态
  - 0x00：**OK**，路由查询成功，客户端继续第二次 RTT
  - 0x01：**ERR_NO_SHARD**，无可用分片（所有 DataNode 离线）
  - 0x02：**ERR_KEY_INVALID**，Key 为空或超长（> 4KB）
- ShardID：分片ID
- Epoch：本地路由表版本号
- RouteInfo：分片地址 ，字符串格式（UTF-8，无 `\r\n`）<IP>:<Port>

### 第二次 RTT：真实操作（GET / PUT / DELETE）

#### 请求报文（Client → DataNode）PayLoad 结构

[**1B** Opcode] [**2B** KeyLen] [**4B** ValueLen] [**KeyLen** KeyData] [**ValueLen** ValueData]

**各操作约束：**

| 操作       | Opcode | KeyLen | ValueLen     | ValueData |
| ---------- | ------ | ------ | ------------ | --------- |
| **GET**    | `0x02` | > 0    | **必须为 0** | 不存在    |
| **PUT**    | `0x03` | > 0    | > 0          | 必须存在  |
| **DELETE** | `0x04` | > 0    | **必须为 0** | 不存在    |

**注意**：DataNode 收到 `0x02 GET` 时，如果 ValueLen ≠ 0，直接返回 `0x03 ERR_BAD_REQUEST`。

#### 响应报文（DataNode → Client）PayLoad 结构

[**1B** Status] [**4B** ValueLen] [**ValueLen** value data] [**ValueLen** RouteInfo



### 状态码全集

| 值     | 名称                      | 使用场景                                             | 客户端行为                             |
| ------ | ------------------------- | ---------------------------------------------------- | -------------------------------------- |
| `0x00` | **OK**                    | 操作成功                                             | 正常处理返回数据                       |
| `0x01` | **NOT\_FOUND**            | GET/DELETE 时 key 不存在                             | 向上层返回空                           |
| `0x02` | **NOT\_MY\_SHARD**        | DataNode 发现 key 不属于自己（路由表过期或正在迁移） | **必须刷新路由缓存**，重新走第一次 RTT |
| `0x03` | **ERR\_BAD\_REQUEST**     | 协议格式错误（如 GET 带 ValueLen>0）                 | 打印日志，不重试                       |
| `0x04` | **ERR\_TIMEOUT**          | DataNode 处理超时                                    | 可重试（幂等操作）                     |
| `0x05` | **ERR\_NO\_SHARD**        | Proxy 无可用分片                                     | 等待后重试或报错                       |
| `0x06` | **ERR\_KEY\_TOO\_LONG**   | Key 超过 4KB                                         | 拒绝该 key                             |
| `0x07` | **ERR\_VALUE\_TOO\_LONG** | Value 超过 16MB                                      | 拒绝该 value                           |
