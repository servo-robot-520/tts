#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace tts {

    /// 一次合成请求。node 已把 TtsText 的 0/空值归一化为具体参数。
    struct SynthesisRequest {
        std::string text;
        /// 空 = 后端默认音色；数字字符串 = 多说话人模型的 speaker id。
        std::string voice_id;
        /// BCP-47 标签；当前后端只把它写进缓存键，合成本身由模型决定。
        std::string language;
        /// 语速倍率，已归一化到 [0.25, 4.0]，1.0 = 后端默认。
        float rate{1.0F};
        /// 音高倍率，已归一化到 [0.5, 2.0]；当前后端不支持，忽略。
        float pitch{1.0F};
        /// 输出 WAV 路径；由调用方提供（缓存临时文件）。
        std::filesystem::path output_wav;
        /// 可选协作式取消标志；后端轮询它并尽快终止子进程/HTTP 请求。
        const std::atomic_bool* cancel{nullptr};
    };

    struct SynthesisResult {
        std::filesystem::path wav_path;
        float duration_seconds{0.0F};
        std::uint32_t sample_rate{0};
    };

    enum class BackendErrorKind {
        Canceled,
        Timeout,
        Failed,
        InvalidRequest,
    };

    struct BackendError {
        BackendErrorKind kind{BackendErrorKind::Failed};
        std::string message;
    };

    /// TTS 后端接口。后端只负责文字 → WAV，不碰声卡、不发布 ROS 消息。
    class TtsBackend {
    public:
        virtual ~TtsBackend() = default;

        /// 后端名，例如 piper、paroli。
        [[nodiscard]] virtual std::string_view name() const noexcept = 0;

        /// 影响合成结果的配置指纹，参与缓存键。
        [[nodiscard]] virtual std::string fingerprint() const = 0;

        /// 校验可执行文件/模型/服务地址；失败时节点保留但拒绝请求。
        [[nodiscard]] virtual std::expected<void, std::string> initialize() = 0;

        /// 同步合成一段文本到 request.output_wav。
        [[nodiscard]] virtual std::expected<SynthesisResult, BackendError>
        synthesize(const SynthesisRequest& request) = 0;
    };

    /// 请求是否已被调用方取消。
    [[nodiscard]] inline bool is_canceled(const SynthesisRequest& request) noexcept {
        return request.cancel != nullptr && request.cancel->load();
    }

} // namespace tts
