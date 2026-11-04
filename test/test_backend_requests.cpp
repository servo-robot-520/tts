#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tts/paroli_backend.hpp"
#include "tts/piper_backend.hpp"
#include "tts/tts_backend.hpp"

namespace {

bool has_argument_pair(const std::vector<std::string>& arguments, const std::string& flag,
                       const std::string& value) {
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == flag && arguments[i + 1] == value) {
            return true;
        }
    }
    return false;
}

TEST(PiperArgumentsTest, IncludesModelOutputAndSpeaker) {
    tts::PiperOptions options;
    options.model = "/models/voice.onnx";
    options.espeak_data = "/data/espeak-ng-data";

    tts::SynthesisRequest request;
    request.output_wav = "/cache/out.wav";
    request.rate = 2.0F;
    request.voice_id = "3";

    const auto arguments = tts::piper_arguments(options, request);
    EXPECT_TRUE(has_argument_pair(arguments, "--model", "/models/voice.onnx"));
    EXPECT_TRUE(has_argument_pair(arguments, "--output_file", "/cache/out.wav"));
    EXPECT_TRUE(has_argument_pair(arguments, "--speaker", "3"));
    EXPECT_TRUE(has_argument_pair(arguments, "--length_scale", "0.5000"));
    EXPECT_TRUE(has_argument_pair(arguments, "--espeak_data", "/data/espeak-ng-data"));
}

TEST(PiperArgumentsTest, OmitsOptionalArgumentsForDefaults) {
    tts::PiperOptions options;
    options.model = "/models/voice.onnx";
    options.speaker_id = -1;

    tts::SynthesisRequest request;
    request.output_wav = "/cache/out.wav";
    request.rate = 1.0F;

    const auto arguments = tts::piper_arguments(options, request);
    EXPECT_FALSE(has_argument_pair(arguments, "--speaker", ""));
    EXPECT_FALSE(has_argument_pair(arguments, "--length_scale", "1.0000"));
    for (const auto& argument : arguments) {
        EXPECT_NE(argument, "--speaker");
        EXPECT_NE(argument, "--length_scale");
        EXPECT_NE(argument, "--config");
        EXPECT_NE(argument, "--espeak_data");
    }
}

TEST(PiperArgumentsTest, RequestVoiceOverridesDefaultSpeaker) {
    tts::PiperOptions options;
    options.model = "/models/voice.onnx";
    options.speaker_id = 1;

    tts::SynthesisRequest request;
    request.output_wav = "/cache/out.wav";
    request.voice_id = "7";

    const auto arguments = tts::piper_arguments(options, request);
    EXPECT_TRUE(has_argument_pair(arguments, "--speaker", "7"));
    EXPECT_FALSE(has_argument_pair(arguments, "--speaker", "1"));
}

TEST(PiperArgumentsTest, RejectsNonNumericVoice) {
    tts::PiperBackend backend{tts::PiperOptions{}};
    tts::SynthesisRequest request;
    request.text = "hello";
    request.voice_id = "alice";
    request.output_wav = "/tmp/unused.wav";

    const auto result = backend.synthesize(request);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, tts::BackendErrorKind::InvalidRequest);
}

TEST(ParoliRequestsTest, BuildsJsonBodyWithEscaping) {
    tts::ParoliOptions options;
    options.speaker_id = 4;

    tts::SynthesisRequest request;
    request.text = "a\"b\n";
    request.rate = 2.0F;

    const auto json = tts::paroli_request_json(options, request);
    EXPECT_EQ(json,
              "{\"text\":\"a\\\"b\\n\",\"audio_format\":\"pcm\",\"speaker_id\":4,"
              "\"length_scale\":0.5000}");
}

TEST(ParoliRequestsTest, OmitsOptionalFieldsForDefaults) {
    tts::ParoliOptions options;
    options.speaker_id = -1;

    tts::SynthesisRequest request;
    request.text = "hello";
    request.rate = 1.0F;

    const auto json = tts::paroli_request_json(options, request);
    EXPECT_EQ(json, "{\"text\":\"hello\",\"audio_format\":\"pcm\"}");
}

TEST(ParoliRequestsTest, JoinsSynthesizeUrl) {
    tts::ParoliOptions options;
    options.url = "http://127.0.0.1:8848/";
    EXPECT_EQ(tts::paroli_synthesize_url(options),
              "http://127.0.0.1:8848/api/v1/synthesise");
}

TEST(ParoliRequestsTest, ValidatesUrlAndSampleRate) {
    tts::ParoliBackend invalid_url{tts::ParoliOptions{.url = "ftp://example"}};
    EXPECT_FALSE(invalid_url.initialize().has_value());

    tts::ParoliOptions zero_rate;
    zero_rate.sample_rate = 0;
    tts::ParoliBackend invalid_rate{zero_rate};
    EXPECT_FALSE(invalid_rate.initialize().has_value());

    tts::ParoliBackend valid{tts::ParoliOptions{}};
    EXPECT_TRUE(valid.initialize().has_value());
}

TEST(ParoliRequestsTest, RejectsNonNumericVoice) {
    tts::ParoliBackend backend{tts::ParoliOptions{}};
    tts::SynthesisRequest request;
    request.text = "hello";
    request.voice_id = "alice";
    request.output_wav = "/tmp/unused.wav";

    const auto result = backend.synthesize(request);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, tts::BackendErrorKind::InvalidRequest);
}

} // namespace
