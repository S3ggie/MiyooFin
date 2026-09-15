#ifndef MIYOOFIN_UPDATE_MANIFEST_HPP
#define MIYOOFIN_UPDATE_MANIFEST_HPP

#include <cstdint>
#include <string>

namespace miyoofin {

/// A single download asset referenced by the manifest.
struct UpdateAsset {
    std::string url;
    std::string sha256;  // 64-char lowercase hex
    std::uint64_t size = 0;
};

/// Parsed OTA update manifest.
struct UpdateManifest {
    std::string name;
    std::string version;
    std::string tag;
    std::string minVersion;
    std::string notes;
    UpdateAsset tarGz;
    UpdateAsset zip;
};

/// Parse the OTA manifest JSON.
///
/// Required fields: `version`, `assets.tar_gz.url`, `assets.tar_gz.sha256`,
/// `assets.tar_gz.size`.  All others are optional.  SHA-256 values are
/// validated to be exactly 64 hex characters and normalised to lowercase.
/// Robust to key order, whitespace, and nested objects.
bool parseUpdateManifest(const std::string &json, UpdateManifest &out);

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_MANIFEST_HPP
