#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tts {

    struct ProcessOutcome {
        int exit_code{-1};
        bool canceled{false};
        bool timed_out{false};
        /// 子进程 stderr；上限 64 KiB。
        std::string stderr_text;
    };

    /// 运行一个子进程，文本经内存文件送入 stdin，stderr 被捕获。
    ///
    /// - argv[0] 可以是 PATH 中的名字；
    /// - timeout 到期或 cancel 置位时杀掉整个进程组；
    /// - stdout 重定向到 /dev/null；后端应把音频写到文件。
    [[nodiscard]] std::expected<ProcessOutcome, std::string>
    run_process(const std::vector<std::string>& argv,
                std::string_view stdin_text,
                const std::atomic_bool* cancel = nullptr,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{30000});

} // namespace tts
