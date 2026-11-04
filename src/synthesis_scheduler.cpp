#include "tts/synthesis_scheduler.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace tts {

    SynthesisScheduler::SynthesisScheduler(const std::size_t max_pending) :
        max_pending_(std::max<std::size_t>(1, max_pending)),
        worker_([this](const std::stop_token) {
            run();
        }) {
    }

    SynthesisScheduler::~SynthesisScheduler() {
        shutdown();
    }

    bool SynthesisScheduler::submit(Job job) {
        std::vector<std::function<void()>> dropped;
        {
            const std::lock_guard lock(mutex_);
            if (stopped_ || queue_.size() >= max_pending_) {
                return false;
            }

            if (!job.coalesce_key.empty()) {
                for (auto iterator = queue_.begin(); iterator != queue_.end();) {
                    if ((*iterator)->job.coalesce_key == job.coalesce_key) {
                        if ((*iterator)->job.on_dropped) {
                            dropped.push_back(std::move((*iterator)->job.on_dropped));
                        }
                        iterator = queue_.erase(iterator);
                    }
                    else {
                        ++iterator;
                    }
                }
            }

            auto entry = std::make_shared<Entry>();
            entry->cancel = job.cancel ? job.cancel : std::make_shared<std::atomic_bool>(false);
            entry->sequence = next_sequence_++;
            entry->job = std::move(job);

            if (entry->job.preempt_active) {
                preempt_lower_priority_locked(entry->job.priority);
            }

            queue_.push_back(std::move(entry));
        }

        // 回调可能发布事件，放在锁外执行。
        for (auto& callback : dropped) {
            callback();
        }
        condition_.notify_one();
        return true;
    }

    void SynthesisScheduler::preempt_lower_priority(const JobPriority priority) {
        const std::lock_guard lock(mutex_);
        preempt_lower_priority_locked(priority);
    }

    void SynthesisScheduler::preempt_lower_priority_locked(const JobPriority priority) {
        if (active_ && active_->job.priority < priority) {
            active_->cancel->store(true);
        }
        for (const auto& pending : queue_) {
            if (pending->job.priority < priority) {
                pending->cancel->store(true);
            }
        }
    }

    void SynthesisScheduler::shutdown() {
        std::vector<std::shared_ptr<Entry>> dropped;
        std::shared_ptr<Entry> active;
        {
            const std::lock_guard lock(mutex_);
            if (!stopped_) {
                stopped_ = true;
                dropped.swap(queue_);
                active = active_;
            }
        }

        if (active) {
            active->cancel->store(true);
        }
        for (const auto& entry : dropped) {
            if (entry->job.on_dropped) {
                entry->job.on_dropped();
            }
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::size_t SynthesisScheduler::pending() const {
        const std::lock_guard lock(mutex_);
        return queue_.size();
    }

    bool SynthesisScheduler::higher_priority(const Entry& left, const Entry& right) noexcept {
        if (left.job.priority != right.job.priority) {
            return left.job.priority > right.job.priority;
        }
        return left.sequence < right.sequence;
    }

    void SynthesisScheduler::run() {
        for (;;) {
            std::shared_ptr<Entry> entry;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] {
                    return stopped_ || !queue_.empty();
                });
                if (queue_.empty()) {
                    return;
                }

                auto best = queue_.begin();
                for (auto candidate = std::next(best); candidate != queue_.end(); ++candidate) {
                    if (higher_priority(**candidate, **best)) {
                        best = candidate;
                    }
                }
                entry = std::move(*best);
                queue_.erase(best);
                active_ = entry;
            }

            if (entry->job.run) {
                entry->job.run(*entry->cancel);
            }

            {
                const std::lock_guard lock(mutex_);
                active_.reset();
            }
        }
    }

} // namespace tts
