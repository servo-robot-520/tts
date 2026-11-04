#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace tts {

    struct WavInfo {
        std::uint32_t sample_rate{0};
        std::uint16_t channels{0};
        std::uint16_t bits_per_sample{0};
        std::uint64_t sample_frames{0};
        float duration_seconds{0.0F};
    };

    /// 写 16-bit PCM 单声道/多声道 WAV；samples 为交错排列。
    [[nodiscard]] std::expected<WavInfo, std::string>
    write_wav_pcm16(const std::filesystem::path& path,
                    std::span<const std::int16_t> samples,
                    std::uint32_t sample_rate,
                    std::uint16_t channels = 1);

    /// 读取 WAV 头并计算时长；只接受 PCM 格式。
    [[nodiscard]] std::expected<WavInfo, std::string>
    read_wav_info(const std::filesystem::path& path);

} // namespace tts
