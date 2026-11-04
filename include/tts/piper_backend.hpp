#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tts/tts_backend.hpp"

namespace tts {

    struct PiperOptions {
        /// 可执行文件路径或 PATH 中的名字。
        std::string executable{"piper"};
        /// 必需：piper voice 的 .onnx 模型。
        std::filesystem::path model;
        /// 可选：模型配置；空时由 piper 使用 <model>.json。
        std::filesystem::path config;
        /// 可选：espeak-ng-data 目录；空时由 piper 在自身目录旁查找。
        std::string espeak_data;
        /// 默认 speaker id；< 0 表示不传 --speaker。
        int speaker_id{-1};
        /// 单次合成超时。
        std::chrono::milliseconds timeout{30000};
    };

    /// 构造 piper CLI 参数（不含 argv[0]），暴露给单元测试。
    [[nodiscard]] std::vector<std::string> piper_arguments(const PiperOptions& options,
                                                           const SynthesisRequest& request);

    /// 通过 piper CLI 子进程合成：stdin 输入文本，输出 WAV 文件。
    class PiperBackend final : public TtsBackend {
    public:
        explicit PiperBackend(PiperOptions options);

        [[nodiscard]] std::string_view name() const noexcept override;
        [[nodiscard]] std::string fingerprint() const override;
        [[nodiscard]] std::expected<void, std::string> initialize() override;
        [[nodiscard]] std::expected<SynthesisResult, BackendError>
        synthesize(const SynthesisRequest& request) override;

    private:
        PiperOptions options_;
        std::string resolved_executable_;
    };

} // namespace tts
