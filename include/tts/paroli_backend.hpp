#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "tts/tts_backend.hpp"

namespace tts {

    struct ParoliOptions {
        /// paroli-server 根地址，例如 http://127.0.0.1:8848。
        std::string url{"http://127.0.0.1:8848"};
        /// paroli 返回裸 PCM，响应里没有采样率；必须与模型的原生采样率一致。
        std::uint32_t sample_rate{22050};
        /// 默认 speaker id；< 0 表示不发送 speaker_id。
        int speaker_id{-1};
        /// HTTP 请求超时。
        std::chrono::milliseconds timeout{30000};
    };

    /// paroli-server 的合成接口地址（/api/v1/synthesise）。
    [[nodiscard]] std::string paroli_synthesize_url(const ParoliOptions& options);

    /// 构造 POST /api/v1/synthesise 的 JSON 请求体，暴露给单元测试。
    [[nodiscard]] std::string paroli_request_json(const ParoliOptions& options,
                                                  const SynthesisRequest& request);

    /// 通过 paroli-server 的 REST API 合成：请求 PCM，写入 WAV 缓存文件。
    class ParoliBackend final : public TtsBackend {
    public:
        explicit ParoliBackend(ParoliOptions options);

        [[nodiscard]] std::string_view name() const noexcept override;
        [[nodiscard]] std::string fingerprint() const override;
        [[nodiscard]] std::expected<void, std::string> initialize() override;
        [[nodiscard]] std::expected<SynthesisResult, BackendError>
        synthesize(const SynthesisRequest& request) override;

    private:
        ParoliOptions options_;
    };

} // namespace tts
