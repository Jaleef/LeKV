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
