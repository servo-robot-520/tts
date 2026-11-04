#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tts {

    enum class JobPriority : std::uint8_t {
        Low = 0,
        Normal = 1,
        High = 2,
    };

    /// 单工作线程的合成调度器。
    ///
    /// - 任意时刻只执行一个任务，后端推理天然串行；
    /// - 按优先级取队首，同优先级 FIFO；
    /// - 非空 `coalesce_key` 会丢弃等待队列中的旧任务（`~/text` 最新覆盖）；
    /// - `preempt_active` 会取消正在执行和等待中的低优先级任务（状态打断播报）。
    class SynthesisScheduler {
    public:
        struct Job {
            JobPriority priority{JobPriority::Normal};
            /// 非空时，提交会丢弃等待队列中相同 key 的任务。
            std::string coalesce_key;
            /// true 时取消正在执行及等待中的低优先级任务。
            bool preempt_active{false};
            /// 外部取消标志（例如 say goal 的标志）；空则由调度器创建。
            std::shared_ptr<std::atomic_bool> cancel;
            /// 在唯一工作线程执行；`cancel` 由调度器或外部置位。
            std::function<void(const std::atomic_bool& cancel)> run;
            /// 任务被合并、关闭或预抢占时调用；与 `run` 互斥。
            std::function<void()> on_dropped;
        };

        explicit SynthesisScheduler(std::size_t max_pending = 16);
        ~SynthesisScheduler();

        SynthesisScheduler(const SynthesisScheduler&) = delete;
        SynthesisScheduler& operator=(const SynthesisScheduler&) = delete;
        SynthesisScheduler(SynthesisScheduler&&) = delete;
        SynthesisScheduler& operator=(SynthesisScheduler&&) = delete;

        /// 入队；返回 false 表示调度器已停止或等待队列已满。
        [[nodiscard]] bool submit(Job job);

        /// 取消正在执行及等待中的低优先级任务，但不入队新任务。
        /// 供命中缓存、无需合成但要打断旧播报的请求使用。
        void preempt_lower_priority(JobPriority priority);

        /// 取消当前任务、丢弃等待任务并等待工作线程退出；可重复调用。
        void shutdown();

        [[nodiscard]] std::size_t pending() const;

    private:
        struct Entry {
            Job job;
            std::shared_ptr<std::atomic_bool> cancel;
            std::uint64_t sequence{0};
        };

        [[nodiscard]] static bool higher_priority(const Entry& left, const Entry& right) noexcept;
        void preempt_lower_priority_locked(JobPriority priority);
        void run();

        const std::size_t max_pending_;
        mutable std::mutex mutex_;
        std::condition_variable condition_;
        std::vector<std::shared_ptr<Entry>> queue_;
        std::shared_ptr<Entry> active_;
        std::uint64_t next_sequence_{0};
        bool stopped_{false};
        std::jthread worker_;
    };

} // namespace tts
