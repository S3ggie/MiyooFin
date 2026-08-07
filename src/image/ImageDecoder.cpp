#include "ImageDecoder.hpp"
#include "../../third_party/stb_image.h"
#include <cstdio>

namespace miyoofin {

DecodedImage ImageDecoder::decodeJpeg(const unsigned char *data, size_t size)
{
    if (!data || size == 0)
        return {};

    int w = 0, h = 0, channels = 0;

    // Request 4 channels (RGBA) regardless of source format.
    unsigned char *pixels = stbi_load_from_memory(
        data, static_cast<int>(size),
        &w, &h, &channels, 4);

    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return {};
    }

    DecodedImage img;
    img.width = w;
    img.height = h;

    // Copy into our own vector so the caller owns the memory.
    size_t pixelBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    img.pixels.assign(pixels, pixels + pixelBytes);

    stbi_image_free(pixels);
    return img;
}

DecodedImage ImageDecoder::decodeJpegFile(const char *path)
{
    if (!path)
        return {};

    FILE *f = std::fopen(path, "rb");
    if (!f)
        return {};

    // Seek to end to get file size
    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (fileSize <= 0) {
        std::fclose(f);
        return {};
    }

    std::vector<unsigned char> buf(static_cast<size_t>(fileSize));
    size_t bytesRead = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);

    if (bytesRead == 0)
        return {};

    return decodeJpeg(buf.data(), bytesRead);
}

} // namespace miyoofin
