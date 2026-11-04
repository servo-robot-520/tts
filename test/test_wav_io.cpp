#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "tts/wav_io.hpp"

namespace {

    std::filesystem::path temp_wav_path(const std::string& name) {
        const auto path = std::filesystem::temp_directory_path() /
            ("tts_wav_test_" + std::to_string(::getpid()) + "_" + name);
        std::filesystem::remove(path);
        return path;
    }

    TEST(WavIo, WritesAndReadsPcm16) {
        const auto path = temp_wav_path("roundtrip.wav");
        std::vector<std::int16_t> samples(2205);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<std::int16_t>(i % 100);
        }

        const auto written = tts::write_wav_pcm16(path, samples, 22050);
        ASSERT_TRUE(written.has_value()) << written.error();
        EXPECT_EQ(written->sample_rate, 22050U);
        EXPECT_EQ(written->channels, 1U);
        EXPECT_EQ(written->bits_per_sample, 16U);
        EXPECT_EQ(written->sample_frames, samples.size());
        EXPECT_NEAR(written->duration_seconds, 0.1F, 0.001F);

        const auto read = tts::read_wav_info(path);
        ASSERT_TRUE(read.has_value()) << read.error();
        EXPECT_EQ(read->sample_rate, written->sample_rate);
        EXPECT_EQ(read->sample_frames, written->sample_frames);
        EXPECT_NEAR(read->duration_seconds, written->duration_seconds, 0.001F);

        std::filesystem::remove(path);
    }

    TEST(WavIo, RejectsNonWavFile) {
        const auto path = temp_wav_path("garbage.wav");
        std::ofstream stream(path, std::ios::binary);
        stream << "this is not a wav file";
        stream.close();

        EXPECT_FALSE(tts::read_wav_info(path).has_value());
        std::filesystem::remove(path);
    }

    TEST(WavIo, RejectsZeroSampleRate) {
        const auto path = temp_wav_path("zero_rate.wav");
        const std::array<std::int16_t, 8> samples{};
        EXPECT_FALSE(tts::write_wav_pcm16(path, samples, 0).has_value());
        EXPECT_FALSE(std::filesystem::exists(path));
    }

    TEST(WavIo, RejectsMissingFile) {
        EXPECT_FALSE(tts::read_wav_info(temp_wav_path("missing.wav")).has_value());
    }

} // namespace
