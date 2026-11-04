#include "tts/sentence_splitter.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace tts {

    namespace {

        /// 解码一个 UTF-8 码点；非法字节按单字节 U+FFFD 处理。
        std::pair<char32_t, std::size_t> decode_utf8(const std::string_view text, const std::size_t offset) {
            const auto byte = [&](const std::size_t index) -> unsigned char {
                return static_cast<unsigned char>(text[index]);
            };
            const unsigned char first = byte(offset);
            if (first < 0x80U) {
                return {first, 1};
            }

            std::size_t length = 0;
            char32_t value = 0;
            if ((first & 0xE0U) == 0xC0U) {
                length = 2;
                value = first & 0x1FU;
            } else if ((first & 0xF0U) == 0xE0U) {
                length = 3;
                value = first & 0x0FU;
            } else if ((first & 0xF8U) == 0xF0U) {
                length = 4;
                value = first & 0x07U;
            } else {
                return {0xFFFDU, 1};
            }

            if (offset + length > text.size()) {
                return {0xFFFDU, 1};
            }
            for (std::size_t i = 1; i < length; ++i) {
                const unsigned char continuation = byte(offset + i);
                if ((continuation & 0xC0U) != 0x80U) {
                    return {0xFFFDU, 1};
                }
                value = (value << 6U) | (continuation & 0x3FU);
            }
            return {value, length};
        }

        bool is_space(const char32_t character) {
            return character == U' ' || character == U'\t' || character == U'\r' ||
                   character == U'\n' || character == U'\v' || character == U'\f';
        }

        bool is_digit(const char32_t character) {
            return character >= U'0' && character <= U'9';
        }

        bool is_terminator(const char32_t character) {
            switch (character) {
                case U'.':
                case U'!':
                case U'?':
                case U';':
                case U'\n':
                case U'\u3002': // 。
                case U'\uFF01': // ！
                case U'\uFF1F': // ？
                case U'\uFF1B': // ；
                    return true;
                default:
                    return false;
            }
        }

    } // namespace

    std::vector<std::string> split_sentences(const std::string_view text, const std::size_t max_chars) {
        std::vector<std::string> sentences;
        if (text.empty()) {
            return sentences;
        }

        std::vector<char32_t> codepoints;
        std::vector<std::size_t> offsets;
        codepoints.reserve(text.size());
        offsets.reserve(text.size() + 1);
        for (std::size_t offset = 0; offset < text.size();) {
            const auto [character, length] = decode_utf8(text, offset);
            offsets.push_back(offset);
            codepoints.push_back(character);
            offset += length;
        }
        offsets.push_back(text.size());

        const std::size_t limit = max_chars == 0 ? std::numeric_limits<std::size_t>::max() : max_chars;

        const auto emit = [&](std::size_t begin, std::size_t end) {
            while (begin < end && is_space(codepoints[begin])) {
                ++begin;
            }
            while (end > begin && is_space(codepoints[end - 1])) {
                --end;
            }
            if (begin == end) {
                return;
            }
            sentences.emplace_back(text.substr(offsets[begin], offsets[end] - offsets[begin]));
        };

        std::size_t begin = 0;
        for (std::size_t index = 0; index < codepoints.size(); ++index) {
            const char32_t character = codepoints[index];
            if (is_terminator(character)) {
                bool should_break = true;
                if (character == U'.' && index + 1 < codepoints.size() && is_digit(codepoints[index + 1])) {
                    should_break = false;
                } else if (character == U'.' && index + 1 < codepoints.size() &&
                           codepoints[index + 1] == U'.') {
                    should_break = false;
                }
                if (should_break) {
                    emit(begin, index + 1);
                    begin = index + 1;
                    continue;
                }
            }

            if (index + 1 - begin < limit) {
                continue;
            }

            // 硬切：优先切在最近的空白处，但不早于半句，避免把词切碎。
            std::size_t cut = index + 1;
            const std::size_t earliest = begin + limit / 2;
            for (std::size_t candidate = cut; candidate > earliest; --candidate) {
                if (is_space(codepoints[candidate - 1])) {
                    cut = candidate;
                    break;
                }
            }
            emit(begin, cut);
            begin = cut;
        }
        emit(begin, codepoints.size());
        return sentences;
    }

} // namespace tts
