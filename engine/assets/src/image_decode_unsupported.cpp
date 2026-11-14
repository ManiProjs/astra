#include <astra/assets/image.hpp>

namespace astra::assets {

std::expected<DecodedImage, Error> decode_image(std::span<const std::byte> encoded,
                                                std::string_view mime_type) {
    (void)encoded;
    (void)mime_type;
    return std::unexpected(Error{{}, "Image decoding is not implemented for this platform."});
}

} // namespace astra::assets
