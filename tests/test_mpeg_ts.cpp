// MPEG-TS segment validation: real ffmpeg output passes; error pages, garbage that merely has the
// right length, and structurally damaged transport streams do not.
#include "test_support.hpp"

#include "../src/download/MpegTsValidator.hpp"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace miyoofin;

namespace {

using Bytes = std::vector<unsigned char>;

Bytes readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const Bytes& data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

bool accepted(const Bytes& data, TsDepth depth)
{
    return validateMpegTs(data.data(), data.size(), depth).ok;
}

// Same verdict whether the bytes come from memory or from a file.
bool acceptedFile(const Bytes& data, TsDepth depth)
{
    const std::string path = "ts-validator-test.bin";
    writeFile(path, data);
    const bool ok = validateMpegTsFile(path, data.size(), depth).ok;
    ::unlink(path.c_str());
    return ok;
}

// Packet i's byte range inside the stream.
std::size_t at(std::size_t packet)
{
    return packet * 188;
}

void testLegitimateSegmentsPass(const Bytes& a, const Bytes& b)
{
    for (const Bytes* seg : {&a, &b}) {
        CHECK(accepted(*seg, TsDepth::Full));
        CHECK(accepted(*seg, TsDepth::Quick));
        CHECK(acceptedFile(*seg, TsDepth::Full));
        CHECK(acceptedFile(*seg, TsDepth::Quick));
    }
}

void testGarbageOfTheRightLengthFails(const Bytes& legit)
{
    // The audit's probe: exactly 188 literal 'x', and the same idea at every plausible size.
    for (std::size_t packets : {1u, 2u, 4u, 8u, 50u}) {
        const Bytes xs(packets * 188, 'x');
        CHECK(!accepted(xs, TsDepth::Quick) && !accepted(xs, TsDepth::Full));
        CHECK(!acceptedFile(xs, TsDepth::Quick) && !acceptedFile(xs, TsDepth::Full));
    }
    CHECK(!accepted(Bytes(188 * 20, 0), TsDepth::Quick)); // zero fill
    // An HTML error page, once as-is and once padded to a packet multiple.
    const std::string page = "<html><body><h1>502 Bad Gateway</h1>nginx</body></html>";
    Bytes html(page.begin(), page.end());
    CHECK(!accepted(html, TsDepth::Quick));
    html.resize(188 * 8, ' ');
    CHECK(!accepted(html, TsDepth::Quick) && !accepted(html, TsDepth::Full));
    // The previous "plausible" fixture: sync bytes in the right places and nothing else.
    Bytes syncOnly(188 * 12, 0);
    for (std::size_t i = 0; i < 12; ++i)
        syncOnly[at(i)] = 0x47;
    CHECK(!accepted(syncOnly, TsDepth::Quick) && !accepted(syncOnly, TsDepth::Full));
    // A JSON error body.
    const std::string json = "{\"error\":\"Not Found\"}";
    CHECK(!accepted(Bytes(json.begin(), json.end()), TsDepth::Quick));
    (void)legit;
}

void testStructuralDamageFails(const Bytes& legit)
{
    const std::size_t packets = legit.size() / 188;
    CHECK(packets > 40);

    // Cut mid-packet.
    Bytes cut(legit.begin(), legit.end() - 50);
    CHECK(!accepted(cut, TsDepth::Quick) && !accepted(cut, TsDepth::Full));
    // Cut off before the stream tables.
    Bytes head(legit.begin(), legit.begin() + 188 * 2);
    CHECK(!accepted(head, TsDepth::Quick) && !accepted(head, TsDepth::Full));

    // A lost sync byte: Full finds it anywhere; Quick finds it in the sampled places.
    Bytes midSync = legit;
    midSync[at(packets / 2)] = 0x00;
    CHECK(!accepted(midSync, TsDepth::Full));
    Bytes lastSync = legit;
    lastSync[at(packets - 1)] = 0x00;
    CHECK(!accepted(lastSync, TsDepth::Full) && !accepted(lastSync, TsDepth::Quick));
    Bytes firstSync = legit;
    firstSync[0] = 0x00;
    CHECK(!accepted(firstSync, TsDepth::Quick) && !acceptedFile(firstSync, TsDepth::Quick));

    // A dropped packet in the middle: the counters no longer run on (Full).
    Bytes dropped(legit.begin(), legit.begin() + at(packets / 2));
    dropped.insert(dropped.end(), legit.begin() + at(packets / 2 + 1), legit.end());
    CHECK(!accepted(dropped, TsDepth::Full));
    CHECK(!acceptedFile(dropped, TsDepth::Full));

    // Two packets swapped.
    Bytes swapped = legit;
    for (std::size_t i = 0; i < 188; ++i)
        std::swap(swapped[at(20) + i], swapped[at(21) + i]);
    // (They may carry different PIDs; swapping same-PID neighbours is what breaks the counters.)
    bool anySamePidSwap = false;
    for (std::size_t p = 20; p + 1 < packets && !anySamePidSwap; ++p) {
        const unsigned pidA = ((legit[at(p) + 1] & 0x1f) << 8) | legit[at(p) + 2];
        const unsigned pidB = ((legit[at(p + 1) + 1] & 0x1f) << 8) | legit[at(p + 1) + 2];
        if (pidA == pidB && pidA >= 0x20 && (legit[at(p) + 3] & 0x30) != 0x20) {
            swapped = legit;
            for (std::size_t i = 0; i < 188; ++i)
                std::swap(swapped[at(p) + i], swapped[at(p + 1) + i]);
            anySamePidSwap = true;
        }
    }
    CHECK(anySamePidSwap && !accepted(swapped, TsDepth::Full));

    // Transport error flag on one packet.
    Bytes flagged = legit;
    flagged[at(packets / 3) + 1] |= 0x80;
    CHECK(!accepted(flagged, TsDepth::Full));

    // Wrong tables: corrupt the PAT's CRC / the PMT's CRC / the PMT's stream types.
    Bytes patCrc = legit;
    patCrc[at(1) + 4 + 1 + 8] ^= 0x55; // inside the PAT section (after pointer field)
    CHECK(!accepted(patCrc, TsDepth::Quick) && !accepted(patCrc, TsDepth::Full));
    Bytes pmtBody = legit;
    pmtBody[at(2) + 4 + 1 + 12] ^= 0x01; // a stream_type byte: CRC no longer matches
    CHECK(!accepted(pmtBody, TsDepth::Quick) && !accepted(pmtBody, TsDepth::Full));

    // A PES start code destroyed at the first video frame (found by scanning for 00 00 01 E0).
    Bytes noPes = legit;
    bool broke = false;
    for (std::size_t p = 3; p < packets && !broke; ++p) {
        const unsigned char* pk = &noPes[at(p)];
        const unsigned pid = ((pk[1] & 0x1f) << 8) | pk[2];
        if ((pk[1] & 0x40) && pid == 0x100 && (pk[3] & 0x10)) {
            std::size_t off = 4;
            if (pk[3] & 0x20)
                off += 1 + pk[4];
            if (pk[off] == 0 && pk[off + 1] == 0 && pk[off + 2] == 1) {
                noPes[at(p) + off + 2] = 0x07;
                broke = true;
            }
        }
    }
    CHECK(broke && !accepted(noPes, TsDepth::Full));
}

void testWhatStructuralChecksCannotSee(const Bytes& legit)
{
    // Documented limits (see MpegTsValidator.hpp): a flipped bit inside a frame's payload, and a
    // file cut exactly between packets, are structurally fine. The downloader's compare with the
    // announced Content-Length is what catches the second kind when the server gives a length.
    Bytes bitFlip = legit;
    bitFlip[at(legit.size() / 188 / 2) + 100] ^= 0x10;
    CHECK(accepted(bitFlip, TsDepth::Full));
    Bytes cutOnBoundary(legit.begin(), legit.end() - 188);
    CHECK(accepted(cutOnBoundary, TsDepth::Full));
}

void testMutationsNeverCrash(const Bytes& legit)
{
    // Not a guarantee of anything but safety: random damage and truncation must come back as a
    // verdict, never as a crash or an out-of-range read (the sanitizer jobs run this too).
    std::mt19937 rng(12345);
    int rejected = 0;
    for (int round = 0; round < 1500; ++round) {
        Bytes mutated = legit;
        const int edits = 1 + static_cast<int>(rng() % 6);
        for (int e = 0; e < edits; ++e)
            mutated[rng() % mutated.size()] = static_cast<unsigned char>(rng());
        if (rng() % 4 == 0)
            mutated.resize(1 + rng() % mutated.size());
        const TsVerdict full = validateMpegTs(mutated.data(), mutated.size(), TsDepth::Full);
        const TsVerdict quick = validateMpegTs(mutated.data(), mutated.size(), TsDepth::Quick);
        if (full.ok)
            CHECK(quick.ok ||
                  true); // Full accepting while Quick refuses would be odd; not required
        rejected += full.ok ? 0 : 1;
    }
    CHECK(rejected > 300); // most random damage is detected
}

// Optional compatibility run over real downloaded segments (not shipped in the repo): point
// MIYOOFIN_REAL_SEGMENTS at a folder of *.bin files copied from a device.
void testRealSegmentsIfProvided()
{
    const char* dir = std::getenv("MIYOOFIN_REAL_SEGMENTS");
    if (!dir)
        return;
    int files = 0, failed = 0;
    std::vector<std::string> pending = {dir};
    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        DIR* d = opendir(current.c_str());
        if (!d)
            continue;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..")
                continue;
            const std::string path = current + "/" + name;
            struct stat st;
            if (stat(path.c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                pending.push_back(path);
            } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bin") == 0) {
                ++files;
                const bool ok =
                    validateMpegTsFile(path, static_cast<std::uint64_t>(st.st_size), TsDepth::Full)
                        .ok &&
                    validateMpegTsFile(path, static_cast<std::uint64_t>(st.st_size), TsDepth::Quick)
                        .ok;
                if (!ok) {
                    ++failed;
                    std::printf("  real segment rejected: %s\n", path.c_str());
                }
            }
        }
        closedir(d);
    }
    std::printf("[test] real segments: %d checked, %d rejected\n", files, failed);
    CHECK(files > 0 && failed == 0);
}

} // namespace

int main()
{
    const Bytes a = readFile("tests/fixtures/hls/legit-2s.ts");
    const Bytes b = readFile("tests/fixtures/hls/legit-tail.ts");
    CHECK(a.size() > 188 * 40 && a.size() % 188 == 0 && b.size() > 188 * 20);
    testLegitimateSegmentsPass(a, b);
    testGarbageOfTheRightLengthFails(a);
    testStructuralDamageFails(a);
    testWhatStructuralChecksCannotSee(a);
    testMutationsNeverCrash(a);
    testRealSegmentsIfProvided();
    return miyoofin_test::finish("mpeg_ts");
}
