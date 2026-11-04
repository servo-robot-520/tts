#include "tts/tts_cache.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace tts {

    namespace {

        std::string sha256_hex(const std::string_view data) {
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int digest_length = 0;

            EVP_MD_CTX* context = EVP_MD_CTX_new();
            if (context == nullptr) {
                throw std::runtime_error("EVP_MD_CTX_new failed");
            }
            const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                            EVP_DigestUpdate(context, data.data(), data.size()) == 1 &&
                            EVP_DigestFinal_ex(context, digest.data(), &digest_length) == 1;
            EVP_MD_CTX_free(context);
            if (!ok) {
                throw std::runtime_error("SHA-256 digest failed");
            }

            static constexpr char kHex[] = "0123456789abcdef";
            std::string result;
            result.reserve(static_cast<std::size_t>(digest_length) * 2U);
            for (unsigned int i = 0; i < digest_length; ++i) {
                result.push_back(kHex[digest[i] >> 4U]);
                result.push_back(kHex[digest[i] & 0x0FU]);
            }
            return result;
        }

        constexpr std::string_view kWavSuffix = ".wav";

    } // namespace

    TtsCache::TtsCache(std::filesystem::path directory, const std::uint64_t max_bytes)
        : directory_(std::move(directory)), max_bytes_(max_bytes) {
        std::error_code error;
        std::filesystem::create_directories(directory_, error);
        if (error) {
            throw std::runtime_error("cannot create TTS cache directory " + directory_.string() +
                                     ": " + error.message());
        }
    }

    std::string TtsCache::make_key(const std::initializer_list<std::string_view> parts) {
        std::string material;
        for (const auto part : parts) {
            material.append(part);
            material.push_back('\x1f');
        }
        return sha256_hex(material);
    }

    std::filesystem::path TtsCache::wav_path(const std::string_view key) const {
        return directory_ / (std::string{key} + std::string{kWavSuffix});
    }

    std::filesystem::path TtsCache::temp_path(const std::string_view key) {
        const std::lock_guard lock(mutex_);
        return directory_ / (std::string{key} + ".tmp-" + std::to_string(temp_counter_++));
    }

    std::optional<WavInfo> TtsCache::lookup(const std::string_view key) const {
        const std::lock_guard lock(mutex_);
        const auto path = wav_path(key);
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return std::nullopt;
        }
        auto info = read_wav_info(path);
        if (!info) {
            std::filesystem::remove(path, error);
            return std::nullopt;
        }

        // 命中即刷新 mtime，让 LRU 反映最近使用顺序。
        const auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(path, now, error);
        return *info;
    }

    std::expected<void, std::string>
    TtsCache::commit(const std::filesystem::path& temporary, const std::string_view key) const {
        const std::lock_guard lock(mutex_);
        const auto target = wav_path(key);
        std::error_code error;

        if (std::filesystem::is_regular_file(target, error)) {
            std::filesystem::remove(temporary, error);
            return {};
        }

        std::filesystem::rename(temporary, target, error);
        if (error) {
            return std::unexpected("cannot move " + temporary.string() + " to " + target.string() +
                                   ": " + error.message());
        }
        return {};
    }

    void TtsCache::evict() const {
        if (max_bytes_ == 0) {
            return;
        }

        struct Entry {
            std::filesystem::path path;
            std::uint64_t bytes{0};
            std::filesystem::file_time_type modified{};
        };

        const std::lock_guard lock(mutex_);
        std::vector<Entry> entries;
        std::uint64_t total_bytes = 0;

        std::error_code error;
        for (std::filesystem::directory_iterator iterator(directory_, error), end;
             !error && iterator != end; iterator.increment(error)) {
            const auto& entry = *iterator;
            if (!entry.is_regular_file(error)) {
                continue;
            }
            if (entry.path().extension() != kWavSuffix) {
                continue;
            }
            Entry candidate;
            candidate.path = entry.path();
            candidate.bytes = entry.file_size(error);
            candidate.modified = entry.last_write_time(error);
            if (error) {
                error.clear();
                continue;
            }
            total_bytes += candidate.bytes;
            entries.push_back(std::move(candidate));
        }

        if (total_bytes <= max_bytes_) {
            return;
        }

        std::ranges::sort(entries, {}, &Entry::modified);
        for (const auto& entry : entries) {
            if (total_bytes <= max_bytes_) {
                break;
            }
            std::filesystem::remove(entry.path, error);
            if (!error) {
                total_bytes -= std::min(total_bytes, entry.bytes);
            }
            error.clear();
        }
    }

} // namespace tts
