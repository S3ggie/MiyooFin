#ifndef MIYOOFIN_IMAGE_DECODER_HPP
#define MIYOOFIN_IMAGE_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace miyoofin {

/// A decoded image in 4-channel RGBA format.
struct DecodedImage
{
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels; ///< RGBA, 4 bytes per pixel

    bool empty() const
    {
        return width == 0 || height == 0 || pixels.empty();
    }
};

/// Memory budget for one decoded image (the target has ~128 MB for everything). Artwork is
/// requested at a few hundred pixels, so this is generous (1920x1080 fits); a server that
/// ignores the requested size cannot make a 64 MB bitmap. The header is read first and the pixel
/// buffer is only allocated when the dimensions fit.
inline constexpr std::size_t kMaxDecodedPixels = 2'500'000;
inline constexpr int kMaxImageDimension = 8192;
inline constexpr std::size_t kMaxCompressedImageBytes = 16u * 1024u * 1024u;

/// JPEG decoding using stb_image.
/// All methods are synchronous and stateless.
class ImageDecoder
{
  public:
    /// Decode JPEG data from memory into RGBA pixels.
    /// Returns an empty DecodedImage on failure (never throws).
    static DecodedImage decodeJpeg(const unsigned char* data, size_t size);

    /// Decode JPEG data from a file on disk.
    /// Returns an empty DecodedImage on failure.
    static DecodedImage decodeJpegFile(const char* path);
};

} // namespace miyoofin

#endif // MIYOOFIN_IMAGE_DECODER_HPP
