#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "tts/wav_io.hpp"

namespace tts {

    /// TTS 合成缓存：每个键一个 WAV，按最后访问时间做 LRU 清理。
    class TtsCache {
    public:
        /// @param directory 缓存目录，不存在时创建
        /// @param max_bytes 总上限；0 = 不限制
        TtsCache(std::filesystem::path directory, std::uint64_t max_bytes);

        /// 确定性缓存键：SHA-256(parts 以 0x1f 连接)，十六进制。
        [[nodiscard]] static std::string make_key(std::initializer_list<std::string_view> parts);

        [[nodiscard]] const std::filesystem::path& directory() const noexcept {
            return directory_;
        }

        [[nodiscard]] std::filesystem::path wav_path(std::string_view key) const;

        /// 为新合成分配唯一的临时路径（同目录，保证 rename 原子）。
        [[nodiscard]] std::filesystem::path temp_path(std::string_view key);

        /// 命中时刷新 mtime 并返回 WAV 信息；不存在或损坏返回 nullopt（损坏文件会被删除）。
        [[nodiscard]] std::optional<WavInfo> lookup(std::string_view key) const;

        /// 把临时文件原子改名为 <key>.wav。
        [[nodiscard]] std::expected<void, std::string>
        commit(const std::filesystem::path& temporary, std::string_view key) const;

        /// 按 mtime 从旧到新删除，直到总量不超过 max_bytes。
        void evict() const;

    private:
        std::filesystem::path directory_;
        std::uint64_t max_bytes_;
        mutable std::mutex mutex_;
        mutable std::uint64_t temp_counter_{0};
    };

} // namespace tts
