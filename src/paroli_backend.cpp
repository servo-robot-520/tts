#include "tts/paroli_backend.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <curl/curl.h>

#include "tts/tts_backend.hpp"
#include "tts/wav_io.hpp"

namespace tts {

    namespace {

        constexpr std::string_view kSynthesizePath = "/api/v1/synthesise";

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

        void append_json_string(std::string& output, const std::string_view value) {
            static constexpr char kHex[] = "0123456789abcdef";
            output.push_back('"');
            for (const unsigned char character : value) {
                switch (character) {
                case '"':
                    output.append("\\\"");
                    break;
                case '\\':
                    output.append("\\\\");
                    break;
                case '\b':
                    output.append("\\b");
                    break;
                case '\f':
                    output.append("\\f");
                    break;
                case '\n':
                    output.append("\\n");
                    break;
                case '\r':
                    output.append("\\r");
                    break;
                case '\t':
                    output.append("\\t");
                    break;
                default:
                    if (character < 0x20U) {
                        output.append("\\u00");
                        output.push_back(kHex[character >> 4U]);
                        output.push_back(kHex[character & 0x0FU]);
                    }
                    else {
                        output.push_back(static_cast<char>(character));
                    }
                    break;
                }
            }
            output.push_back('"');
        }

        std::string format_scale(const float value) {
            char buffer[32] = {};
            std::snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(value));
            return buffer;
        }

        int resolve_speaker_id(const ParoliOptions& options, const SynthesisRequest& request) {
            if (const auto from_voice = parse_positive_int(request.voice_id)) {
                return *from_voice;
            }
            return options.speaker_id;
        }

        std::size_t write_body(const char* data, const std::size_t size, const std::size_t count,
                               void* user_data) {
            auto* body = static_cast<std::vector<unsigned char>*>(user_data);
            const std::size_t bytes = size * count;
            body->insert(body->end(),
                         reinterpret_cast<const unsigned char*>(data),
                         reinterpret_cast<const unsigned char*>(data) + bytes);
            return bytes;
        }

        int progress_body(void* user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            const auto* cancel = static_cast<const std::atomic_bool*>(user_data);
            return cancel != nullptr && cancel->load() ? 1 : 0;
        }

        std::string http_body_preview(const std::vector<unsigned char>& body) {
            constexpr std::size_t kMaxPreview = 200;
            const std::size_t length = std::min(body.size(), kMaxPreview);
            std::string preview(reinterpret_cast<const char*>(body.data()), length);
            for (char& character : preview) {
                if (character == '\n' || character == '\r') {
                    character = ' ';
                }
            }
            return preview;
        }

    } // namespace

    std::string paroli_synthesize_url(const ParoliOptions& options) {
        std::string base = options.url;
        while (!base.empty() && base.back() == '/') {
            base.pop_back();
        }
        return base + std::string{kSynthesizePath};
    }

    std::string paroli_request_json(const ParoliOptions& options,
                                    const SynthesisRequest& request) {
        std::string json{"{\"text\":"};
        append_json_string(json, request.text);
        json.append(",\"audio_format\":\"pcm\"");

        const int speaker_id = resolve_speaker_id(options, request);
        if (speaker_id >= 0) {
            json.append(",\"speaker_id\":");
            json.append(std::to_string(speaker_id));
        }
        if (request.rate > 0.0F && request.rate != 1.0F) {
            json.append(",\"length_scale\":");
            json.append(format_scale(1.0F / request.rate));
        }
        json.push_back('}');
        return json;
    }

    ParoliBackend::ParoliBackend(ParoliOptions options) :
        options_(std::move(options)) {
    }

    std::string_view ParoliBackend::name() const noexcept {
        return "paroli";
    }

    std::expected<void, std::string> ParoliBackend::initialize() {
        if (options_.url.rfind("http://", 0) != 0 && options_.url.rfind("https://", 0) != 0) {
            return std::unexpected("paroli.url must start with http:// or https://, got: " +
                                   options_.url);
        }
        if (options_.sample_rate == 0) {
            return std::unexpected("paroli.sample_rate must be greater than zero");
        }
        if (options_.timeout.count() <= 0) {
            return std::unexpected("paroli.timeout_ms must be greater than zero");
        }
        return {};
    }

    std::string ParoliBackend::fingerprint() const {
        return "paroli|" + options_.url + "|" + std::to_string(options_.sample_rate) + "|" +
            std::to_string(options_.speaker_id);
    }

    std::expected<SynthesisResult, BackendError>
    ParoliBackend::synthesize(const SynthesisRequest& request) {
        if (request.text.empty()) {
            return std::unexpected(BackendError{BackendErrorKind::InvalidRequest,
                                                "text is empty"});
        }
        if (!request.voice_id.empty() && !parse_positive_int(request.voice_id)) {
            return std::unexpected(BackendError{
                BackendErrorKind::InvalidRequest,
                "paroli voice_id must be empty or a numeric speaker id, got: " +
                    request.voice_id});
        }
        if (request.output_wav.empty()) {
            return std::unexpected(BackendError{BackendErrorKind::InvalidRequest,
                                                "output_wav is empty"});
        }

        static std::once_flag curl_once;
        std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            return std::unexpected(BackendError{BackendErrorKind::Failed,
                                                "curl_easy_init failed"});
        }

        const std::string url = paroli_synthesize_url(options_);
        const std::string json = paroli_request_json(options_, request);
        std::vector<unsigned char> body;

        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: audio/raw");
        headers = curl_slist_append(headers, "Expect:");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options_.timeout.count()));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_body);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, request.cancel);

        const CURLcode performed = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (is_canceled(request) || performed == CURLE_ABORTED_BY_CALLBACK) {
            return std::unexpected(BackendError{BackendErrorKind::Canceled,
                                                "paroli synthesis canceled"});
        }
        if (performed == CURLE_OPERATION_TIMEDOUT) {
            return std::unexpected(BackendError{
                BackendErrorKind::Timeout,
                "paroli timed out after " + std::to_string(options_.timeout.count()) + " ms"});
        }
        if (performed != CURLE_OK) {
            return std::unexpected(BackendError{
                BackendErrorKind::Failed,
                std::string{"paroli request failed: "} + curl_easy_strerror(performed)});
        }
        if (response_code != 200) {
            return std::unexpected(BackendError{
                BackendErrorKind::Failed,
                "paroli-server returned HTTP " + std::to_string(response_code) + ": " +
                    http_body_preview(body)});
        }
        if (body.empty()) {
            return std::unexpected(BackendError{BackendErrorKind::Failed,
                                                "paroli-server returned empty PCM"});
        }
        if ((body.size() % 2U) != 0U) {
            return std::unexpected(BackendError{BackendErrorKind::Failed,
                                                "paroli-server returned truncated 16-bit PCM"});
        }

        std::vector<std::int16_t> samples(body.size() / 2U);
        std::memcpy(samples.data(), body.data(), body.size());

        std::error_code error;
        std::filesystem::create_directories(request.output_wav.parent_path(), error);
        std::filesystem::remove(request.output_wav, error);

        const auto written = write_wav_pcm16(request.output_wav, samples, options_.sample_rate);
        if (!written) {
            return std::unexpected(BackendError{BackendErrorKind::Failed, written.error()});
        }

        SynthesisResult result;
        result.wav_path = request.output_wav;
        result.duration_seconds = written->duration_seconds;
        result.sample_rate = written->sample_rate;
        return result;
    }

} // namespace tts
