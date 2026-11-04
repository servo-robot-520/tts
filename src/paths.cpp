#include "tts/paths.hpp"

#include <cstdlib>
#include <string>

namespace tts {

    std::filesystem::path expand_user_path(const std::string_view raw) {
        if (raw.empty()) {
            return {};
        }

        const std::string value{raw};
        const char* home_environment = std::getenv("HOME");
        const std::string home = home_environment != nullptr ? home_environment : "";
        if (home.empty()) {
            return value;
        }

        if (value == "~") {
            return home;
        }
        if (value.rfind("~/", 0) == 0) {
            return home + value.substr(1);
        }
        if (value.rfind("${HOME}", 0) == 0) {
            return home + value.substr(7);
        }
        if (value.rfind("$HOME", 0) == 0) {
            return home + value.substr(5);
        }
        return value;
    }

} // namespace tts
