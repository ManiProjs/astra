#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace astra::platform::filesystem {

using Path = std::filesystem::path;
using Error = std::error_code;
using ByteBuffer = std::vector<std::byte>;

[[nodiscard]] std::expected<bool, Error> exists(const Path& path);
[[nodiscard]] std::expected<void, Error> create_directories(const Path& path);
[[nodiscard]] std::expected<ByteBuffer, Error> read_file(const Path& path);
[[nodiscard]] std::expected<std::string, Error> read_text_file(const Path& path);
[[nodiscard]] std::expected<void, Error> write_file(const Path& path,
                                                    std::span<const std::byte> contents);
[[nodiscard]] std::expected<void, Error> write_text_file(const Path& path,
                                                         std::string_view contents);
[[nodiscard]] std::expected<std::vector<Path>, Error> list_directory(const Path& path);

} // namespace astra::platform::filesystem
