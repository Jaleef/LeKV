#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

namespace lekv {
// ---------------------------------------------------------------------------
// SPSC 无锁环形队列
// ---------------------------------------------------------------------------
// 单生产者 / 单消费者，无锁、无分配、仅原子操作。
//
// 使用约束：
//   - prepare_write() 只能由**生产者线程**调用
//   - peek() / consume() 只能由**消费者线程**调用
//   - 任意线程可调用 empty() / readable_bytes()
//
// 内存模型：
//   - write_pos_ 由生产者写 (release)，消费者读 (acquire)
//   - read_pos_  由消费者写 (release)，生产者读 (acquire)
//   - 缓冲区中的数据本身由 memcpy 写入，无需额外 fence
//
// Wrap-around 处理：
//   - 容量对齐到 2 的幂，用位运算 (pos & mask) 映射到 buffer
//   - prepare_write 要求整块连续空间；若尾部剩余空间不足则返回 nullptr
//     （队列容量远大于单条日志，此情况极少触发）
//   - readable_bytes 同样只返回连续可读字节数
// ---------------------------------------------------------------------------

class SpscQueue {
public:
    // capacity: 期望容量，实际会对齐到不小于它的 2 的幂
    explicit SpscQueue(size_t capacity);

    ~SpscQueue();

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    // ========== 生产者接口（前端线程）===========
    /**
     * @brief 请求预留一块 连续的 len 字节的写入空间
     * 
     * @param len，请求字节数 
     * @return uint8_t*；空闲不足或尾部连续空间不足（需等待消费者）返回 nullptr，成功返回指向可写空间的指针
     */
    [[nodiscard]] uint8_t* prepare_write(size_t len);

    /**
     * @brief 提交写入：使 prepare_write 返回的 len 字节数据对消费者可见
     * 必须在 prepare_write 成功后，且实际写入数据完毕才能调用
     * 
     * @param len 提交写入的字节数
     */
    void commit_write(size_t len);

    /**
     * @brief 查询当前可写的连续字节数（到 buffer 末尾为止）
     * 
     * @return size_t 可写字节数
     */
    [[nodiscard]] size_t writable_bytes() const;

    // ========== 消费者接口（后端线程）===========

    /**
     * @brief 查看当前可读数据的起始指针（不移动读指针）
     *
     * @return uint8_t*；无可读数据返回 nullptr，成功返回指向可读数据的指针 
     */
    [[nodiscard]] const uint8_t* peek() const;

    /**
     * @brief 当前可连续读取的字节数（到 buffer 末尾为止，不跨 wrap）
     * 若数据跨过了 buffer 末尾边界，需先 consume 第一段后再次读取
     * 
     * @return size_t 可读字节数
     */
    [[nodiscard]] size_t readable_bytes() const;

    /**
     * @brief 消费 len 字节数据（移动读指针使其不再可见）
     * 
     * @param len 要消费的字节数，必须不超过 readable_bytes() 返回值 
     */
    void consume(size_t len);

    // ========== 通用接口 ============
    [[nodiscard]] bool empty() const;

    [[nodiscard]] size_t capacity() const noexcept;

private:
    // 写指针（仅生产者修改）
    alignas(64) std::atomic<size_t> write_pos_{0};

    // 读指针（仅消费者修改）
    alignas(64) std::atomic<size_t> read_pos_{0};

    const size_t capacity_;  // 队列容量（对齐到 2 的幂）
    const size_t mask_;      // capacity_ - 1, 用于位运算映射到 buffer
    uint8_t* const buffer_;  // 64 字节对齐的环形缓冲区

};

}   // namespace LEKV
