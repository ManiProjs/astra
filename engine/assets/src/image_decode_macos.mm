#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <astra/assets/image.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace astra::assets {

std::expected<DecodedImage, Error> decode_image(std::span<const std::byte> encoded,
                                                std::string_view mime_type) {
    (void)mime_type;
    if (encoded.empty() || encoded.size() > std::numeric_limits<CFIndex>::max()) {
        return std::unexpected(Error{{}, "Image payload is empty or too large."});
    }
    @autoreleasepool {
        NSData *data = [NSData dataWithBytes:encoded.data() length:encoded.size()];
        CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
        if (source == nullptr)
            return std::unexpected(Error{{}, "ImageIO could not read the encoded image."});
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        CFRelease(source);
        if (image == nullptr)
            return std::unexpected(Error{{}, "ImageIO could not decode the image."});

        const size_t width = CGImageGetWidth(image);
        const size_t height = CGImageGetHeight(image);
        if (width == 0 || height == 0 || width > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::uint32_t>::max() ||
            width > std::numeric_limits<std::size_t>::max() / 4U / height) {
            CGImageRelease(image);
            return std::unexpected(Error{{}, "Decoded image dimensions are invalid or too large."});
        }

        DecodedImage result;
        result.width = static_cast<std::uint32_t>(width);
        result.height = static_cast<std::uint32_t>(height);
        result.rgba8.resize(width * height * 4U);
        CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(
            result.rgba8.data(), width, height, 8, width * 4U, color_space,
            static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big) |
                static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        if (context == nullptr) {
            if (color_space != nullptr) CGColorSpaceRelease(color_space);
            CGImageRelease(image);
            return std::unexpected(Error{{}, "CoreGraphics could not allocate the image buffer."});
        }
        CGContextTranslateCTM(context, 0.0, static_cast<CGFloat>(height));
        CGContextScaleCTM(context, 1.0, -1.0);
        CGContextDrawImage(context, CGRectMake(0.0, 0.0, width, height), image);
        CGContextRelease(context);
        if (color_space != nullptr) CGColorSpaceRelease(color_space);
        CGImageRelease(image);

        // Remove CoreGraphics' premultiplication so glTF base-color alpha remains straight.
        for (std::size_t offset = 0; offset < result.rgba8.size(); offset += 4U) {
            const auto alpha = std::to_integer<unsigned char>(result.rgba8[offset + 3U]);
            if (alpha == 0) {
                result.rgba8[offset] = result.rgba8[offset + 1U] =
                    result.rgba8[offset + 2U] = std::byte{0};
            } else if (alpha != 255) {
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    const auto value = std::to_integer<unsigned char>(result.rgba8[offset + channel]);
                    const auto straight = std::min(255U, (static_cast<unsigned>(value) * 255U + alpha / 2U) / alpha);
                    result.rgba8[offset + channel] = static_cast<std::byte>(straight);
                }
            }
        }
        return result;
    }
}

} // namespace astra::assets
