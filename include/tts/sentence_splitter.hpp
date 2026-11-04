#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tts {

    /// 把文本切成适合逐句合成的片段。
    ///
    /// - 在 `.` `!` `?` `。` `！` `？` `；` `;` 和换行后切句；
    /// - `.` 后紧跟数字（3.14）时不切；
    /// - 单句超过 max_chars 个码点时硬切，优先在切点前的空白处断开；
    /// - 丢弃纯空白片段，保留原句标点。
    [[nodiscard]] std::vector<std::string> split_sentences(std::string_view text,
                                                           std::size_t max_chars);

} // namespace tts
