#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tts/process_runner.hpp"

namespace {

    using namespace std::chrono_literals;

    TEST(ProcessRunnerTest, RunsProcessAndFeedsStdin) {
        const auto outcome = tts::run_process({"/bin/cat"}, "hello", nullptr, 5000ms);
        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_FALSE(outcome->canceled);
        EXPECT_FALSE(outcome->timed_out);
        EXPECT_EQ(outcome->exit_code, 0);
    }

    TEST(ProcessRunnerTest, ReportsExitCodeAndStderr) {
        const auto outcome =
            tts::run_process({"/bin/sh", "-c", "echo boom >&2; exit 3"}, "", nullptr, 5000ms);
        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_EQ(outcome->exit_code, 3);
        EXPECT_NE(outcome->stderr_text.find("boom"), std::string::npos);
    }

    TEST(ProcessRunnerTest, ResolvesExecutableFromPath) {
        const auto outcome = tts::run_process({"true"}, "", nullptr, 5000ms);
        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_EQ(outcome->exit_code, 0);
    }

    TEST(ProcessRunnerTest, ReportsExecFailure) {
        const auto outcome =
            tts::run_process({"/nonexistent/tts-binary"}, "", nullptr, 5000ms);
        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_EQ(outcome->exit_code, 127);
        EXPECT_NE(outcome->stderr_text.find("execvp"), std::string::npos);
    }

    TEST(ProcessRunnerTest, TimesOut) {
        const auto outcome = tts::run_process({"sleep", "5"}, "", nullptr, 100ms);
        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_TRUE(outcome->timed_out);
        EXPECT_FALSE(outcome->canceled);
    }

    TEST(ProcessRunnerTest, CancelsRunningProcess) {
        std::atomic_bool cancel{false};
        std::thread trigger([&cancel] {
            std::this_thread::sleep_for(100ms);
            cancel.store(true);
        });

        const auto outcome = tts::run_process({"sleep", "5"}, "", &cancel, 5000ms);
        trigger.join();

        ASSERT_TRUE(outcome.has_value()) << outcome.error();
        EXPECT_TRUE(outcome->canceled);
        EXPECT_FALSE(outcome->timed_out);
    }

} // namespace
