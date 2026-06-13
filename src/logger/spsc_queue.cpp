#include "spsc_queue.h"
#include "common.h"

#include <bit>

namespace LEKV {

SpscQueue::SpscQueue(size_t capacity)
    : capacity_(bit_ceil(capacity | 1))    // 至少为 2，且为 2 的幂次方
    , mask_(capacity_ - 1)
    , buffer_(new (std::align_val_t{64}) uint8_t[capacity_]) {}

SpscQueue::~SpscQueue() {
    operator delete[](buffer_, std::align_val_t{64});
}

// ========== 生产者接口 ==========
uint8_t* SpscQueue::prepare_write(size_t len) {
    // len 为 0 直接返回当前写位置
    if (len == 0) {
        const size_t wp = write_pos_.load(std::memory_order_relaxed);
        return buffer_ + (wp & mask_);
    }

    const size_t wp = write_pos_.load(std::memory_order_relaxed);

    // 生产者以 acquire 读 read_pos，确保看到消费者最新的 consume 进度
    const size_t rp = read_pos_.load(std::memory_order_acquire);

    // 可用空间 = capacity - 已用 - 1，需要预留一个字节区分满和空
    const size_t used = wp - rp;
    if (len > capacity_ - used - 1) {
        return nullptr;    // 空间不足
    }

    // 检查尾部连续空间是否足够（不跨 wrap）
    const size_t pos = wp & mask_;
    const size_t to_end = capacity_ - pos;
    if (to_end < len) {
        return nullptr;    // 尾部空间不足，需等待消费者消费
    }

    return buffer_ + pos;
}

void SpscQueue::commit_write(size_t len) {
    // release: 确保之前对 buffer 的写入在此操作前完成，并对消费者可见
    write_pos_.fetch_add(len, std::memory_order_release);
}

size_t SpscQueue::writable_bytes() const {
    const size_t wp = write_pos_.load(std::memory_order_relaxed);
    const size_t rp = read_pos_.load(std::memory_order_acquire);

    const size_t free_total = capacity_ - (wp - rp) - 1;
    const size_t pos = wp & mask_;
    const size_t to_end = capacity_ - pos;

    return std::min(free_total, to_end);
}

// ========== 消费者接口 ==========
const uint8_t* SpscQueue::peek() const {
    const size_t rp = read_pos_.load(std::memory_order_relaxed);

    // 消费者以 acquire 读 write_pos，确保看到生产者最新的 commit 进度
    const size_t wp = write_pos_.load(std::memory_order_acquire);

    if (rp == wp) {
        return nullptr;    // 队列空
    }

    return buffer_ + (rp & mask_);
}

size_t SpscQueue::readable_bytes() const {
    const size_t rp = read_pos_.load(std::memory_order_relaxed);
    const size_t wp = write_pos_.load(std::memory_order_acquire);

    const size_t readable_total = wp - rp;
    if (readable_total == 0) {
        return 0;    // 队列空
    }

    const size_t pos = rp & mask_;
    const size_t to_end = capacity_ - pos;

    // 只返回连续到 buffer 末尾的字节数
    return std::min(readable_total, to_end);
}

void SpscQueue::consume(size_t len) {
    // release: 确保之前对 buffer 的读取在此操作前完成，并对生产者可见
    read_pos_.fetch_add(len, std::memory_order_release);
}

// ========== 通用接口 ==========
bool SpscQueue::empty() const {
    const size_t rp = read_pos_.load(std::memory_order_relaxed);
    const size_t wp = write_pos_.load(std::memory_order_acquire);
    return rp == wp;
}

size_t SpscQueue::capacity() const noexcept {
    return capacity_;
}

}    // namespace LEKV
