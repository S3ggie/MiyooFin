#ifndef MIYOOFIN_MPEG_TS_VALIDATOR_HPP
#define MIYOOFIN_MPEG_TS_VALIDATOR_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace miyoofin {

/// Structural validation of the one segment container the download pipeline asks the server for:
/// MPEG transport stream carrying H.264 video and AAC audio (the HLS profile in HlsProfile.hpp).
///
/// What a pass means: the file is a whole number of 188-byte packets, every packet starts with the
/// sync byte and carries no transport-error flag, the stream opens with a valid PAT and PMT (their
/// CRC-32 checks out) that declare H.264 and AAC, continuity counters run unbroken per PID, and
/// the elementary-stream packets that start a frame start with a PES header. Error pages, text,
/// zero fill, misaligned or cut-off packets and most mid-file damage fail.
///
/// What it does NOT mean: the payload is not decoded, so a flipped bit inside a video frame, or a
/// file cut exactly on a packet boundary when the server gave no length, still passes. Content
/// integrity needs a digest from the server, which Jellyfin does not provide for transcodes. The
/// downloader additionally compares the byte count with the announced Content-Length.
enum class TsDepth
{
    /// Header (PAT/PMT) plus a sample of packets across the file and the last one: cheap enough to
    /// run on every segment of a long movie when the library is checked.
    Quick,
    /// Every packet: run once on a freshly downloaded segment, before it is accepted.
    Full
};

struct TsVerdict
{
    bool ok = false;
    std::string reason; // why it failed (empty when ok)
};

TsVerdict validateMpegTs(const unsigned char* data, std::size_t size, TsDepth depth);
/// Reads the file itself (`size` is its length): Full streams it in chunks, Quick reads about a
/// dozen small ranges.
TsVerdict validateMpegTsFile(const std::string& path, std::uint64_t size, TsDepth depth);

/// CRC-32/MPEG-2 as used by PSI sections (exposed for tests).
std::uint32_t mpegCrc32(const unsigned char* data, std::size_t size);

} // namespace miyoofin

#endif
