#include <astra/assets/hdr.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace astra::assets;

namespace {

void append(std::vector<std::byte>& bytes, std::initializer_list<std::uint8_t> values) {
    for (const auto value : values) bytes.push_back(static_cast<std::byte>(value));
}

std::vector<std::byte> header(std::string_view resolution,
                              std::string_view format = "32-bit_rle_rgbe") {
    const std::string text = "#?RADIANCE\nFORMAT=" + std::string(format) + "\n\n" +
                            std::string(resolution) + "\n";
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char value : text) result.push_back(static_cast<std::byte>(value));
    return result;
}

void append_flat_pixel(std::vector<std::byte>& bytes, std::uint8_t r, std::uint8_t g,
                       std::uint8_t b, std::uint8_t e) {
    append(bytes, {r, g, b, e});
}

bool near(float a, float b) { return std::abs(a - b) < 1.0e-6F; }

void expect_pixel(const HdrImage& image, std::uint32_t x, std::uint32_t y,
                  float r, float g, float b) {
    const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
    assert(near(image.rgba32f[offset], r));
    assert(near(image.rgba32f[offset + 1], g));
    assert(near(image.rgba32f[offset + 2], b));
    assert(image.rgba32f[offset + 3] == 1.0F);
}

} // namespace

int main() {
    // Flat RGBE data, with the standard 2^(E-136) conversion to linear float.
    auto flat = header("-Y 1 +X 2");
    append_flat_pixel(flat, 128, 64, 32, 136);
    append_flat_pixel(flat, 255, 0, 128, 135);
    const auto decoded_flat = decode_hdr(flat);
    assert(decoded_flat && decoded_flat->width == 2 && decoded_flat->height == 1);
    expect_pixel(*decoded_flat, 0, 0, 128.0F, 64.0F, 32.0F);
    expect_pixel(*decoded_flat, 1, 0, 127.5F, 0.0F, 64.0F);

    // Modern component RLE with literal and run packets across its channels.
    auto rle = header("-Y 1 +X 8");
    append(rle, {2, 2, 0, 8});
    append(rle, {8, 128, 128, 128, 128, 128, 128, 128, 128});
    append(rle, {136, 64});
    append(rle, {136, 32});
    append(rle, {136, 136});
    const auto decoded_rle = decode_hdr(rle);
    assert(decoded_rle && decoded_rle->width == 8);
    for (std::uint32_t x = 0; x < 8; ++x)
        expect_pixel(*decoded_rle, x, 0, 128.0F, 64.0F, 32.0F);

    // Both axis orders and all four orientation sign combinations normalize to top-left origin.
    auto reverse_x = header("-Y 1 -X 2");
    append_flat_pixel(reverse_x, 20, 0, 0, 136);
    append_flat_pixel(reverse_x, 10, 0, 0, 136);
    const auto decoded_reverse_x = decode_hdr(reverse_x);
    assert(decoded_reverse_x);
    expect_pixel(*decoded_reverse_x, 0, 0, 10, 0, 0);
    expect_pixel(*decoded_reverse_x, 1, 0, 20, 0, 0);

    auto bottom_up = header("+Y 2 +X 1");
    append_flat_pixel(bottom_up, 0, 20, 0, 136); // bottom row
    append_flat_pixel(bottom_up, 0, 10, 0, 136); // top row
    const auto decoded_bottom_up = decode_hdr(bottom_up);
    assert(decoded_bottom_up);
    expect_pixel(*decoded_bottom_up, 0, 0, 0, 10, 0);
    expect_pixel(*decoded_bottom_up, 0, 1, 0, 20, 0);

    std::vector<std::byte> crlf;
    const std::string crlf_text = "#?RGBE\r\nEXPOSURE=1.0\r\nFORMAT=32-bit_rle_rgbe\r\n\r\n-Y 1 +X 1\r\n";
    for (const char value : crlf_text) crlf.push_back(static_cast<std::byte>(value));
    append_flat_pixel(crlf, 64, 32, 16, 136);
    const auto decoded_crlf = decode_hdr(crlf);
    assert(decoded_crlf);
    expect_pixel(*decoded_crlf, 0, 0, 64, 32, 16);

    auto swapped_axes = header("+X 2 -Y 2");
    append_flat_pixel(swapped_axes, 1, 0, 0, 136);
    append_flat_pixel(swapped_axes, 2, 0, 0, 136);
    append_flat_pixel(swapped_axes, 3, 0, 0, 136);
    append_flat_pixel(swapped_axes, 4, 0, 0, 136);
    const auto decoded_swapped = decode_hdr(swapped_axes);
    assert(decoded_swapped);
    expect_pixel(*decoded_swapped, 0, 0, 1, 0, 0);
    expect_pixel(*decoded_swapped, 1, 0, 3, 0, 0);
    expect_pixel(*decoded_swapped, 0, 1, 2, 0, 0);
    expect_pixel(*decoded_swapped, 1, 1, 4, 0, 0);

    assert(!decode_hdr({}));
    auto bad_magic = flat;
    bad_magic[0] = std::byte{'!'};
    assert(!decode_hdr(bad_magic));
    assert(!decode_hdr(header("-Y 1 +X 1", "32-bit_rgbe")));
    assert(!decode_hdr(header("-Y 1 +X 1"))); // truncated pixel
    assert(!decode_hdr(header("-Y 1 +X 2"))); // truncated flat scanline
    assert(!decode_hdr(header("-Y 1 +X 8"))); // truncated RLE marker/data
    HdrDecodeOptions tiny_output_limit;
    tiny_output_limit.maximum_output_bytes = 16;
    assert(!decode_hdr(flat, tiny_output_limit));

    auto bad_rle_width = header("-Y 1 +X 8");
    append(bad_rle_width, {2, 2, 0, 7});
    assert(!decode_hdr(bad_rle_width));
    auto zero_packet = header("-Y 1 +X 8");
    append(zero_packet, {2, 2, 0, 8, 0});
    assert(!decode_hdr(zero_packet));
    auto oversized_run = header("-Y 1 +X 8");
    append(oversized_run, {2, 2, 0, 8, 137, 42});
    assert(!decode_hdr(oversized_run));
    auto truncated_literal = header("-Y 1 +X 8");
    append(truncated_literal, {2, 2, 0, 8, 8, 1, 2});
    assert(!decode_hdr(truncated_literal));

    assert(!decode_hdr(header("-Y 0 +X 1")));
    assert(!decode_hdr(header("-Y 1 +X 1 extra")));
    assert(!decode_hdr(header("-Y 4294967295 +X 4294967295")));
    std::vector<std::byte> missing_format;
    for (const char value : std::string_view("#?RADIANCE\n\n-Y 1 +X 1\n"))
        missing_format.push_back(static_cast<std::byte>(value));
    assert(!decode_hdr(missing_format));
    const std::array<std::byte, 2> bad_header{std::byte{'#'}, std::byte{'?'}};
    assert(!decode_hdr(bad_header));
}
