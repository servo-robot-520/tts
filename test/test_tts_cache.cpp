#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "tts/tts_cache.hpp"
#include "tts/wav_io.hpp"

namespace {

    class TtsCacheTest : public ::testing::Test {
    protected:
        void SetUp() override {
            directory_ = std::filesystem::temp_directory_path() /
                ("tts_cache_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++));
            std::filesystem::remove_all(directory_);
        }

        void TearDown() override {
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
        }

        std::filesystem::path write_clip(tts::TtsCache& cache, const std::string& key,
                                         const std::size_t samples) const {
            const auto temporary = cache.temp_path(key);
            std::vector<std::int16_t> audio(samples, 0);
            const auto written = tts::write_wav_pcm16(temporary, audio, 22050);
            EXPECT_TRUE(written.has_value()) << written.error();
            const auto committed = cache.commit(temporary, key);
            EXPECT_TRUE(committed.has_value()) << committed.error();
            return cache.wav_path(key);
        }

        std::filesystem::path directory_;
        static inline int counter_{0};
    };

    TEST_F(TtsCacheTest, MakeKeyIsStableAndDistinct) {
        const auto first = tts::TtsCache::make_key({"piper", "text", "voice", "1.0000"});
        const auto second = tts::TtsCache::make_key({"piper", "text", "voice", "1.0000"});
        const auto other = tts::TtsCache::make_key({"piper", "text", "voice", "2.0000"});

        EXPECT_EQ(first, second);
        EXPECT_NE(first, other);
        EXPECT_EQ(first.size(), 64U);
    }

    TEST_F(TtsCacheTest, CommitThenLookupReturnsDuration) {
        tts::TtsCache cache(directory_, 0);
        const std::string key = tts::TtsCache::make_key({"text"});
        const auto path = write_clip(cache, key, 4410);

        ASSERT_TRUE(std::filesystem::is_regular_file(path));
        const auto info = cache.lookup(key);
        ASSERT_TRUE(info.has_value());
        EXPECT_NEAR(info->duration_seconds, 0.2F, 0.001F);
    }

    TEST_F(TtsCacheTest, LookupDropsCorruptedEntry) {
        tts::TtsCache cache(directory_, 0);
        const std::string key = tts::TtsCache::make_key({"broken"});
        const auto path = cache.wav_path(key);

        std::ofstream stream(path, std::ios::binary);
        stream << "not a wav";
        stream.close();

        EXPECT_FALSE(cache.lookup(key).has_value());
        EXPECT_FALSE(std::filesystem::exists(path));
    }

    TEST_F(TtsCacheTest, EvictsOldestEntries) {
        tts::TtsCache cache(directory_, 2000);
        const std::string old_key = tts::TtsCache::make_key({"old"});
        const std::string new_key = tts::TtsCache::make_key({"new"});

        const auto old_path = write_clip(cache, old_key, 500);
        const auto new_path = write_clip(cache, new_key, 500);

        std::error_code error;
        std::filesystem::last_write_time(
            old_path, std::filesystem::file_time_type::clock::now() - std::chrono::hours{1},
            error);
        ASSERT_FALSE(error);

        cache.evict();

        EXPECT_FALSE(std::filesystem::exists(old_path));
        EXPECT_TRUE(std::filesystem::exists(new_path));
    }

    TEST_F(TtsCacheTest, TempPathsAreUnique) {
        tts::TtsCache cache(directory_, 0);
        const std::string key = tts::TtsCache::make_key({"text"});
        EXPECT_NE(cache.temp_path(key), cache.temp_path(key));
    }

} // namespace
