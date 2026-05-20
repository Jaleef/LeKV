# 1. 删除旧的日志和数据
rm -rf *.log
rm -rf db_*
rm -rf *.json


# 1. 编译并启动服务端
./../build/bin/lekv 9002 > /dev/null 2>&1 &    # DataNode
./../build/bin/lekv 9003 > /dev/null 2>&1 &    # DataNode  
./../build/bin/lekv 9001 > /dev/null 2>&1 &    # Proxy


# 2. 确认进程已启动
sleep 1
ps aux | grep lekv

# 3. 运行全部测试
cd ../../test
python test_lekv.py all

# # 4. 单独运行某项测试
# python3 test_lekv.py basic    # 基本 CRUD
# python3 test_lekv.py split    # 自动分裂（写入15条后等待分裂）
# python3 test_lekv.py stats    # Tablet 统计
# python3 test_lekv.py perf     # 性能基准（1000条）
# python3 test_lekv.py persist  # 持久化检查

# 4. 性能数据会自动写入 perf_result.txt
cat perf_result.txt

# 5. 清理进程
pkill lekv
