#include "tts/tts_node.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

#include <rcl_action/action_server.h>

#include "tts/paroli_backend.hpp"
#include "tts/paths.hpp"
#include "tts/piper_backend.hpp"
#include "tts/sentence_splitter.hpp"

namespace tts {

    namespace {

        constexpr std::size_t kMaxRememberedClips = 128;

        /// goal 状态可能被并发取消/关闭改变；非法 transition 不应终止节点。
        template <typename Callable>
        void safe_goal_transition(const rclcpp::Logger& logger, Callable&& callable) {
            try {
                callable();
            }
            catch (const rclcpp::exceptions::RCLError& error) {
                RCLCPP_WARN(logger, "goal transition failed: %s", error.what());
            }
        }
        constexpr std::int64_t kDefaultCacheBytes = 256LL * 1024LL * 1024LL;

        float normalize_rate(const float rate) {
            return rate > 0.0F ? std::clamp(rate, 0.25F, 4.0F) : 1.0F;
        }

        float normalize_pitch(const float pitch) {
            return pitch > 0.0F ? std::clamp(pitch, 0.5F, 2.0F) : 1.0F;
        }

    } // namespace

    TtsNode::TtsNode(rclcpp::Node::SharedPtr node) :
        node_(std::move(node)) {
        if (!node_) {
            throw std::invalid_argument("TtsNode requires a non-null rclcpp::Node");
        }

        load_parameters();
        cache_ = std::make_unique<TtsCache>(cache_dir_, static_cast<std::uint64_t>(cache_max_bytes_));
        backend_ = create_backend();
        scheduler_ = std::make_unique<SynthesisScheduler>(
            static_cast<std::size_t>(std::max(1, scheduler_max_pending_)));
        create_interfaces();

        say_worker_ = std::jthread([this](const std::stop_token stop_token) {
            run_say_worker(stop_token);
        });

        if (backend_error_.empty()) {
            RCLCPP_INFO(node_->get_logger(),
                        "TTS node ready: backend=%s, audio_io=%s, cache=%s",
                        backend_name_.c_str(), audio_io_node_.c_str(), cache_dir_.c_str());
        }
        else {
            RCLCPP_ERROR(node_->get_logger(),
                         "TTS node started with an unavailable %s backend: %s",
                         backend_name_.c_str(), backend_error_.c_str());
        }
    }

    TtsNode::~TtsNode() {
        // 让正在合成的 say 目标尽快退出，再停止工作线程。
        {
            const std::lock_guard lock(cancel_mutex_);
            for (auto& [uuid, flag] : cancel_flags_) {
                flag->store(true);
            }
        }
        if (scheduler_) {
            scheduler_->shutdown();
        }
        {
            const std::lock_guard lock(say_mutex_);
            say_stop_ = true;
        }
        say_cv_.notify_all();
        if (say_worker_.joinable()) {
            say_worker_.join();
        }
    }

    void TtsNode::load_parameters() {
        const auto declare_string = [this](const std::string& name, const std::string& fallback) {
            return node_->declare_parameter<std::string>(name, fallback);
        };

        backend_name_ = declare_string("backend", "piper");
        piper_executable_ = expand_user_path(declare_string("piper.executable", "piper")).string();
        piper_model_ = expand_user_path(declare_string("piper.model", "")).string();
        piper_config_ = expand_user_path(declare_string("piper.config", "")).string();
        piper_espeak_data_ = expand_user_path(declare_string("piper.espeak_data", "")).string();
        piper_speaker_id_ = node_->declare_parameter<int>("piper.speaker", -1);

        paroli_url_ = declare_string("paroli.url", "http://127.0.0.1:8848");
        paroli_sample_rate_ = node_->declare_parameter<int>("paroli.sample_rate", 22050);
        paroli_speaker_id_ = node_->declare_parameter<int>("paroli.speaker_id", -1);

        backend_timeout_ms_ = node_->declare_parameter<int>("backend.timeout_ms", 30000);
        if (backend_timeout_ms_ <= 0) {
            throw std::invalid_argument("backend.timeout_ms must be greater than zero");
        }

        audio_io_node_ = audio_io_interface::qualify_node_fqn(audio_io_interface::kDefaultNodeFqn, node_->get_namespace());
        frame_id_ = declare_string("frame_id", "audio");
        default_group_ = declare_string("default.group", "tts");
        default_volume_ = node_->declare_parameter<double>("default.volume", 1.0);
        if (default_volume_ <= 0.0) {
            default_volume_ = 1.0;
        }

        max_sentence_chars_ = std::max<int>(16, node_->declare_parameter<int>("max_sentence_chars", 120));
        scheduler_max_pending_ = std::max<int>(
            1, node_->declare_parameter<int>("scheduler.max_pending", 16));
        scheduler_preempt_speech_ =
            node_->declare_parameter<bool>("scheduler.preempt_speech", true);
        const auto raw_cache_dir = declare_string("cache.dir", "~/.cache/robot_audio/tts");
        cache_max_bytes_ = std::max<std::int64_t>(
            0, node_->declare_parameter<std::int64_t>("cache.max_bytes", kDefaultCacheBytes));

        std::error_code error;
        cache_dir_ = std::filesystem::absolute(expand_user_path(raw_cache_dir), error).string();
        if (error) {
            throw std::runtime_error("cannot resolve cache.dir: " + error.message());
        }
    }

    std::unique_ptr<TtsBackend> TtsNode::create_backend() {
        std::unique_ptr<TtsBackend> backend;
        if (backend_name_ == "piper") {
            PiperOptions options;
            options.executable = piper_executable_;
            options.model = piper_model_;
            options.config = piper_config_;
            options.espeak_data = piper_espeak_data_;
            options.speaker_id = piper_speaker_id_;
            options.timeout = std::chrono::milliseconds{backend_timeout_ms_};
            backend = std::make_unique<PiperBackend>(std::move(options));
        }
        else if (backend_name_ == "paroli") {
            ParoliOptions options;
            options.url = paroli_url_;
            options.sample_rate = static_cast<std::uint32_t>(std::max(0, paroli_sample_rate_));
            options.speaker_id = paroli_speaker_id_;
            options.timeout = std::chrono::milliseconds{backend_timeout_ms_};
            backend = std::make_unique<ParoliBackend>(std::move(options));
        }
        else {
            throw std::invalid_argument("unknown tts backend: " + backend_name_ +
                                        " (expected piper or paroli)");
        }

        if (auto ready = backend->initialize(); !ready) {
            backend_error_ = ready.error();
        }
        return backend;
    }

    void TtsNode::create_interfaces() {
        // 每个接口一个 mutually exclusive group：长合成只阻塞同组回调，不挡住其他接口。
        // 使用默认的 automatically_add_to_executor_with_node=true，执行器 add_node 时注册这些 group。
        text_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
        io_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
        service_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
        action_group_ = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

        const auto clip_topic = audio_io_interface::topic_play(audio_io_interface::kDefaultNodeFqn, node_->get_namespace());
        const auto io_event_topic = audio_io_interface::topic_event(audio_io_interface::kDefaultNodeFqn, node_->get_namespace());

        rclcpp::SubscriptionOptions text_options;
        text_options.callback_group = text_group_;
        text_subscription_ = node_->create_subscription<MsgTtsText>(
            std::string{tts_interface::kPrivateTopicText},
            rclcpp::QoS(rclcpp::KeepLast(10)).reliable(),
            [this](const MsgTtsText::ConstSharedPtr& message) { on_text(message); },
            text_options);

        rclcpp::SubscriptionOptions io_options;
        io_options.callback_group = io_group_;
        io_event_subscription_ = node_->create_subscription<audio_io_interface::msg::AudioEvent>(
            io_event_topic,
            rclcpp::QoS(rclcpp::KeepLast(50)).reliable(),
            [this](const audio_io_interface::msg::AudioEvent::ConstSharedPtr& message) {
                on_audio_io_event(message);
            },
            io_options);

        event_publisher_ = node_->create_publisher<MsgTtsEvent>(
            std::string{tts_interface::kPrivateTopicEvent},
            rclcpp::QoS(rclcpp::KeepLast(50)).reliable());

        clip_publisher_ = node_->create_publisher<audio_io_interface::msg::AudioClip>(
            clip_topic, rclcpp::QoS(rclcpp::KeepLast(10)).reliable());

        synthesize_service_ = node_->create_service<SrvSynthesize>(
            std::string{tts_interface::kPrivateServiceSynthesize},
            [this](const std::shared_ptr<SrvSynthesize::Request>& request,
                   std::shared_ptr<SrvSynthesize::Response> response) {
                on_synthesize(request, std::move(response));
            },
            rmw_qos_profile_services_default,
            service_group_);

        say_server_ = rclcpp_action::create_server<ActionSay>(
            node_,
            std::string{tts_interface::kPrivateActionSay},
            [this](const rclcpp_action::GoalUUID& uuid,
                   std::shared_ptr<const ActionSay::Goal> goal) {
                return on_say_goal(uuid, std::move(goal));
            },
            [this](const std::shared_ptr<SayGoalHandle>& goal_handle) {
                return on_say_cancel(goal_handle);
            },
            [this](const std::shared_ptr<SayGoalHandle>& goal_handle) {
                on_say_accepted(goal_handle);
            },
            rcl_action_server_get_default_options(),
            action_group_);
    }

    void TtsNode::on_text(const MsgTtsText::ConstSharedPtr& message) {
        const MsgTtsText request = *message;
        const std::string request_id = make_request_id();

        if (const auto cached = lookup_cache(request, request_id)) {
            if (request.play) {
                if (scheduler_preempt_speech_) {
                    scheduler_->preempt_lower_priority(JobPriority::High);
                }
                submit_clip(*cached, request, request_id, false);
            }
            return;
        }

        SynthesisScheduler::Job job;
        job.priority = JobPriority::High;
        job.coalesce_key = "text:" + effective_group(request);
        job.preempt_active = scheduler_preempt_speech_ && request.play;
        job.run = [this, request, request_id](const std::atomic_bool& cancel) {
            const auto outcome = synthesize_cached(request, request_id, &cancel);
            if (!outcome || !request.play) {
                return;
            }
            submit_clip(*outcome, request, request_id, false);
        };
        job.on_dropped = [this, request_id] {
            publish_event(MsgTtsEvent::CANCELED, request_id, {}, 0.0F,
                          "superseded by a newer text request");
        };

        if (!scheduler_->submit(std::move(job))) {
            constexpr std::string_view error = "synthesis scheduler is full or stopped";
            publish_event(MsgTtsEvent::FAILED, request_id, {}, 0.0F, std::string{error});
            RCLCPP_WARN(node_->get_logger(), "TTS text request %s rejected: %s",
                        request_id.c_str(), error.data());
        }
    }

    void TtsNode::on_synthesize(const std::shared_ptr<SrvSynthesize::Request>& request,
                                std::shared_ptr<SrvSynthesize::Response> response) {
        const std::string request_id = make_request_id();
        const auto outcome = synthesize_blocking(request->request, request_id,
                                                 JobPriority::High, false, nullptr);
        if (!outcome) {
            response->success = false;
            response->audio_uri.clear();
            response->duration = 0.0F;
            response->error = outcome.error().message;
            return;
        }
        response->success = true;
        response->audio_uri = file_uri(outcome->wav);
        response->duration = outcome->duration_seconds;
        response->error.clear();
    }

    rclcpp_action::GoalResponse TtsNode::on_say_goal(const rclcpp_action::GoalUUID& uuid,
                                                     std::shared_ptr<const ActionSay::Goal> goal) {
        if (goal->text.text.empty()) {
            RCLCPP_WARN(node_->get_logger(), "Rejecting say goal: text is empty");
            return rclcpp_action::GoalResponse::REJECT;
        }
        if (!backend_error_.empty()) {
            RCLCPP_ERROR(node_->get_logger(), "Rejecting say goal: %s", backend_error_.c_str());
            return rclcpp_action::GoalResponse::REJECT;
        }

        const std::lock_guard lock(cancel_mutex_);
        cancel_flags_.try_emplace(uuid, std::make_shared<std::atomic_bool>(false));
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    }

    rclcpp_action::CancelResponse
    TtsNode::on_say_cancel(const std::shared_ptr<SayGoalHandle>& goal_handle) {
        if (const auto flag = cancel_flag_for(goal_handle->get_goal_id(), true)) {
            flag->store(true);
        }
        return rclcpp_action::CancelResponse::ACCEPT;
    }

    void TtsNode::on_say_accepted(const std::shared_ptr<SayGoalHandle>& goal_handle) {
        {
            const std::lock_guard lock(say_mutex_);
            say_queue_.push_back(goal_handle);
        }
        say_cv_.notify_one();
    }

    void TtsNode::run_say_worker(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::shared_ptr<SayGoalHandle> goal_handle;
            {
                std::unique_lock lock(say_mutex_);
                say_cv_.wait(lock, [this, &stop_token] {
                    return say_stop_ || stop_token.stop_requested() || !say_queue_.empty();
                });
                if (say_queue_.empty()) {
                    return;
                }
                goal_handle = std::move(say_queue_.front());
                say_queue_.pop_front();
            }
            execute_say(goal_handle);
        }
    }

    void TtsNode::execute_say(const std::shared_ptr<SayGoalHandle>& goal_handle) {
        const auto goal = goal_handle->get_goal();
        auto result = std::make_shared<ActionSay::Result>();
        result->request_id = make_request_id();

        const auto uuid = goal_handle->get_goal_id();
        const auto cancel_flag = cancel_flag_for(uuid, true);
        const auto release = [this, &uuid] { static_cast<void>(cancel_flag_for(uuid, false)); };

        const auto sentences = split_sentences(goal->text.text,
                                               static_cast<std::size_t>(max_sentence_chars_));
        if (sentences.empty()) {
            publish_event(MsgTtsEvent::FAILED, result->request_id, {}, 0.0F, "text is empty");
            goal_handle->abort(result);
            release();
            return;
        }

        const auto finish = [&](const bool success) {
            if (!goal_handle->is_canceling() && success) {
                safe_goal_transition(node_->get_logger(), [&] {
                    static_cast<void>(goal_handle->succeed(result));
                });
                return;
            }
            if (goal_handle->is_canceling()) {
                safe_goal_transition(node_->get_logger(), [&] {
                    static_cast<void>(goal_handle->canceled(result));
                });
                return;
            }
            // 内部抢占（例如状态文本打断播报）没有经过 rcl_action 的 cancel 请求，
            // 只能走 ABORTED；对客户端仍表现为“未完成”。
            safe_goal_transition(node_->get_logger(), [&] {
                static_cast<void>(goal_handle->abort(result));
            });
        };

        for (std::size_t index = 0; index < sentences.size(); ++index) {
            if (!goal_handle->is_active()) {
                release();
                return;
            }
            if (goal_handle->is_canceling() || cancel_flag->load()) {
                finish(false);
                release();
                return;
            }

            MsgTtsText piece = goal->text;
            piece.text = sentences[index];
            const std::string sentence_id =
                result->request_id + "-" + std::to_string(index);

            auto outcome = synthesize_blocking(piece, sentence_id, JobPriority::Normal, false,
                                               cancel_flag);
            if (!outcome) {
                finish(false);
                release();
                return;
            }

            if (piece.play) {
                submit_clip(*outcome, piece, sentence_id, true);
            }

            auto feedback = std::make_shared<ActionSay::Feedback>();
            feedback->progress = static_cast<float>(index + 1) /
                static_cast<float>(sentences.size());
            feedback->current_sentence = sentences[index];
            safe_goal_transition(node_->get_logger(), [&] {
                goal_handle->publish_feedback(feedback);
            });
        }

        finish(true);
        release();
    }

    std::optional<TtsNode::SynthesisOutcome>
    TtsNode::lookup_cache(const MsgTtsText& request, const std::string& request_id) {
        if (request.text.empty() || !backend_error_.empty()) {
            return std::nullopt;
        }
        const std::string key = make_cache_key(request);
        const auto cached = cache_->lookup(key);
        if (!cached) {
            return std::nullopt;
        }
        const auto wav = cache_->wav_path(key);
        publish_event(MsgTtsEvent::CACHE_HIT, request_id, file_uri(wav), cached->duration_seconds,
                      {});
        return SynthesisOutcome{wav, cached->duration_seconds, true};
    }

    TtsNode::SynthesisAttempt TtsNode::synthesize_cached(const MsgTtsText& request,
                                                         const std::string& request_id,
                                                         const std::atomic_bool* cancel) {
        if (request.text.empty()) {
            publish_event(MsgTtsEvent::FAILED, request_id, {}, 0.0F, "text is empty");
            return std::unexpected(SynthesisFailure{false, "text is empty"});
        }
        if (!backend_error_.empty()) {
            publish_event(MsgTtsEvent::FAILED, request_id, {}, 0.0F, backend_error_);
            return std::unexpected(SynthesisFailure{false, backend_error_});
        }
        if (cancel != nullptr && cancel->load()) {
            publish_event(MsgTtsEvent::CANCELED, request_id, {}, 0.0F, "request canceled");
            return std::unexpected(SynthesisFailure{true, "request canceled"});
        }

        const float pitch = normalize_pitch(request.pitch);
        if (pitch != 1.0F) {
            RCLCPP_WARN_THROTTLE(node_->get_logger(), *node_->get_clock(), 5000,
                                 "TTS pitch is not supported by the %s backend; ignoring %.3f",
                                 backend_name_.c_str(), static_cast<double>(pitch));
        }

        const std::string key = make_cache_key(request);
        if (const auto cached = cache_->lookup(key)) {
            const auto wav = cache_->wav_path(key);
            publish_event(MsgTtsEvent::CACHE_HIT, request_id, file_uri(wav),
                          cached->duration_seconds, {});
            return SynthesisOutcome{wav, cached->duration_seconds, true};
        }

        cache_->evict();
        publish_event(MsgTtsEvent::SYNTHESIZE_STARTED, request_id, {}, 0.0F, {});
        if (cancel != nullptr && cancel->load()) {
            publish_event(MsgTtsEvent::CANCELED, request_id, {}, 0.0F, "request canceled");
            return std::unexpected(SynthesisFailure{true, "request canceled"});
        }

        SynthesisRequest backend_request;
        backend_request.text = request.text;
        backend_request.voice_id = request.voice_id;
        backend_request.language = request.language;
        backend_request.rate = normalize_rate(request.rate);
        backend_request.pitch = pitch;
        backend_request.output_wav = cache_->temp_path(key);
        backend_request.cancel = cancel;

        auto synthesized = backend_->synthesize(backend_request);
        if (!synthesized) {
            std::error_code error;
            std::filesystem::remove(backend_request.output_wav, error);
            const bool canceled = synthesized.error().kind == BackendErrorKind::Canceled;
            publish_event(canceled ? MsgTtsEvent::CANCELED : MsgTtsEvent::FAILED, request_id, {},
                          0.0F, synthesized.error().message);
            if (canceled) {
                RCLCPP_DEBUG(node_->get_logger(), "TTS request %s canceled: %s",
                             request_id.c_str(), synthesized.error().message.c_str());
            }
            else {
                RCLCPP_ERROR(node_->get_logger(), "TTS request %s failed: %s",
                             request_id.c_str(), synthesized.error().message.c_str());
            }
            return std::unexpected(
                SynthesisFailure{canceled, synthesized.error().message});
        }

        if (auto committed = cache_->commit(synthesized->wav_path, key); !committed) {
            publish_event(MsgTtsEvent::FAILED, request_id, {}, 0.0F, committed.error());
            RCLCPP_ERROR(node_->get_logger(), "TTS cache commit failed: %s",
                         committed.error().c_str());
            return std::unexpected(SynthesisFailure{false, committed.error()});
        }

        const auto wav = cache_->wav_path(key);
        publish_event(MsgTtsEvent::SYNTHESIZED, request_id, file_uri(wav),
                      synthesized->duration_seconds, {});
        RCLCPP_DEBUG(node_->get_logger(), "Synthesized request %s in %.3f s of audio",
                     request_id.c_str(), static_cast<double>(synthesized->duration_seconds));
        return SynthesisOutcome{wav, synthesized->duration_seconds, false};
    }

    TtsNode::SynthesisAttempt
    TtsNode::synthesize_blocking(const MsgTtsText& request, const std::string& request_id,
                                 const JobPriority priority, const bool preempt_active,
                                 std::shared_ptr<std::atomic_bool> cancel) {
        if (!scheduler_) {
            return std::unexpected(
                SynthesisFailure{false, "synthesis scheduler is not running"});
        }

        if (cancel != nullptr && cancel->load()) {
            publish_event(MsgTtsEvent::CANCELED, request_id, {}, 0.0F, "request canceled");
            return std::unexpected(SynthesisFailure{true, "request canceled"});
        }
        if (const auto cached = lookup_cache(request, request_id)) {
            return *cached;
        }

        auto promise = std::make_shared<std::promise<SynthesisAttempt>>();
        auto future = promise->get_future();

        SynthesisScheduler::Job job;
        job.priority = priority;
        job.preempt_active = preempt_active;
        job.cancel = std::move(cancel);
        job.run = [this, request, request_id, promise](const std::atomic_bool& cancel_flag) {
            SynthesisAttempt result = synthesize_cached(request, request_id, &cancel_flag);
            try {
                promise->set_value(std::move(result));
            }
            catch (const std::future_error&) {
                // on_dropped 已给出结果；正常调度下不会发生。
            }
        };
        job.on_dropped = [this, request_id, promise] {
            publish_event(MsgTtsEvent::CANCELED, request_id, {}, 0.0F,
                          "synthesis job dropped");
            try {
                promise->set_value(std::unexpected(
                    SynthesisFailure{true, "synthesis job dropped"}));
            }
            catch (const std::future_error&) {
            }
        };

        if (!scheduler_->submit(std::move(job))) {
            constexpr std::string_view error = "synthesis scheduler is full or stopped";
            publish_event(MsgTtsEvent::FAILED, request_id, {}, 0.0F, std::string{error});
            return std::unexpected(SynthesisFailure{false, std::string{error}});
        }
        return future.get();
    }

    void TtsNode::submit_clip(const SynthesisOutcome& outcome, const MsgTtsText& request,
                              const std::string& request_id, const bool queue) {
        audio_io_interface::msg::AudioClip clip;
        clip.header.stamp = node_->now();
        clip.header.frame_id = frame_id_;
        clip.request_id = request_id;
        clip.uri = file_uri(outcome.wav);
        clip.format = "wav";
        clip.gain = effective_volume(request);
        clip.loop = false;
        clip.priority = audio_io_interface::msg::AudioClip::PRIORITY_SPEECH;
        clip.policy = queue ? audio_io_interface::msg::AudioClip::POLICY_QUEUE
                            : audio_io_interface::msg::AudioClip::POLICY_REPLACE_SAME_GROUP;
        clip.group = effective_group(request);
        clip.start_offset = 0.0;
        clip.duration = 0.0;

        // 先登记再发布，避免 audio_io 的 STARTED 事件早于登记被丢弃。
        remember_clip(request_id, clip.uri, outcome.duration_seconds);
        clip_publisher_->publish(clip);
    }

    void TtsNode::on_audio_io_event(
        const audio_io_interface::msg::AudioEvent::ConstSharedPtr& message) {
        using AudioEvent = audio_io_interface::msg::AudioEvent;
        if (message->request_id.empty()) {
            return;
        }

        SubmittedClip record;
        {
            const std::lock_guard lock(clips_mutex_);
            const auto found = clips_.find(message->request_id);
            if (found == clips_.end()) {
                return;
            }
            record = found->second;
            if (message->event != AudioEvent::EVENT_STARTED) {
                clips_.erase(found);
                std::erase(clip_order_, message->request_id);
            }
        }

        switch (message->event) {
        case AudioEvent::EVENT_STARTED:
            publish_event(MsgTtsEvent::PLAY_STARTED, message->request_id, record.uri,
                          record.duration_seconds, {});
            break;
        case AudioEvent::EVENT_FINISHED:
            publish_event(MsgTtsEvent::PLAY_FINISHED, message->request_id, record.uri,
                          record.duration_seconds, {});
            break;
        case AudioEvent::EVENT_PREEMPTED:
            publish_event(MsgTtsEvent::CANCELED, message->request_id, record.uri,
                          record.duration_seconds,
                          message->detail.empty() ? "preempted by audio_io" : message->detail);
            break;
        case AudioEvent::EVENT_FAILED:
        case AudioEvent::EVENT_DECODE_FAILED:
            publish_event(MsgTtsEvent::FAILED, message->request_id, record.uri, 0.0F,
                          message->detail.empty() ? "audio_io playback failed" : message->detail);
            break;
        default:
            break;
        }
    }

    void TtsNode::publish_event(const std::uint8_t event, const std::string& request_id,
                                const std::string& audio_uri, const float duration,
                                const std::string& error) {
        if (!event_publisher_) {
            return;
        }
        MsgTtsEvent message;
        message.header.stamp = node_->now();
        message.header.frame_id = frame_id_;
        message.request_id = request_id;
        message.event = event;
        message.audio_uri = audio_uri;
        message.duration = duration;
        message.error = error;
        event_publisher_->publish(message);
    }

    std::string TtsNode::make_request_id() {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        return std::format("tts-{}-{}", stamp, request_counter_.fetch_add(1));
    }

    std::string TtsNode::make_cache_key(const MsgTtsText& request) const {
        const std::string rate = std::format("{:.4f}", normalize_rate(request.rate));
        const std::string pitch = std::format("{:.4f}", normalize_pitch(request.pitch));
        return TtsCache::make_key({backend_->fingerprint(), request.text, request.voice_id,
                                   request.language, rate, pitch});
    }

    float TtsNode::effective_volume(const MsgTtsText& request) const {
        const auto requested = static_cast<double>(request.volume);
        const double volume = requested > 0.0 ? requested : default_volume_;
        return static_cast<float>(std::clamp(volume, 0.0, 4.0));
    }

    std::string TtsNode::effective_group(const MsgTtsText& request) const {
        if (!request.group.empty()) {
            return request.group;
        }
        return default_group_.empty() ? std::string{"tts"} : default_group_;
    }

    std::string TtsNode::file_uri(const std::filesystem::path& path) {
        return "file://" + path.string();
    }

    void TtsNode::remember_clip(const std::string& request_id, const std::string& uri,
                                const float duration) {
        const std::lock_guard lock(clips_mutex_);
        if (clips_.contains(request_id)) {
            clips_[request_id] = SubmittedClip{uri, duration};
            return;
        }
        while (clip_order_.size() >= kMaxRememberedClips) {
            clips_.erase(clip_order_.front());
            clip_order_.pop_front();
        }
        clips_.emplace(request_id, SubmittedClip{uri, duration});
        clip_order_.push_back(request_id);
    }

    std::shared_ptr<std::atomic_bool>
    TtsNode::cancel_flag_for(const rclcpp_action::GoalUUID& uuid, const bool create) {
        const std::lock_guard lock(cancel_mutex_);
        if (create) {
            return cancel_flags_.try_emplace(uuid, std::make_shared<std::atomic_bool>(false))
                .first->second;
        }
        const auto found = cancel_flags_.find(uuid);
        if (found == cancel_flags_.end()) {
            return nullptr;
        }
        auto flag = found->second;
        cancel_flags_.erase(found);
        return flag;
    }

} // namespace tts
