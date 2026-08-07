#ifndef MIYOOFIN_IMAGE_DECODER_HPP
#define MIYOOFIN_IMAGE_DECODER_HPP

#include <cstdint>
#include <vector>

namespace miyoofin {

/// A decoded image in 4-channel RGBA format.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;  ///< RGBA, 4 bytes per pixel

    bool empty() const { return width == 0 || height == 0 || pixels.empty(); }
};

/// JPEG decoding using stb_image.
/// All methods are synchronous and stateless.
class ImageDecoder {
public:
    /// Decode JPEG data from memory into RGBA pixels.
    /// Returns an empty DecodedImage on failure (never throws).
    static DecodedImage decodeJpeg(const unsigned char *data, size_t size);

    /// Decode JPEG data from a file on disk.
    /// Returns an empty DecodedImage on failure.
    static DecodedImage decodeJpegFile(const char *path);
};

} // namespace miyoofin

#endif // MIYOOFIN_IMAGE_DECODER_HPP
