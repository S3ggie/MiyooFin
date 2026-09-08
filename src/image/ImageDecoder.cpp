#include "ImageDecoder.hpp"
#include "../../third_party/stb_image.h"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include <limits>
#include <cstdio>

namespace miyoofin {

static void recordDecodeTelemetry(TelemetryTimer &timer, size_t inputBytes,
                                   bool success, size_t outputBytes) noexcept
{
    PerformanceTelemetry &telemetry = performanceTelemetry();
    if (!timer.active() || !telemetry.enabledFast())
        return;

    const uint64_t durationUs = timer.elapsedUs();
    telemetry.recordArtworkDecode(success, durationUs);
    TelemetryRecord record{};
    record.header.record_type = RecordType::ArtworkDecode;
    record.payload.artwork_decode.context = static_cast<uint8_t>(
        currentArtworkContext());
    record.payload.artwork_decode.outcome = static_cast<uint8_t>(
        success ? Outcome::Success : Outcome::Failure);
    record.payload.artwork_decode.duration_us = durationUs;
    record.payload.artwork_decode.compressed_input_bytes = inputBytes;
    record.payload.artwork_decode.decoded_rgba_bytes = outputBytes;
    telemetry.emitRecord(record);
}

DecodedImage ImageDecoder::decodeJpeg(const unsigned char *data, size_t size)
{
    TelemetryTimer timer;
    if (!data || size == 0) {
        recordDecodeTelemetry(timer, size, false, 0);
        return {};
    }

    int w = 0, h = 0, channels = 0;

    // Request 4 channels (RGBA) regardless of source format.
    unsigned char *pixels = stbi_load_from_memory(
        data, static_cast<int>(size),
        &w, &h, &channels, 4);

    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        recordDecodeTelemetry(timer, size, false, 0);
        return {};
    }

    // Copy into our own vector so the caller owns the memory.
    const size_t width = static_cast<size_t>(w);
    const size_t height = static_cast<size_t>(h);
    const bool outputSizeFits = width <= std::numeric_limits<size_t>::max() / 4
        && height <= (std::numeric_limits<size_t>::max() / 4) / width;
    if (!outputSizeFits) {
        stbi_image_free(pixels);
        recordDecodeTelemetry(timer, size, false, 0);
        return {};
    }
    const size_t pixelBytes = width * height * 4;
    DecodedImage img;
    img.width = w;
    img.height = h;
    img.pixels.assign(pixels, pixels + pixelBytes);

    stbi_image_free(pixels);
    recordDecodeTelemetry(timer, size, true, pixelBytes);
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
