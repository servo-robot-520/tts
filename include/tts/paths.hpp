#pragma once

#include <filesystem>
#include <string_view>

namespace tts {

    /// 展开开头的 `~`、`$HOME`、`${HOME}`；其他内容原样保留。
    [[nodiscard]] std::filesystem::path expand_user_path(std::string_view raw);

} // namespace tts
