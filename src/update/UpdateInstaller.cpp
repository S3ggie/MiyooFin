#include "UpdateInstaller.hpp"
#include "UpdateInstallPlan.hpp"
#include "miyoofin/version.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace miyoofin {

// -------------------------------------------------------------------
// ELF / ARM helpers
// -------------------------------------------------------------------

bool isElfMagic(const unsigned char *buf, std::size_t len)
{
    if (len < 4) return false;
    return buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F';
}

bool isElfArm(const unsigned char *buf, std::size_t len)
{
    // ELF header: e_machine is at offset 18 (2 bytes, little-endian on ARM).
    // ARM machine number is 0x28.
    if (len < 20) return false;
    if (!isElfMagic(buf, len)) return false;
    std::uint16_t e_machine = static_cast<std::uint16_t>(buf[18] | (buf[19] << 8));
    return e_machine == 0x28;
}

// -------------------------------------------------------------------
// Internal helpers — C++ / POSIX only, no shell
// -------------------------------------------------------------------

static bool pathExists(const std::string &path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

/// Recursively remove a directory tree (rm -rf equivalent in C++).
/// Uses lstat to avoid following symlinks — symlinks are unlinked,
/// not recursed into.
static bool rmrf(const std::string &path)
{
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return true; // already gone

    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        return ::unlink(path.c_str()) == 0;
    }

    DIR *d = ::opendir(path.c_str());
    if (!d) return false;

    struct dirent *ent;
    bool ok = true;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        std::string child = path + "/" + ent->d_name;
        if (!rmrf(child)) ok = false;
    }
    ::closedir(d);
    if (::rmdir(path.c_str()) != 0) ok = false;
    return ok;
}

/// mkdir -p equivalent in C++.
static bool mkdirp(const std::string &path)
{
    if (path.empty()) return true;
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    // Create parent first
    auto pos = path.rfind('/');
    if (pos != std::string::npos && pos > 0) {
        if (!mkdirp(path.substr(0, pos))) return false;
    }
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

static bool mkdirPForFile(const std::string &filePath)
{
    auto pos = filePath.rfind('/');
    if (pos == std::string::npos) return true;
    return mkdirp(filePath.substr(0, pos));
}

// -------------------------------------------------------------------
// fork+execvp helpers — no shell, no word-splitting
// -------------------------------------------------------------------

/// Locate a usable `tar`.  Do NOT probe by executing it: BusyBox tar (OnionOS)
/// rejects `--version` and exits non-zero, which would falsely report tar as
/// missing.  Prefer an executable absolute path; otherwise fall back to "tar"
/// and let execvp resolve it from PATH (a real tar error will surface if it
/// truly is absent).
static const char *findTar()
{
    static const char *tarPaths[] = {
        "/bin/tar", "/usr/bin/tar", "/sbin/tar", "/usr/sbin/tar"
    };
    for (auto *tp : tarPaths)
        if (::access(tp, X_OK) == 0)
            return tp;
    return "tar";
}

/// Run a command via fork+execvp, capturing stdout into a pipe.
/// Returns true on exit code 0.  stdoutOutput receives all stdout bytes.
static bool runCaptured(const char *argv[],
                        std::string &stdoutOutput,
                        std::string &error)
{
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        error = "pipe failed";
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        error = "fork failed";
        return false;
    }

    if (pid == 0) {
        // Child: redirect stdout to pipe, suppress stderr
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::execvp(argv[0], const_cast<char *const *>(argv));
        ::_exit(127);
    }

    // Parent: read stdout from pipe
    ::close(pipefd[1]);
    stdoutOutput.clear();
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        stdoutOutput.append(buf, static_cast<std::size_t>(n));
    }
    ::close(pipefd[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        error = "command failed (exit code " + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

/// Run a command via fork+execvp, no stdout capture.  stderr suppressed.
static bool runSimple(const char *argv[], std::string &error)
{
    pid_t pid = ::fork();
    if (pid < 0) { error = "fork failed"; return false; }

    if (pid == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::execvp(argv[0], const_cast<char *const *>(argv));
        ::_exit(127);
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        error = "command failed (exit code " + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

// -------------------------------------------------------------------
// Collapsed tar operations: single verbose pass extracts filenames
// and detects symlinks/hardlinks.
// -------------------------------------------------------------------

/// Structure for a parsed tar verbose line.
struct TarEntry {
    std::string filename;   // normalized path (stripped MiyooFin/ prefix)
    bool isSymlink = false;
    bool isHardlink = false;
};

/// Parse a single tar -tvzf verbose line.  Extracts the filename
/// and link type.  GNU tar format:
///   TYPE PERMS OWNER/GROUP SIZE DATE TIME NAME [-> LINK] [link to LINK]
/// Returns true if the line was parseable.
static bool parseTarVerboseLine(const std::string &line, TarEntry &out)
{
    if (line.empty()) return false;

    char type = line[0];
    if (type != '-' && type != 'l' && type != 'h' &&
        type != 'd' && type != 'b' && type != 'c' && type != 'p')
        return false;

    // Find the filename by locating the ISO datetime pattern and taking
    // everything after the following space.  GNU tar prints "YYYY-MM-DD HH:MM"
    // while BusyBox tar (OnionOS) prints "YYYY-MM-DD HH:MM:SS"; accept both.
    // This is more robust than counting variable-width fields.
    size_t nameStart = 0;
    const size_t lineLen = line.size();
    for (size_t i = 1; i + 16 < lineLen; ++i) {
        // Match: DIGIT DIGIT DIGIT DIGIT '-' DIGIT DIGIT '-' DIGIT DIGIT
        //        ' ' DIGIT DIGIT ':' DIGIT DIGIT
        if (std::isdigit(static_cast<unsigned char>(line[i]))   &&
            std::isdigit(static_cast<unsigned char>(line[i+1])) &&
            std::isdigit(static_cast<unsigned char>(line[i+2])) &&
            std::isdigit(static_cast<unsigned char>(line[i+3])) &&
            line[i+4] == '-' &&
            std::isdigit(static_cast<unsigned char>(line[i+5])) &&
            std::isdigit(static_cast<unsigned char>(line[i+6])) &&
            line[i+7] == '-' &&
            std::isdigit(static_cast<unsigned char>(line[i+8])) &&
            std::isdigit(static_cast<unsigned char>(line[i+9])) &&
            line[i+10] == ' ' &&
            std::isdigit(static_cast<unsigned char>(line[i+11])) &&
            std::isdigit(static_cast<unsigned char>(line[i+12])) &&
            line[i+13] == ':' &&
            std::isdigit(static_cast<unsigned char>(line[i+14])) &&
            std::isdigit(static_cast<unsigned char>(line[i+15]))) {
            if (line[i+16] == ' ') {
                nameStart = i + 17;
                break;
            }
            // BusyBox prints seconds too: "HH:MM:SS name".
            if (line[i+16] == ':' && i + 19 < lineLen &&
                std::isdigit(static_cast<unsigned char>(line[i+17])) &&
                std::isdigit(static_cast<unsigned char>(line[i+18])) &&
                line[i+19] == ' ') {
                nameStart = i + 20;
                break;
            }
        }
    }
    if (nameStart == 0 || nameStart >= line.size()) return false;

    // Extract the full name portion
    std::string namePortion = line.substr(nameStart);

    // Check for symlink: " -> " anywhere in the name
    if (type == 'l') {
        out.isSymlink = true;
        auto arrow = namePortion.find(" -> ");
        if (arrow != std::string::npos)
            out.filename = namePortion.substr(0, arrow);
        else
            out.filename = namePortion;
        return true;
    }

    // Check for hardlink: " link to " anywhere in the name
    if (type == 'h') {
        out.isHardlink = true;
        auto linkto = namePortion.find(" link to ");
        if (linkto != std::string::npos)
            out.filename = namePortion.substr(0, linkto);
        else
            out.filename = namePortion;
        return true;
    }

    // BusyBox tar lists a hardlink with a leading '-' (not 'h') and a
    // " -> target" suffix, so a plain entry containing " -> " is a link too.
    auto arrow = namePortion.find(" -> ");
    if (arrow != std::string::npos) {
        out.isHardlink = true;
        out.filename = namePortion.substr(0, arrow);
        return true;
    }

    out.filename = namePortion;
    return true;
}

/// Public wrapper around the static parseTarVerboseLine.
/// Delegates to parseTarVerboseLine; does not duplicate logic.
bool parseTarListingLine(const std::string &line, std::string &filename,
                         bool &isSymlink, bool &isHardlink)
{
    TarEntry entry;
    if (!parseTarVerboseLine(line, entry))
        return false;
    filename   = std::move(entry.filename);
    isSymlink  = entry.isSymlink;
    isHardlink = entry.isHardlink;
    return true;
}

/// Collapse: run tar -tvzf once, parse output for filenames AND link detection.
/// Returns the list of safe filenames (without MiyooFin/ prefix) or empty on error.
static bool tarListAndDetectLinks(const std::string &tarGzPath,
                                  std::vector<std::string> &filenames,
                                  std::string &error)
{
    const char *tarBin = findTar();
    if (!tarBin) {
        error = "tar not found in PATH";
        return false;
    }

    // argv: tar -tvzf <path>
    std::string pathArg = tarGzPath;
    const char *argv[] = { tarBin, "-tvzf", pathArg.c_str(), nullptr };

    std::string output;
    if (!runCaptured(argv, output, error)) {
        error = "tar -tvzf failed: " + error;
        return false;
    }

    // Parse each line
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim trailing newline/carriage return
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty()) continue;

        TarEntry entry;
        if (!parseTarVerboseLine(line, entry)) {
            error = "unparseable tar listing: " + line;
            return false;
        }

        // Directory entries: skip (trailing /)
        if (!entry.filename.empty() && entry.filename.back() == '/') continue;

        // Symlinks and hardlinks are forbidden (M2)
        if (entry.isSymlink || entry.isHardlink) {
            error = "archive contains " +
                    std::string(entry.isSymlink ? "symlink" : "hardlink") +
                    " entry: " + entry.filename;
            return false;
        }

        filenames.push_back(entry.filename);
    }

    return true;
}

/// Extract only the listed paths from the tar archive via fork+execvp.
static bool tarExtract(const std::string &tarGzPath,
                       const std::string &destDir,
                       const std::vector<std::string> &paths,
                       std::string &error)
{
    const char *tarBin = findTar();
    if (!tarBin) {
        error = "tar not found in PATH";
        return false;
    }

    // Build argv: tar -xzf <tarGz> -C <destDir> <paths...>
    // We allocate into a vector because paths is dynamic.
    std::vector<std::string> storage;
    storage.push_back(std::string(tarBin));
    storage.push_back("-xzf");
    storage.push_back(tarGzPath);
    storage.push_back("-C");
    storage.push_back(destDir);
    for (auto &p : paths) {
        storage.push_back("MiyooFin/" + p);
    }
    storage.push_back("2>/dev/null");

    // Convert to C-style argv (last element null-terminated, but we need
    // to NOT pass "2>/dev/null" — that's shell syntax).
    // Actually: we can't use "2>/dev/null" as an argument. Let's redo
    // without it.  Stderr suppression is handled in runSimple.
    storage.pop_back(); // remove "2>/dev/null"

    std::vector<const char *> argv;
    for (auto &s : storage) argv.push_back(s.c_str());
    argv.push_back(nullptr);

    return runSimple(argv.data(), error);
}

/// Copy a single file preserving mode bits.
/// Checks stream errors and short writes (M6).
static bool copyFile(const std::string &src, const std::string &dest)
{
    struct stat st {};
    if (::stat(src.c_str(), &st) != 0) return false;
    const auto expectedSize = static_cast<std::size_t>(st.st_size);

    int fdIn = ::open(src.c_str(), O_RDONLY);
    if (fdIn < 0) return false;

    mkdirPForFile(dest);
    int fdOut = ::open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fdOut < 0) { ::close(fdIn); return false; }

    std::size_t totalWritten = 0;
    char buf[8192];
    bool ok = true;
    while (true) {
        ssize_t n = ::read(fdIn, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        ssize_t w = ::write(fdOut, buf, static_cast<std::size_t>(n));
        if (w != n) { ok = false; break; }
        totalWritten += static_cast<std::size_t>(w);
    }

    if (ok && totalWritten != expectedSize) ok = false;

    ::close(fdIn);
    if (::fchmod(fdOut, st.st_mode) != 0) ok = false;
    ::close(fdOut);

    if (!ok) {
        ::unlink(dest.c_str());
        return false;
    }
    return true;
}

/// Atomically install a single file: write .tmp -> fsync -> chmod -> rename.
/// Opens the .tmp for writing to fsync the actual write fd (M6).
static bool atomicInstallFile(const std::string &src,
                              const std::string &dest,
                              mode_t mode)
{
    mkdirPForFile(dest);

    std::string tmpPath = dest + ".tmp";

    // Copy to .tmp
    if (!copyFile(src, tmpPath)) return false;

    // M6: fsync the write fd before rename.  Open for writing so we
    // fsync the fd that matters (not a reopened read-only fd).
    int fd = ::open(tmpPath.c_str(), O_WRONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }

    // chmod
    ::chmod(tmpPath.c_str(), mode);

    // rename over destination
    if (::rename(tmpPath.c_str(), dest.c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        return false;
    }

    return true;
}

/// fsync the parent directory of a path.
static bool fsyncDir(const std::string &filePath)
{
    auto pos = filePath.rfind('/');
    std::string dir = (pos != std::string::npos) ? filePath.substr(0, pos) : ".";
    DIR *d = ::opendir(dir.c_str());
    if (!d) return false;
    ::fsync(::dirfd(d));
    ::closedir(d);
    return true;
}

/// Get the mode for a path based on extension/name (0755 for binaries/sh, 0644 otherwise).
static mode_t installMode(const std::string &rel)
{
    // Binaries
    if (rel == "miyoofin" || rel == "miyoofin-https-bridge" ||
        rel == "miyoofin-perf-reporter")
        return 0755;

    // Shell scripts
    if (rel.size() >= 3 && rel.compare(rel.size() - 3, 3, ".sh") == 0)
        return 0755;

    return 0644;
}

// -------------------------------------------------------------------
// installUpdate
// -------------------------------------------------------------------

bool installUpdate(const std::string &appDir,
                   const std::string &tarGzPath,
                   const std::string &targetVersion,
                   std::string &error,
                   const std::atomic<bool> *cancelled,
                   const std::function<void(int percent)> &progress)
{
    error.clear();

    auto report = [&](int pct) {
        if (progress) progress(pct);
    };

    auto checkCancelled = [&]() -> bool {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };

    report(0);

    // ---------------------------------------------------------------
    // Step 1: List archive contents AND detect symlinks/hardlinks
    //         in a single tar -tvzf pass (collapsed tar passes).
    // ---------------------------------------------------------------
    std::string stagingDir = appDir + "/update-staging";
    mkdirp(stagingDir);

    std::vector<std::string> listingLines;
    if (!tarListAndDetectLinks(tarGzPath, listingLines, error)) return false;
    report(10);

    // ---------------------------------------------------------------
    // Step 2: Build install plan from the listing
    // ---------------------------------------------------------------
    // Filter out directory entries (trailing /) — tar creates dirs
    // automatically during extraction.
    std::vector<std::string> filteredLines;
    for (auto &line : listingLines) {
        if (!line.empty() && line.back() != '/')
            filteredLines.push_back(line);
    }

    std::vector<std::string> plan = buildInstallPlan(filteredLines, error);
    if (plan.empty()) return false;
    report(15);

    if (checkCancelled()) { error = "cancelled"; return false; }

    // ---------------------------------------------------------------
    // Step 3: Extract planned files to staging
    // ---------------------------------------------------------------
    rmrf(stagingDir);
    mkdirp(stagingDir);

    if (!tarExtract(tarGzPath, stagingDir, plan, error)) return false;
    report(30);

    if (checkCancelled()) { error = "cancelled"; return false; }

    // ---------------------------------------------------------------
    // Step 4: Sanity-check the miyoofin binary
    // ---------------------------------------------------------------
    {
        std::string binPath = stagingDir + "/MiyooFin/miyoofin";
        unsigned char elfHeader[20];
        std::ifstream ifs(binPath, std::ios::binary);
        if (!ifs.read(reinterpret_cast<char *>(elfHeader), sizeof(elfHeader))) {
            error = "extracted miyoofin binary is too small or unreadable";
            return false;
        }
        if (!isElfMagic(elfHeader, sizeof(elfHeader))) {
            error = "extracted miyoofin is not a valid ELF binary";
            return false;
        }
        if (!isElfArm(elfHeader, sizeof(elfHeader))) {
            error = "extracted miyoofin is not an ARM binary";
            return false;
        }
    }
    report(40);

    if (checkCancelled()) { error = "cancelled"; return false; }

    // ---------------------------------------------------------------
    // Step 5: Back up current copies of planned files
    // ---------------------------------------------------------------
    std::string backupDir = appDir + "/update-backup/" + targetVersion;
    mkdirp(backupDir);

    // (rel, backupPath) — backupPath is where the original copy lives
    std::vector<std::pair<std::string, std::string>> backedUp;

    for (auto &rel : plan) {
        std::string destPath = appDir + "/" + rel;
        if (pathExists(destPath)) {
            std::string bakPath = backupDir + "/" + rel;
            mkdirPForFile(bakPath);
            if (!copyFile(destPath, bakPath)) {
                error = "failed to backup: " + rel;
                return false;
            }
            backedUp.push_back({rel, bakPath});
        }
    }
    report(50);

    // M5: Once extraction/replacement starts, ignore cancellation.
    // This is the point of no return — the install will always
    // either complete or roll back.  We still check for errors
    // (which trigger rollback) but cancellation is ignored.

    // ---------------------------------------------------------------
    // Step 6: Install each planned file — miyoofin LAST
    // ---------------------------------------------------------------
    // buildInstallPlan already guarantees miyoofin is last.
    std::vector<std::string> installed; // track for rollback

    for (auto &rel : plan) {
        std::string srcPath = stagingDir + "/MiyooFin/" + rel;
        std::string destPath = appDir + "/" + rel;
        mode_t mode = installMode(rel);

        if (!pathExists(srcPath)) {
            error = "expected file missing from staging: " + rel;
            // Rollback installed files in reverse order
            for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
                for (auto &bak : backedUp) {
                    if (bak.first == *it) {
                        copyFile(bak.second, appDir + "/" + bak.first);
                        break;
                    }
                }
            }
            return false;
        }

        if (!atomicInstallFile(srcPath, destPath, mode)) {
            error = "failed to install: " + rel;
            // Rollback in reverse order
            for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
                for (auto &bak : backedUp) {
                    if (bak.first == *it) {
                        copyFile(bak.second, appDir + "/" + bak.first);
                        break;
                    }
                }
            }
            return false;
        }

        fsyncDir(destPath);
        installed.push_back(rel);
        report(50 + (40 * static_cast<int>(installed.size())) /
               static_cast<int>(plan.size()));
        // NOTE: cancellation is intentionally NOT checked here (M5).
    }

    report(90);

    // ---------------------------------------------------------------
    // Step 7: Write update-applied.json atomically
    // (non-fatal: all files are already installed)
    // ---------------------------------------------------------------
    {
        std::string jsonPath = appDir + "/update-applied.json";
        std::string tmpJson = jsonPath + ".tmp";
        std::ofstream ofs(tmpJson);
        if (ofs) {
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            char timeBuf[64];
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ",
                          std::gmtime(&tt));

            ofs << "{\n"
                << "  \"from\": \"" << VERSION_STR << "\",\n"
                << "  \"to\": \"" << targetVersion << "\",\n"
                << "  \"at\": \"" << timeBuf << "\"\n"
                << "}\n";
            ofs.close();

            // fsync + rename
            int fd = ::open(tmpJson.c_str(), O_RDONLY);
            if (fd >= 0) { ::fsync(fd); ::close(fd); }

            if (::rename(tmpJson.c_str(), jsonPath.c_str()) != 0) {
                std::fprintf(stderr,
                    "[update] warning: failed to atomically write "
                    "update-applied.json\n");
            }
            fsyncDir(jsonPath);
        } else {
            std::fprintf(stderr,
                "[update] warning: failed to create update-applied.json\n");
        }
    }

    // ---------------------------------------------------------------
    // Cleanup: remove staging dir (keep backup)
    // ---------------------------------------------------------------
    rmrf(stagingDir);

    report(100);
    return true;
}

} // namespace miyoofin
