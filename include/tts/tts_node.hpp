#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

#include "audio_io_interface/ros_msg.hpp"
#include "tts/tts_backend.hpp"
#include "tts/tts_cache.hpp"
#include "tts_interface/ros_msg.hpp"

namespace tts {

    struct GoalUuidHash {
        std::size_t operator()(const rclcpp_action::GoalUUID& uuid) const noexcept {
            std::size_t hash = 1469598103934665603ULL;
            for (const auto byte : uuid) {
                hash ^= byte;
                hash *= 1099511628211ULL;
            }
            return hash;
        }
    };

    /// TTS 节点组件：合成结果以 AudioClip 交给 audio_io，自己不碰声卡。
    ///
    /// 采用组合而不是继承：内部只持有一个 rclcpp::Node，不继承它。其他节点可以
    /// 直接构造本类并传入自己的节点，共享同一个执行器：
    ///
    /// @code
    /// auto node = std::make_shared<rclcpp::Node>("system");
    /// auto tts = std::make_shared<tts::TtsNode>(node);
    /// @endcode
    ///
    /// 参数全部声明在传入的节点上；构造失败（参数非法、缓存目录不可写）会抛出
    /// std::runtime_error。后端不可用（模型缺失、paroli-server 未启动）不会中断
    /// 构造，但此后的合成请求会返回错误。
    class TtsNode {
    public:
        using MsgTtsText = tts_interface::msg::TtsText;
        using MsgTtsEvent = tts_interface::msg::TtsEvent;
        using SrvSynthesize = tts_interface::srv::Synthesize;
        using ActionSay = tts_interface::action::Say;
        using SayGoalHandle = rclcpp_action::ServerGoalHandle<ActionSay>;

        explicit TtsNode(rclcpp::Node::SharedPtr node);
        ~TtsNode();

        TtsNode(const TtsNode&) = delete;
        TtsNode& operator=(const TtsNode&) = delete;
        TtsNode(TtsNode&&) = delete;
        TtsNode& operator=(TtsNode&&) = delete;

        [[nodiscard]] const rclcpp::Node::SharedPtr& node() const noexcept {
            return node_;
        }

        /// 后端名（piper / paroli），日志与状态展示用。
        [[nodiscard]] const std::string& backend_name() const noexcept {
            return backend_name_;
        }

    private:
        struct SynthesisOutcome {
            std::filesystem::path wav;
            float duration_seconds{0.0F};
            bool cache_hit{false};
        };

        struct SynthesisFailure {
            bool canceled{false};
            std::string message;
        };

        struct SubmittedClip {
            std::string uri;
            float duration_seconds{0.0F};
        };

        void load_parameters();
        [[nodiscard]] std::unique_ptr<TtsBackend> create_backend();
        void create_interfaces();

        void on_text(const MsgTtsText::ConstSharedPtr& message);
        void on_synthesize(const std::shared_ptr<SrvSynthesize::Request>& request,
                           std::shared_ptr<SrvSynthesize::Response> response);
        rclcpp_action::GoalResponse on_say_goal(const rclcpp_action::GoalUUID& uuid,
                                                std::shared_ptr<const ActionSay::Goal> goal);
        rclcpp_action::CancelResponse on_say_cancel(const std::shared_ptr<SayGoalHandle>& goal_handle);
        void on_say_accepted(const std::shared_ptr<SayGoalHandle>& goal_handle);
        void run_say_worker(std::stop_token stop_token);
        void execute_say(const std::shared_ptr<SayGoalHandle>& goal_handle);
        void on_audio_io_event(const audio_io_interface::msg::AudioEvent::ConstSharedPtr& message);

        [[nodiscard]] std::expected<SynthesisOutcome, SynthesisFailure>
        synthesize_cached(const MsgTtsText& request, const std::string& request_id,
                          const std::atomic_bool* cancel);
        void submit_clip(const SynthesisOutcome& outcome, const MsgTtsText& request,
                         const std::string& request_id, bool queue);
        void publish_event(std::uint8_t event, const std::string& request_id,
                           const std::string& audio_uri, float duration,
                           const std::string& error);

        [[nodiscard]] std::string make_request_id();
        [[nodiscard]] std::string make_cache_key(const MsgTtsText& request) const;
        [[nodiscard]] float effective_volume(const MsgTtsText& request) const;
        [[nodiscard]] std::string effective_group(const MsgTtsText& request) const;
        [[nodiscard]] static std::string file_uri(const std::filesystem::path& path);
        void remember_clip(const std::string& request_id, const std::string& uri, float duration);
        [[nodiscard]] std::shared_ptr<std::atomic_bool>
        cancel_flag_for(const rclcpp_action::GoalUUID& uuid, bool create);

        rclcpp::Node::SharedPtr node_;

        // 参数
        std::string backend_name_;
        std::string piper_executable_;
        std::string piper_model_;
        std::string piper_config_;
        std::string piper_espeak_data_;
        int piper_speaker_id_{-1};
        std::string paroli_url_;
        int paroli_sample_rate_{22050};
        int paroli_speaker_id_{-1};
        int backend_timeout_ms_{30000};
        std::string audio_io_node_;
        std::string frame_id_;
        std::string default_group_;
        double default_volume_{1.0};
        int max_sentence_chars_{120};
        std::string cache_dir_;
        std::int64_t cache_max_bytes_{0};

        std::unique_ptr<TtsBackend> backend_;
        std::string backend_error_;
        std::unique_ptr<TtsCache> cache_;

        rclcpp::CallbackGroup::SharedPtr text_group_;
        rclcpp::CallbackGroup::SharedPtr io_group_;
        rclcpp::CallbackGroup::SharedPtr service_group_;
        rclcpp::CallbackGroup::SharedPtr action_group_;
        rclcpp::Subscription<MsgTtsText>::SharedPtr text_subscription_;
        rclcpp::Subscription<audio_io_interface::msg::AudioEvent>::SharedPtr io_event_subscription_;
        rclcpp::Publisher<MsgTtsEvent>::SharedPtr event_publisher_;
        rclcpp::Publisher<audio_io_interface::msg::AudioClip>::SharedPtr clip_publisher_;
        rclcpp::Service<SrvSynthesize>::SharedPtr synthesize_service_;
        rclcpp_action::Server<ActionSay>::SharedPtr say_server_;

        std::mutex backend_mutex_;
        std::atomic<std::uint64_t> request_counter_{0};

        mutable std::mutex clips_mutex_;
        std::unordered_map<std::string, SubmittedClip> clips_;
        std::deque<std::string> clip_order_;

        std::mutex cancel_mutex_;
        std::unordered_map<rclcpp_action::GoalUUID, std::shared_ptr<std::atomic_bool>, GoalUuidHash>
                cancel_flags_;

        std::mutex say_mutex_;
        std::condition_variable say_cv_;
        std::deque<std::shared_ptr<SayGoalHandle>> say_queue_;
        bool say_stop_{false};
        std::jthread say_worker_;
    };

} // namespace tts
