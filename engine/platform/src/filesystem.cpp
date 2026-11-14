#include <astra/platform/filesystem.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>

namespace astra::platform::filesystem {

std::expected<bool, Error> exists(const Path& path) {
    Error error;
    const bool result = std::filesystem::exists(path, error);
    if (error) {
        return std::unexpected(error);
    }
    return result;
}

std::expected<void, Error> create_directories(const Path& path) {
    Error error;
    std::filesystem::create_directories(path, error);
    if (error) {
        return std::unexpected(error);
    }
    return {};
}

std::expected<ByteBuffer, Error> read_file(const Path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    if (static_cast<std::uintmax_t>(end) >
        static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(std::make_error_code(std::errc::file_too_large));
    }
    if (static_cast<std::uintmax_t>(end) >
        static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(std::make_error_code(std::errc::file_too_large));
    }

    ByteBuffer contents(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!contents.empty() && !input.read(reinterpret_cast<char*>(contents.data()),
                                         static_cast<std::streamsize>(contents.size()))) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    return contents;
}

std::expected<std::string, Error> read_text_file(const Path& path) {
    auto contents = read_file(path);
    if (!contents) {
        return std::unexpected(contents.error());
    }

    if (contents->empty()) {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char*>(contents->data()), contents->size());
}

std::expected<void, Error> write_file(const Path& path, std::span<const std::byte> contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    if (!contents.empty()) {
        if (contents.size() >
            static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return std::unexpected(std::make_error_code(std::errc::file_too_large));
        }
        output.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
    }
    if (!output) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    output.flush();
    if (!output) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    output.close();
    if (output.fail()) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    return {};
}

std::expected<void, Error> write_text_file(const Path& path, std::string_view contents) {
    const auto* data = reinterpret_cast<const std::byte*>(contents.data());
    return write_file(path, std::span<const std::byte>{data, contents.size()});
}

std::expected<std::vector<Path>, Error> list_directory(const Path& path) {
    Error error;
    std::filesystem::directory_iterator iterator(path, error);
    if (error) {
        return std::unexpected(error);
    }

    std::vector<Path> entries;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        entries.push_back(iterator->path());
        iterator.increment(error);
        if (error) {
            return std::unexpected(error);
        }
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

} // namespace astra::platform::filesystem
