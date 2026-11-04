#include "tts/wav_io.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace tts {

    namespace {

        constexpr std::uint16_t kWaveFormatPcm = 1;
        constexpr std::size_t kWavHeaderSize = 44;

        void append_u16(std::vector<char>& buffer, const std::uint16_t value) {
            buffer.push_back(static_cast<char>(value & 0xFFU));
            buffer.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        }

        void append_u32(std::vector<char>& buffer, const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                buffer.push_back(static_cast<char>((value >> shift) & 0xFFU));
            }
        }

        std::uint16_t read_u16(const char* data) {
            return static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]) |
                                              (static_cast<unsigned char>(data[1]) << 8U));
        }

        std::uint32_t read_u32(const char* data) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 8U) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 16U) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(data[3])) << 24U);
        }

    } // namespace

    std::expected<WavInfo, std::string>
    write_wav_pcm16(const std::filesystem::path& path,
                    const std::span<const std::int16_t> samples,
                    const std::uint32_t sample_rate,
                    const std::uint16_t channels) {
        static_assert(std::endian::native == std::endian::little,
                      "wav_io 直接写 16-bit 小端样本，要求小端主机");

        if (sample_rate == 0) {
            return std::unexpected("sample_rate must be greater than zero");
        }
        if (channels == 0) {
            return std::unexpected("channels must be greater than zero");
        }
        const std::uint64_t data_bytes = static_cast<std::uint64_t>(samples.size_bytes());
        if (data_bytes > std::numeric_limits<std::uint32_t>::max() - kWavHeaderSize) {
            return std::unexpected("audio is too large for a WAV file");
        }

        std::vector<char> header;
        header.reserve(kWavHeaderSize);
        header.insert(header.end(), {'R', 'I', 'F', 'F'});
        append_u32(header, static_cast<std::uint32_t>(kWavHeaderSize - 8 + data_bytes));
        header.insert(header.end(), {'W', 'A', 'V', 'E'});
        header.insert(header.end(), {'f', 'm', 't', ' '});
        append_u32(header, 16);
        append_u16(header, kWaveFormatPcm);
        append_u16(header, channels);
        append_u32(header, sample_rate);
        append_u32(header, sample_rate * channels * 2U);
        append_u16(header, static_cast<std::uint16_t>(channels * 2U));
        append_u16(header, 16);
        header.insert(header.end(), {'d', 'a', 't', 'a'});
        append_u32(header, static_cast<std::uint32_t>(data_bytes));

        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return std::unexpected("cannot open " + path.string() + " for writing");
        }
        stream.write(header.data(), static_cast<std::streamsize>(header.size()));
        if (data_bytes > 0) {
            stream.write(reinterpret_cast<const char*>(samples.data()),
                         static_cast<std::streamsize>(data_bytes));
        }
        if (!stream) {
            return std::unexpected("failed to write " + path.string());
        }

        WavInfo info;
        info.sample_rate = sample_rate;
        info.channels = channels;
        info.bits_per_sample = 16;
        info.sample_frames = samples.size() / channels;
        info.duration_seconds = static_cast<float>(info.sample_frames) /
            static_cast<float>(sample_rate);
        return info;
    }

    std::expected<WavInfo, std::string> read_wav_info(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return std::unexpected("cannot open " + path.string());
        }

        char riff[12] = {};
        if (!stream.read(riff, sizeof(riff)) ||
            std::memcmp(riff, "RIFF", 4) != 0 ||
            std::memcmp(riff + 8, "WAVE", 4) != 0) {
            return std::unexpected(path.string() + " is not a RIFF/WAVE file");
        }

        WavInfo info;
        bool have_format = false;
        bool have_data = false;
        while (stream && !(have_format && have_data)) {
            char chunk_header[8] = {};
            if (!stream.read(chunk_header, sizeof(chunk_header))) {
                break;
            }
            const std::uint32_t chunk_size = read_u32(chunk_header + 4);
            if (std::memcmp(chunk_header, "fmt ", 4) == 0) {
                if (chunk_size < 16) {
                    return std::unexpected(path.string() + " has a truncated fmt chunk");
                }
                char format[16] = {};
                if (!stream.read(format, sizeof(format))) {
                    return std::unexpected(path.string() + " has a truncated fmt chunk");
                }
                const std::uint16_t audio_format = read_u16(format);
                if (audio_format != kWaveFormatPcm) {
                    return std::unexpected(path.string() + " is not PCM WAV");
                }
                info.channels = read_u16(format + 2);
                info.sample_rate = read_u32(format + 4);
                info.bits_per_sample = read_u16(format + 14);
                if (chunk_size > sizeof(format)) {
                    stream.seekg(static_cast<std::streamoff>(chunk_size - sizeof(format)),
                                 std::ios::cur);
                }
                have_format = true;
            }
            else if (std::memcmp(chunk_header, "data", 4) == 0) {
                if (!have_format || info.channels == 0 || info.sample_rate == 0 ||
                    info.bits_per_sample == 0) {
                    return std::unexpected(path.string() + " has data before a valid fmt chunk");
                }
                const std::uint64_t bytes_per_frame =
                    static_cast<std::uint64_t>(info.channels) * (info.bits_per_sample / 8U);
                if (bytes_per_frame == 0) {
                    return std::unexpected(path.string() + " has an invalid block alignment");
                }
                info.sample_frames = chunk_size / bytes_per_frame;
                info.duration_seconds = static_cast<float>(info.sample_frames) /
                    static_cast<float>(info.sample_rate);
                have_data = true;
            }
            else {
                stream.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
            }

            if ((chunk_size % 2U) != 0U) {
                stream.seekg(1, std::ios::cur);
            }
        }

        if (!have_format || !have_data) {
            return std::unexpected(path.string() + " is missing fmt/data chunks");
        }
        return info;
    }

} // namespace tts
