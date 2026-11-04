#include "tts/piper_backend.hpp"

#include <cstdlib>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "tts/process_runner.hpp"
#include "tts/tts_backend.hpp"
#include "tts/wav_io.hpp"

namespace tts {

    namespace {

        std::optional<std::string> resolve_executable(const std::string& name) {
            if (name.empty()) {
                return std::nullopt;
            }
            std::error_code error;
            if (name.find('/') != std::string::npos) {
                if (::access(name.c_str(), X_OK) == 0) {
                    return name;
                }
                return std::nullopt;
            }

            const char* path_environment = std::getenv("PATH");
            if (path_environment == nullptr) {
                return std::nullopt;
            }
            std::string_view paths{path_environment};
            while (!paths.empty()) {
                const auto separator = paths.find(':');
                const auto directory = paths.substr(0, separator);
                if (!directory.empty()) {
                    const std::string candidate =
                            (std::filesystem::path{directory} / name).string();
                    if (::access(candidate.c_str(), X_OK) == 0) {
                        return candidate;
                    }
                }
                if (separator == std::string_view::npos) {
                    break;
                }
                paths.remove_prefix(separator + 1);
            }
            return std::nullopt;
        }

        std::optional<int> parse_positive_int(const std::string_view text) {
            if (text.empty()) {
                return std::nullopt;
            }
            int value = 0;
            for (const char character : text) {
                if (character < '0' || character > '9') {
                    return std::nullopt;
                }
                if (value > 100000) {
                    return std::nullopt;
                }
                value = value * 10 + (character - '0');
            }
            return value;
        }

        std::string format_scale(const float value) {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << std::fixed << std::setprecision(4) << value;
            return stream.str();
        }

        int resolve_speaker_id(const PiperOptions& options, const SynthesisRequest& request) {
            if (const auto from_voice = parse_positive_int(request.voice_id)) {
                return *from_voice;
            }
            return options.speaker_id;
        }

        std::string last_stderr(const std::string_view text) {
            constexpr std::size_t kMaxMessage = 1000;
            if (text.size() <= kMaxMessage) {
                return std::string{text};
            }
            return std::string{text.substr(text.size() - kMaxMessage)};
        }

    } // namespace

    std::vector<std::string> piper_arguments(const PiperOptions& options,
                                             const SynthesisRequest& request) {
        std::vector<std::string> arguments;
        arguments.emplace_back("--model");
        arguments.push_back(options.model.string());
        arguments.emplace_back("--output_file");
        arguments.push_back(request.output_wav.string());

        if (!options.config.empty()) {
            arguments.emplace_back("--config");
            arguments.push_back(options.config.string());
        }

        const int speaker_id = resolve_speaker_id(options, request);
        if (speaker_id >= 0) {
            arguments.emplace_back("--speaker");
            arguments.push_back(std::to_string(speaker_id));
        }

        if (request.rate > 0.0F && request.rate != 1.0F) {
            arguments.emplace_back("--length_scale");
            arguments.push_back(format_scale(1.0F / request.rate));
        }

        if (!options.espeak_data.empty()) {
            arguments.emplace_back("--espeak_data");
            arguments.push_back(options.espeak_data);
        }
        return arguments;
    }

    PiperBackend::PiperBackend(PiperOptions options) : options_(std::move(options)) {}

    std::string_view PiperBackend::name() const noexcept {
        return "piper";
    }

    std::expected<void, std::string> PiperBackend::initialize() {
        const auto executable = resolve_executable(options_.executable);
        if (!executable) {
            return std::unexpected("piper executable not found: " + options_.executable);
        }
        resolved_executable_ = *executable;

        if (options_.model.empty()) {
            return std::unexpected("piper.model is empty");
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(options_.model, error)) {
            return std::unexpected("piper model not found: " + options_.model.string());
        }
        if (!options_.config.empty() &&
            !std::filesystem::is_regular_file(options_.config, error)) {
            return std::unexpected("piper config not found: " + options_.config.string());
        }
        if (options_.timeout.count() <= 0) {
            return std::unexpected("piper.timeout_ms must be greater than zero");
        }
        return {};
    }

    std::string PiperBackend::fingerprint() const {
        std::error_code error;
        const auto modified = std::filesystem::last_write_time(options_.model, error);
        const auto stamp = error ? 0 : modified.time_since_epoch().count();
        return "piper|" + resolved_executable_ + "|" + options_.model.string() + "|" +
               std::to_string(static_cast<long long>(stamp)) + "|" + options_.config.string() +
               "|" + options_.espeak_data + "|" + std::to_string(options_.speaker_id);
    }

    std::expected<SynthesisResult, BackendError>
    PiperBackend::synthesize(const SynthesisRequest& request) {
        if (request.text.empty()) {
            return std::unexpected(BackendError{BackendErrorKind::InvalidRequest,
                                                "text is empty"});
        }
        if (!request.voice_id.empty() && !parse_positive_int(request.voice_id)) {
            return std::unexpected(BackendError{
                    BackendErrorKind::InvalidRequest,
                    "piper voice_id must be empty or a numeric speaker id, got: " +
                            request.voice_id});
        }
        if (request.output_wav.empty()) {
            return std::unexpected(BackendError{BackendErrorKind::InvalidRequest,
                                                "output_wav is empty"});
        }

        std::error_code error;
        std::filesystem::create_directories(request.output_wav.parent_path(), error);
        std::filesystem::remove(request.output_wav, error);

        std::vector<std::string> arguments = piper_arguments(options_, request);
        arguments.insert(arguments.begin(), resolved_executable_);

        const auto outcome = run_process(arguments, request.text, request.cancel, options_.timeout);
        if (!outcome) {
            return std::unexpected(BackendError{BackendErrorKind::Failed, outcome.error()});
        }
        if (outcome->canceled) {
            return std::unexpected(BackendError{BackendErrorKind::Canceled,
                                                "piper synthesis canceled"});
        }
        if (outcome->timed_out) {
            return std::unexpected(BackendError{
                    BackendErrorKind::Timeout,
                    "piper timed out after " + std::to_string(options_.timeout.count()) + " ms"});
        }
        if (outcome->exit_code != 0) {
            return std::unexpected(BackendError{
                    BackendErrorKind::Failed,
                    "piper exited with " + std::to_string(outcome->exit_code) + ": " +
                            last_stderr(outcome->stderr_text)});
        }

        const auto info = read_wav_info(request.output_wav);
        if (!info) {
            return std::unexpected(BackendError{BackendErrorKind::Failed, info.error()});
        }
        if (info->sample_frames == 0) {
            return std::unexpected(BackendError{BackendErrorKind::Failed,
                                                "piper produced empty audio"});
        }

        SynthesisResult result;
        result.wav_path = request.output_wav;
        result.duration_seconds = info->duration_seconds;
        result.sample_rate = info->sample_rate;
        return result;
    }

} // namespace tts
