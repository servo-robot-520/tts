#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tts/sentence_splitter.hpp"

namespace {

TEST(SentenceSplitter, SplitsChinesePunctuation) {
    const auto sentences = tts::split_sentences("你好。我是小鸭！你呢？", 120);
    ASSERT_EQ(sentences.size(), 3U);
    EXPECT_EQ(sentences[0], "你好。");
    EXPECT_EQ(sentences[1], "我是小鸭！");
    EXPECT_EQ(sentences[2], "你呢？");
}

TEST(SentenceSplitter, SplitsAsciiPunctuation) {
    const auto sentences = tts::split_sentences("Hello world. How are you? Fine!", 120);
    ASSERT_EQ(sentences.size(), 3U);
    EXPECT_EQ(sentences[0], "Hello world.");
    EXPECT_EQ(sentences[1], "How are you?");
    EXPECT_EQ(sentences[2], "Fine!");
}

TEST(SentenceSplitter, KeepsDecimalNumbersTogether) {
    const auto sentences = tts::split_sentences("Pi is 3.14 and that is fine.", 120);
    ASSERT_EQ(sentences.size(), 1U);
    EXPECT_EQ(sentences[0], "Pi is 3.14 and that is fine.");
}

TEST(SentenceSplitter, SplitsAfterEllipsis) {
    const auto sentences = tts::split_sentences("Wait... what?", 120);
    ASSERT_EQ(sentences.size(), 2U);
    EXPECT_EQ(sentences[0], "Wait...");
    EXPECT_EQ(sentences[1], "what?");
}

TEST(SentenceSplitter, SplitsOnNewlinesAndTrimsWhitespace) {
    const auto sentences = tts::split_sentences("  first line \n\n second line  ", 120);
    ASSERT_EQ(sentences.size(), 2U);
    EXPECT_EQ(sentences[0], "first line");
    EXPECT_EQ(sentences[1], "second line");
}

TEST(SentenceSplitter, HardSplitsLongTextAtWhitespace) {
    const auto sentences = tts::split_sentences("aaaa bbbb cccc", 6);
    ASSERT_EQ(sentences.size(), 3U);
    EXPECT_EQ(sentences[0], "aaaa");
    EXPECT_EQ(sentences[1], "bbbb");
    EXPECT_EQ(sentences[2], "cccc");
}

TEST(SentenceSplitter, HardSplitsLongChineseText) {
    const auto sentences = tts::split_sentences("你好世界你好世界", 4);
    ASSERT_EQ(sentences.size(), 2U);
    EXPECT_EQ(sentences[0], "你好世界");
    EXPECT_EQ(sentences[1], "你好世界");
}

TEST(SentenceSplitter, ReturnsNothingForEmptyOrWhitespace) {
    EXPECT_TRUE(tts::split_sentences("", 120).empty());
    EXPECT_TRUE(tts::split_sentences("  \n\t ", 120).empty());
}

TEST(SentenceSplitter, ZeroLimitDisablesHardSplit) {
    const auto sentences = tts::split_sentences("one long sentence without punctuation", 0);
    ASSERT_EQ(sentences.size(), 1U);
}

} // namespace
