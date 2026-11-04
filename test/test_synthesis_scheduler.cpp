#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <gtest/gtest.h>

#include "tts/synthesis_scheduler.hpp"

namespace {

    using namespace std::chrono_literals;

    TEST(SynthesisSchedulerTest, RunsHighPriorityBeforeQueuedLowerPriority) {
        tts::SynthesisScheduler scheduler{8};
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<int> order;
        bool gate_started = false;
        bool release_gate = false;
        int finished = 0;

        const auto make_job = [&](const tts::JobPriority priority, const int id) {
            tts::SynthesisScheduler::Job job;
            job.priority = priority;
            job.run = [&, id](const std::atomic_bool&) {
                if (id == 0) {
                    std::unique_lock lock(mutex);
                    gate_started = true;
                    condition.notify_all();
                    condition.wait(lock, [&] {
                        return release_gate;
                    });
                }
                {
                    const std::lock_guard lock(mutex);
                    order.push_back(id);
                    ++finished;
                }
                condition.notify_all();
            };
            return job;
        };

        ASSERT_TRUE(scheduler.submit(make_job(tts::JobPriority::Low, 0)));
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return gate_started;
            });
        }

        ASSERT_TRUE(scheduler.submit(make_job(tts::JobPriority::Normal, 1)));
        ASSERT_TRUE(scheduler.submit(make_job(tts::JobPriority::Normal, 2)));
        ASSERT_TRUE(scheduler.submit(make_job(tts::JobPriority::High, 3)));

        {
            const std::lock_guard lock(mutex);
            release_gate = true;
        }
        condition.notify_all();
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return finished == 4;
            });
        }

        EXPECT_EQ(order, (std::vector<int>{0, 3, 1, 2}));
    }

    TEST(SynthesisSchedulerTest, CoalescesPendingJobsWithSameKey) {
        tts::SynthesisScheduler scheduler{8};
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<int> order;
        bool gate_started = false;
        bool release_gate = false;
        int dropped = 0;
        int finished = 0;

        tts::SynthesisScheduler::Job gate;
        gate.priority = tts::JobPriority::Low;
        gate.run = [&](const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            gate_started = true;
            condition.notify_all();
            condition.wait(lock, [&] {
                return release_gate;
            });
            order.push_back(0);
            ++finished;
        };
        ASSERT_TRUE(scheduler.submit(std::move(gate)));
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return gate_started;
            });
        }

        const auto make_text_job = [&](const int id) {
            tts::SynthesisScheduler::Job job;
            job.priority = tts::JobPriority::High;
            job.coalesce_key = "text:tts";
            job.run = [&, id](const std::atomic_bool&) {
                {
                    const std::lock_guard lock(mutex);
                    order.push_back(id);
                    ++finished;
                }
                condition.notify_all();
            };
            job.on_dropped = [&] {
                const std::lock_guard lock(mutex);
                ++dropped;
            };
            return job;
        };

        ASSERT_TRUE(scheduler.submit(make_text_job(1)));
        ASSERT_TRUE(scheduler.submit(make_text_job(2)));

        {
            const std::lock_guard lock(mutex);
            release_gate = true;
        }
        condition.notify_all();
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return finished == 2;
            });
        }

        EXPECT_EQ(dropped, 1);
        EXPECT_EQ(order, (std::vector<int>{0, 2}));
        EXPECT_EQ(scheduler.pending(), 0U);
    }

    TEST(SynthesisSchedulerTest, PreemptsActiveAndPendingLowerPriorityJobs) {
        tts::SynthesisScheduler scheduler{8};
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<int> order;
        bool gate_started = false;
        int finished = 0;

        const auto cancel_flag = std::make_shared<std::atomic_bool>(false);
        tts::SynthesisScheduler::Job gate;
        gate.priority = tts::JobPriority::Low;
        gate.cancel = cancel_flag;
        gate.run = [&](const std::atomic_bool& cancel) {
            std::unique_lock lock(mutex);
            gate_started = true;
            condition.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
                condition.wait_for(lock, 5ms);
            }
            order.push_back(cancel.load() ? 0 : -1);
            ++finished;
            condition.notify_all();
        };
        ASSERT_TRUE(scheduler.submit(std::move(gate)));
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return gate_started;
            });
        }

        tts::SynthesisScheduler::Job pending;
        pending.priority = tts::JobPriority::Normal;
        pending.cancel = std::make_shared<std::atomic_bool>(false);
        const auto pending_cancel = pending.cancel;
        pending.run = [&](const std::atomic_bool& cancel) {
            {
                const std::lock_guard lock(mutex);
                order.push_back(cancel.load() ? 2 : 1);
                ++finished;
            }
            condition.notify_all();
        };
        ASSERT_TRUE(scheduler.submit(std::move(pending)));

        tts::SynthesisScheduler::Job status;
        status.priority = tts::JobPriority::High;
        status.preempt_active = true;
        status.run = [&](const std::atomic_bool&) {
            {
                const std::lock_guard lock(mutex);
                order.push_back(3);
                ++finished;
            }
            condition.notify_all();
        };
        ASSERT_TRUE(scheduler.submit(std::move(status)));

        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return finished == 3;
            });
        }

        EXPECT_TRUE(cancel_flag->load());
        EXPECT_TRUE(pending_cancel->load());
        EXPECT_EQ(order, (std::vector<int>{0, 3, 2}));
    }

    TEST(SynthesisSchedulerTest, ExplicitPreemptionCancelsLowerPriorityWithoutEnqueue) {
        tts::SynthesisScheduler scheduler{8};
        std::mutex mutex;
        std::condition_variable condition;
        bool gate_started = false;
        int finished = 0;

        const auto active_cancel = std::make_shared<std::atomic_bool>(false);
        tts::SynthesisScheduler::Job active;
        active.priority = tts::JobPriority::Low;
        active.cancel = active_cancel;
        active.run = [&](const std::atomic_bool& cancel) {
            std::unique_lock lock(mutex);
            gate_started = true;
            condition.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
                condition.wait_for(lock, 5ms);
            }
            ++finished;
            condition.notify_all();
        };
        ASSERT_TRUE(scheduler.submit(std::move(active)));
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return gate_started;
            });
        }

        const auto pending_cancel = std::make_shared<std::atomic_bool>(false);
        tts::SynthesisScheduler::Job pending;
        pending.priority = tts::JobPriority::Normal;
        pending.cancel = pending_cancel;
        pending.run = [&](const std::atomic_bool&) {
            {
                const std::lock_guard lock(mutex);
                ++finished;
            }
            condition.notify_all();
        };
        ASSERT_TRUE(scheduler.submit(std::move(pending)));

        scheduler.preempt_lower_priority(tts::JobPriority::High);

        EXPECT_TRUE(active_cancel->load());
        EXPECT_TRUE(pending_cancel->load());
        EXPECT_EQ(scheduler.pending(), 1U);

        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return finished == 2;
            });
        }
    }

    TEST(SynthesisSchedulerTest, RejectsJobWhenQueueIsFull) {
        tts::SynthesisScheduler scheduler{1};
        std::mutex mutex;
        std::condition_variable condition;
        bool gate_started = false;
        bool release_gate = false;
        int finished = 0;

        tts::SynthesisScheduler::Job gate;
        gate.run = [&](const std::atomic_bool&) {
            std::unique_lock lock(mutex);
            gate_started = true;
            condition.notify_all();
            condition.wait(lock, [&] {
                return release_gate;
            });
            ++finished;
            condition.notify_all();
        };
        ASSERT_TRUE(scheduler.submit(std::move(gate)));
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return gate_started;
            });
        }

        tts::SynthesisScheduler::Job pending;
        pending.run = [&](const std::atomic_bool&) {
            {
                const std::lock_guard lock(mutex);
                ++finished;
            }
            condition.notify_all();
        };
        ASSERT_TRUE(scheduler.submit(std::move(pending)));

        tts::SynthesisScheduler::Job rejected;
        rejected.run = [&](const std::atomic_bool&) {
            const std::lock_guard lock(mutex);
            ++finished;
        };
        EXPECT_FALSE(scheduler.submit(std::move(rejected)));

        {
            const std::lock_guard lock(mutex);
            release_gate = true;
        }
        condition.notify_all();
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return finished == 2;
            });
        }
        EXPECT_EQ(finished, 2);
    }

    TEST(SynthesisSchedulerTest, ShutdownCancelsActiveAndDropsPending) {
        tts::SynthesisScheduler scheduler{8};
        std::mutex mutex;
        std::condition_variable condition;
        bool gate_started = false;
        bool gate_canceled = false;
        int dropped = 0;

        tts::SynthesisScheduler::Job gate;
        gate.run = [&](const std::atomic_bool& cancel) {
            std::unique_lock lock(mutex);
            gate_started = true;
            condition.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
                condition.wait_for(lock, 5ms);
            }
            gate_canceled = cancel.load();
        };
        ASSERT_TRUE(scheduler.submit(std::move(gate)));
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] {
                return gate_started;
            });
        }

        tts::SynthesisScheduler::Job pending;
        pending.on_dropped = [&] {
            const std::lock_guard lock(mutex);
            ++dropped;
        };
        ASSERT_TRUE(scheduler.submit(std::move(pending)));

        scheduler.shutdown();

        {
            const std::lock_guard lock(mutex);
            EXPECT_TRUE(gate_canceled);
            EXPECT_EQ(dropped, 1);
        }
        EXPECT_FALSE(scheduler.submit(tts::SynthesisScheduler::Job{}));
    }

} // namespace
