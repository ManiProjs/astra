#include <astra/assets/hdr.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace astra::assets {
namespace {

constexpr std::size_t maximum_header_line = 4096;

struct Axis final {
    char name{};
    char sign{};
    std::uint32_t length{};
};

class Decoder final {
public:
    Decoder(std::span<const std::byte> bytes, HdrDecodeOptions options)
        : bytes_(bytes), options_(options) {}

    std::expected<HdrImage, HdrDecodeError> decode() {
        if (!read_header()) return std::unexpected(error_);

        const std::uint64_t scanline_count = major_.length;
        const std::uint64_t scanline_width = minor_.length;
        const std::uint64_t minimum_scanline_bytes =
            scanline_width >= 8 && scanline_width <= 32767 ? 12U : scanline_width * 4U;
        if (scanline_count > 0 && minimum_scanline_bytes >
                std::numeric_limits<std::uint64_t>::max() / scanline_count) {
            return fail("HDR image dimensions overflow the encoded-size calculation.");
        }
        const std::uint64_t minimum_bytes = scanline_count * minimum_scanline_bytes;
        if (minimum_bytes > remaining())
            return fail("HDR pixel data is truncated.");

        const std::uint64_t pixel_count = static_cast<std::uint64_t>(width_) * height_;
        if (pixel_count > std::numeric_limits<std::size_t>::max() / (4U * sizeof(float)))
            return fail("HDR image dimensions are too large.");
        const std::size_t output_bytes =
            static_cast<std::size_t>(pixel_count) * 4U * sizeof(float);
        if (output_bytes > options_.maximum_output_bytes)
            return fail("HDR image exceeds the configured decoded-size limit.");
        const std::size_t component_count = static_cast<std::size_t>(pixel_count) * 4U;
        if (component_count > HdrImage{}.rgba32f.max_size())
            return fail("HDR image dimensions exceed the output container limit.");

        HdrImage image;
        image.width = width_;
        image.height = height_;
        try {
            image.rgba32f.resize(component_count);
        } catch (const std::bad_alloc&) {
            return fail("Unable to allocate the decoded HDR image.");
        } catch (const std::length_error&) {
            return fail("HDR image dimensions exceed the output container limit.");
        }

        std::vector<std::array<std::uint8_t, 4>> scanline;
        try {
            scanline.resize(minor_.length);
        } catch (const std::bad_alloc&) {
            return fail("Unable to allocate an HDR scanline.");
        } catch (const std::length_error&) {
            return fail("HDR scanline dimensions are too large.");
        }

        for (std::uint32_t row = 0; row < major_.length; ++row) {
            if (!read_scanline(scanline)) return std::unexpected(error_);
            for (std::uint32_t column = 0; column < minor_.length; ++column) {
                const std::uint32_t major_coordinate =
                    coordinate(major_, row);
                const std::uint32_t minor_coordinate =
                    coordinate(minor_, column);
                const std::uint32_t x = major_.name == 'X' ? major_coordinate : minor_coordinate;
                const std::uint32_t y = major_.name == 'Y' ? major_coordinate : minor_coordinate;
                const auto& rgbe = scanline[column];
                const float scale = rgbe[3] == 0
                    ? 0.0F
                    : std::ldexp(1.0F, static_cast<int>(rgbe[3]) - (128 + 8));
                const std::size_t destination =
                    (static_cast<std::size_t>(y) * width_ + x) * 4U;
                image.rgba32f[destination + 0] = static_cast<float>(rgbe[0]) * scale;
                image.rgba32f[destination + 1] = static_cast<float>(rgbe[1]) * scale;
                image.rgba32f[destination + 2] = static_cast<float>(rgbe[2]) * scale;
                image.rgba32f[destination + 3] = 1.0F;
            }
        }
        return image;
    }

private:
    std::span<const std::byte> bytes_;
    HdrDecodeOptions options_;
    std::size_t cursor_{};
    std::uint32_t width_{};
    std::uint32_t height_{};
    Axis major_{};
    Axis minor_{};
    HdrDecodeError error_{};

    std::size_t remaining() const { return bytes_.size() - cursor_; }

    std::uint8_t byte() const {
        return std::to_integer<std::uint8_t>(bytes_[cursor_]);
    }

    bool read_line(std::string_view& line) {
        const std::size_t start = cursor_;
        while (cursor_ < bytes_.size() && byte() != '\n') {
            if (cursor_ - start >= maximum_header_line) {
                error_.message = "HDR header line exceeds the supported length.";
                return false;
            }
            ++cursor_;
        }
        if (cursor_ == bytes_.size()) {
            error_.message = "HDR header is truncated before its terminating newline.";
            return false;
        }
        std::size_t end = cursor_++;
        if (end > start && std::to_integer<unsigned char>(bytes_[end - 1]) == '\r') --end;
        line = {reinterpret_cast<const char*>(bytes_.data() + start), end - start};
        return true;
    }

    bool read_header() {
        std::string_view line;
        if (!read_line(line)) return false;
        if (line != "#?RADIANCE" && line != "#?RGBE") {
            error_.message = "Input is not a Radiance RGBE image.";
            return false;
        }

        bool has_format = false;
        bool found_separator = false;
        while (read_line(line)) {
            if (line.empty()) {
                found_separator = true;
                break;
            }
            constexpr std::string_view prefix = "FORMAT=";
            if (line.starts_with(prefix)) {
                if (has_format || line.substr(prefix.size()) != "32-bit_rle_rgbe") {
                    error_.message = "HDR image has a duplicate or unsupported FORMAT declaration.";
                    return false;
                }
                has_format = true;
            }
        }
        if (!found_separator) {
            if (error_.message.empty()) error_.message = "HDR header is missing its blank separator.";
            return false;
        }
        if (!has_format) {
            error_.message = "HDR header is missing FORMAT=32-bit_rle_rgbe.";
            return false;
        }
        if (!read_line(line)) return false;
        return parse_resolution(line);
    }

    static bool parse_axis(std::string_view token, std::string_view dimension,
                           char expected_axis, Axis& axis) {
        if (token.size() != 2 || (token[0] != '+' && token[0] != '-') ||
            token[1] != expected_axis) return false;
        std::uint32_t length{};
        const char* first = dimension.data();
        const char* last = dimension.data() + dimension.size();
        const auto result = std::from_chars(first, last, length);
        if (result.ec != std::errc{} || result.ptr != last || length == 0) return false;
        axis = {expected_axis, token[0], length};
        return true;
    }

    bool parse_resolution(std::string_view line) {
        std::array<std::string_view, 4> tokens{};
        std::size_t count = 0;
        std::size_t position = 0;
        while (position < line.size()) {
            while (position < line.size() && (line[position] == ' ' || line[position] == '\t'))
                ++position;
            if (position == line.size()) break;
            const std::size_t start = position;
            while (position < line.size() && line[position] != ' ' && line[position] != '\t')
                ++position;
            if (count == tokens.size()) {
                error_.message = "HDR resolution line contains extra fields.";
                return false;
            }
            tokens[count++] = line.substr(start, position - start);
        }
        if (count != 4) {
            error_.message = "HDR resolution line must contain two axis and dimension pairs.";
            return false;
        }
        const char first_axis = tokens[0].size() > 1 ? tokens[0][1] : '\0';
        const char second_axis = tokens[2].size() > 1 ? tokens[2][1] : '\0';
        if ((first_axis != 'X' && first_axis != 'Y') ||
            (second_axis != 'X' && second_axis != 'Y') || first_axis == second_axis ||
            !parse_axis(tokens[0], tokens[1], first_axis, major_) ||
            !parse_axis(tokens[2], tokens[3], second_axis, minor_)) {
            error_.message = "HDR resolution line has invalid axis signs or dimensions.";
            return false;
        }
        width_ = major_.name == 'X' ? major_.length : minor_.length;
        height_ = major_.name == 'Y' ? major_.length : minor_.length;
        if (static_cast<std::uint64_t>(width_) * height_ >
            std::numeric_limits<std::size_t>::max() / (4U * sizeof(float))) {
            error_.message = "HDR image dimensions overflow the output-size calculation.";
            return false;
        }
        return true;
    }

    static std::uint32_t coordinate(const Axis& axis, std::uint32_t index) {
        // Radiance's X axis grows left-to-right; its Y axis grows bottom-to-top.
        const bool forward = axis.name == 'X' ? axis.sign == '+' : axis.sign == '-';
        return forward ? index : axis.length - 1U - index;
    }

    bool read_scanline(std::vector<std::array<std::uint8_t, 4>>& output) {
        const std::uint32_t width = minor_.length;
        const bool can_be_rle = width >= 8 && width <= 32767;
        if (can_be_rle && remaining() >= 4 && byte() == 2 &&
            std::to_integer<std::uint8_t>(bytes_[cursor_ + 1]) == 2 &&
            (std::to_integer<std::uint8_t>(bytes_[cursor_ + 2]) & 0x80U) == 0) {
            const std::uint32_t encoded_width =
                (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes_[cursor_ + 2])) << 8U) |
                std::to_integer<std::uint8_t>(bytes_[cursor_ + 3]);
            cursor_ += 4;
            if (encoded_width != width) {
                error_.message = "HDR RLE scanline width does not match the resolution.";
                return false;
            }
            for (std::size_t channel = 0; channel < 4; ++channel) {
                std::uint32_t decoded = 0;
                while (decoded < width) {
                    if (remaining() < 1) {
                        error_.message = "HDR RLE scanline is truncated before a packet code.";
                        return false;
                    }
                    const std::uint8_t code = byte();
                    ++cursor_;
                    if (code == 0) {
                        error_.message = "HDR RLE scanline contains a zero-length packet.";
                        return false;
                    }
                    if (code > 128) {
                        const std::uint32_t run = code - 128U;
                        if (run > width - decoded || remaining() < 1) {
                            error_.message = "HDR RLE run is oversized or truncated.";
                            return false;
                        }
                        const std::uint8_t value = byte();
                        ++cursor_;
                        for (std::uint32_t i = 0; i < run; ++i)
                            output[decoded++][channel] = value;
                    } else {
                        const std::uint32_t literal = code;
                        if (literal > width - decoded || remaining() < literal) {
                            error_.message = "HDR RLE literal packet is oversized or truncated.";
                            return false;
                        }
                        for (std::uint32_t i = 0; i < literal; ++i)
                            output[decoded++][channel] = byte(), ++cursor_;
                    }
                }
            }
            return true;
        }

        const std::uint64_t bytes_needed = static_cast<std::uint64_t>(width) * 4U;
        if (bytes_needed > remaining()) {
            error_.message = "HDR flat scanline is truncated.";
            return false;
        }
        for (std::uint32_t i = 0; i < width; ++i) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                output[i][channel] = byte();
                ++cursor_;
            }
        }
        return true;
    }

    std::unexpected<HdrDecodeError> fail(std::string message) {
        return std::unexpected(HdrDecodeError{std::move(message)});
    }
};

} // namespace

std::expected<HdrImage, HdrDecodeError> decode_hdr(std::span<const std::byte> encoded,
                                                   HdrDecodeOptions options) {
    if (encoded.empty())
        return std::unexpected(HdrDecodeError{"HDR input is empty."});
    return Decoder(encoded, options).decode();
}

} // namespace astra::assets
