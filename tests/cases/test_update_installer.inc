// -------------------------------------------------------------------
// tests/cases/test_update_installer.inc — HttpClient::downloadToFile
// and installUpdate unit tests
// -------------------------------------------------------------------
#include "../../src/net/HttpClient.hpp"
#include "../../src/update/UpdateInstaller.hpp"
#include "../../src/update/UpdateInstallPlan.hpp"
#include "../../src/update/AppDir.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace miyoofin;

// -------------------------------------------------------------------
// Test-local filesystem helpers (mirrors installer internals)
// -------------------------------------------------------------------

static bool testPathExists(const std::string &path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

static bool testIsDir(const std::string &path)
{
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool testRmrf(const std::string &path)
{
    std::string cmd = "rm -rf " + path;
    return ::system(cmd.c_str()) == 0;
}

static bool testMkdirp(const std::string &path)
{
    std::string cmd = "mkdir -p " + path;
    return ::system(cmd.c_str()) == 0;
}

// ===================================================================
// Local POSIX HTTP server helper
// ===================================================================

/// Minimal HTTP server that runs in a background thread.
/// Serves a fixed body and supports Range + redirect responses.
class LocalHttpServer {
public:
    LocalHttpServer() : m_fd(-1), m_port(0) {}
    ~LocalHttpServer() { stop(); }

    /// Start listening on 127.0.0.1 with an ephemeral port.
    bool start() {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0) return false;

        int opt = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral

        if (::bind(m_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(m_fd, (sockaddr *)&addr, &len);
        m_port = ntohs(addr.sin_port);

        if (::listen(m_fd, 1) < 0) {
            ::close(m_fd);
            m_fd = -1;
            return false;
        }

        m_running = true;
        m_thread = std::thread([this]() { run(); });
        return true;
    }

    void stop() {
        m_running = false;
        if (m_fd >= 0) { ::shutdown(m_fd, SHUT_RDWR); ::close(m_fd); m_fd = -1; }
        if (m_thread.joinable()) m_thread.join();
    }

    uint16_t port() const { return m_port; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(m_port) + "/test";
    }

    /// Set the response body, HTTP code, and optional Content-Length.
    void setResponse(const std::string &body, long code = 200,
                     bool includeContentLength = true) {
        m_responseBody = body;
        m_responseCode = code;
        m_includeContentLength = includeContentLength;
    }

    /// Set a redirect response to the given URL.
    void setRedirect(long code, const std::string &location) {
        m_responseCode = code;
        m_redirectLocation = location;
        m_isRedirect = true;
    }

    /// Enable Range support: when Range header is present, serve partial content.
    void enableRange(const std::string &fullBody) {
        m_fullBody = fullBody;
        m_supportRange = true;
    }

    void setStall(bool s) { m_stall = s; }

    /// After serving one request, close the listener (for 404 / single-shot).
    void setSingleShot(bool s) { m_singleShot = s; }

    /// Get the last Range header received (for resume testing).
    std::string lastRangeHeader() const { return m_lastRange; }

private:
    void run() {
        while (m_running) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = ::accept(m_fd, (sockaddr *)&clientAddr, &clientLen);
            if (clientFd < 0) break;

            // Read request headers
            char buf[4096];
            std::string request;
            ssize_t n;
            while ((n = ::recv(clientFd, buf, sizeof(buf) - 1, 0)) > 0) {
                buf[n] = '\0';
                request += buf;
                if (request.find("\r\n\r\n") != std::string::npos) break;
            }

            // Parse Range header
            m_lastRange.clear();
            {
                auto rangePos = request.find("Range: ");
                if (rangePos != std::string::npos) {
                    auto end = request.find("\r\n", rangePos);
                    m_lastRange = request.substr(rangePos + 7,
                                                 end - rangePos - 7);
                }
            }

            // Build response
            std::string response;
            if (m_stall) {
                // Stall: sleep in small increments, checking m_running
                for (int i = 0; i < 600 && m_running; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                ::close(clientFd);
                return;
            }

            if (m_isRedirect) {
                response = "HTTP/1.1 " + std::to_string(m_responseCode) +
                           " Redirect\r\n"
                           "Location: " + m_redirectLocation + "\r\n"
                           "Content-Length: 0\r\n\r\n";
                ::send(clientFd, response.c_str(), response.size(), 0);
                ::close(clientFd);
                if (m_singleShot) return;
                continue;
            }

            std::string body;
            if (m_supportRange) {
                // Check if client sent Range
                if (!m_lastRange.empty()) {
                    // Parse "bytes=10-"
                    auto eqPos = m_lastRange.find('=');
                    if (eqPos != std::string::npos) {
                        std::string val = m_lastRange.substr(eqPos + 1);
                        auto dashPos = val.find('-');
                        if (dashPos != std::string::npos) {
                            std::string startStr = val.substr(0, dashPos);
                            long start = std::strtol(startStr.c_str(), nullptr, 10);
                            if (start > 0 && start < (long)m_fullBody.size()) {
                                body = m_fullBody.substr(start);
                                response = "HTTP/1.1 206 Partial Content\r\n"
                                           "Content-Type: application/octet-stream\r\n"
                                           "Content-Range: bytes " +
                                           std::to_string(start) + "-" +
                                           std::to_string(m_fullBody.size() - 1) +
                                           "/" +
                                           std::to_string(m_fullBody.size()) +
                                           "\r\n"
                                           "Content-Length: " +
                                           std::to_string(body.size()) +
                                           "\r\n\r\n" + body;
                                ::send(clientFd, response.c_str(), response.size(), 0);
                                ::close(clientFd);
                                if (m_singleShot) return;
                                continue;
                            }
                            // Range starts at or past end — 416.
                            if (start >= (long)m_fullBody.size()) {
                                response = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                           "Content-Range: bytes */" +
                                           std::to_string(m_fullBody.size()) +
                                           "\r\n"
                                           "Content-Length: 0\r\n\r\n";
                                ::send(clientFd, response.c_str(), response.size(), 0);
                                ::close(clientFd);
                                if (m_singleShot) return;
                                continue;
                            }
                        }
                    }
                }
                body = m_fullBody;
            } else {
                body = m_responseBody;
            }

            if (m_responseCode == 200 || m_responseCode == 206) {
                response = "HTTP/1.1 " + std::to_string(m_responseCode) +
                           " OK\r\n"
                           "Content-Type: application/octet-stream\r\n";
                if (m_includeContentLength) {
                    response += "Content-Length: " + std::to_string(body.size()) +
                                "\r\n";
                }
                response += "\r\n" + body;
            } else {
                response = "HTTP/1.1 " + std::to_string(m_responseCode) +
                           " Error\r\n"
                           "Content-Length: 0\r\n\r\n";
            }

            ::send(clientFd, response.c_str(), response.size(), 0);
            ::close(clientFd);
            if (m_singleShot) return;
        }
    }

    int m_fd;
    uint16_t m_port;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::string m_responseBody;
    std::string m_fullBody;
    long m_responseCode = 200;
    bool m_includeContentLength = true;
    bool m_supportRange = false;
    bool m_stall = false;
    bool m_singleShot = false;
    bool m_isRedirect = false;
    std::string m_redirectLocation;
    std::string m_lastRange;
};

// ===================================================================
// downloadToFile tests
// ===================================================================

static void testDownloadToFileBasic()
{
    std::printf("[test] downloadToFile basic\n");

    LocalHttpServer server;
    std::string body = "Hello from the local server! This is test data.";
    server.setResponse(body, 200);
    CHECK(server.start());

    HttpClient client;
    std::string dest = "/tmp/miyoofin_test_dl_basic.bin";
    std::string error;
    std::uint64_t bytes = 0;
    bool ok = client.downloadToFile(server.url(), {}, dest, error, &bytes);

    CHECK(ok);
    CHECK(error.empty());
    CHECK(bytes == body.size());

    // Verify file content
    std::string content = readTestBytes(dest);
    CHECK(content == body);

    ::unlink(dest.c_str());
    server.stop();
    std::printf("[test] downloadToFile basic OK\n");
}

static void testDownloadToFileProgress()
{
    std::printf("[test] downloadToFile progress\n");

    LocalHttpServer server;
    // 64 KB body so we get multiple progress callbacks
    std::string body(64 * 1024, 'X');
    server.setResponse(body, 200);
    CHECK(server.start());

    HttpClient client;
    std::string dest = "/tmp/miyoofin_test_dl_progress.bin";
    std::string error;

    int progressCount = 0;
    std::uint64_t maxReceived = 0;
    bool ok = client.downloadToFile(
        server.url(), {}, dest, error, nullptr,
        [&](std::uint64_t received, std::uint64_t) {
            ++progressCount;
            if (received > maxReceived) maxReceived = received;
        });

    CHECK(ok);
    // For very fast local transfers, curl may not invoke the progress callback
    // at all — that's acceptable. When it does, values should be sane.
    if (progressCount > 0) {
        CHECK(maxReceived <= body.size());
    }

    ::unlink(dest.c_str());
    server.stop();
    std::printf("[test] downloadToFile progress OK\n");
}

static void testDownloadToFileCancel()
{
    std::printf("[test] downloadToFile cancel\n");

    LocalHttpServer server;
    server.setStall(true); // server never responds
    CHECK(server.start());

    HttpClient client;
    std::string dest = "/tmp/miyoofin_test_dl_cancel.bin";
    std::string error;
    std::atomic<bool> cancelled{false};

    // Start download in a thread, cancel after 100ms
    auto fut = std::async(std::launch::async, [&]() {
        return client.downloadToFile(server.url(), {}, dest, error,
                                     nullptr, {}, &cancelled, 10, 5);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancelled = true;

    bool ok = fut.get();
    CHECK(!ok);
    // Partial file should exist (kept on cancel)
    CHECK(testPathExists(dest));

    ::unlink(dest.c_str());
    server.stop();
    std::printf("[test] downloadToFile cancel OK\n");
}

static void testDownloadToFileResume()
{
    std::printf("[test] downloadToFile resume\n");

    LocalHttpServer server;
    std::string fullBody = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    server.enableRange(fullBody);
    CHECK(server.start());

    // Pre-create a partial file
    std::string dest = "/tmp/miyoofin_test_dl_resume.bin";
    {
        std::ofstream ofs(dest, std::ios::binary);
        ofs << fullBody.substr(0, 10); // first 10 bytes
    }

    HttpClient client;
    std::string error;
    std::uint64_t bytes = 0;
    bool ok = client.downloadToFile(server.url(), {}, dest, error, &bytes,
                                    {}, nullptr, 300, 15, 10);

    CHECK(ok);
    CHECK(bytes == fullBody.size() - 10); // received from byte 10 onward
    CHECK(server.lastRangeHeader().find("bytes=10-") != std::string::npos);

    // Verify file content matches full body
    std::string content = readTestBytes(dest);
    CHECK(content == fullBody);

    ::unlink(dest.c_str());
    server.stop();
    std::printf("[test] downloadToFile resume OK\n");
}

static void testDownloadToFileRedirect()
{
    std::printf("[test] downloadToFile redirect\n");

    // Set up a second server as the redirect target
    LocalHttpServer target;
    std::string body = "redirected content";
    target.setResponse(body, 200);
    CHECK(target.start());

    LocalHttpServer server;
    server.setRedirect(302, target.url());
    CHECK(server.start());

    HttpClient client;
    std::string dest = "/tmp/miyoofin_test_dl_redirect.bin";
    std::string error;
    std::uint64_t bytes = 0;
    bool ok = client.downloadToFile(server.url(), {}, dest, error, &bytes);

    CHECK(ok);
    CHECK(bytes == body.size());

    std::string content = readTestBytes(dest);
    CHECK(content == body);

    ::unlink(dest.c_str());
    server.stop();
    target.stop();
    std::printf("[test] downloadToFile redirect OK\n");
}

static void testDownloadToFile404()
{
    std::printf("[test] downloadToFile 404\n");

    LocalHttpServer server;
    server.setResponse("", 404);
    CHECK(server.start());

    HttpClient client;
    std::string dest = "/tmp/miyoofin_test_dl_404.bin";
    std::string error;
    bool ok = client.downloadToFile(server.url(), {}, dest, error);

    CHECK(!ok);
    CHECK(error.find("404") != std::string::npos);

    ::unlink(dest.c_str());
    server.stop();
    std::printf("[test] downloadToFile 404 OK\n");
}

// ===================================================================
// ELF / ARM helper tests
// ===================================================================

static void testIsElfMagic()
{
    std::printf("[test] isElfMagic\n");

    unsigned char good[] = {0x7f, 'E', 'L', 'F', 0, 0, 0, 0};
    CHECK(isElfMagic(good, sizeof(good)));

    unsigned char bad[] = {'M', 'Z', 0, 0};
    CHECK(!isElfMagic(bad, sizeof(bad)));

    unsigned char tooShort[] = {0x7f, 'E'};
    CHECK(!isElfMagic(tooShort, sizeof(tooShort)));

    std::printf("[test] isElfMagic OK\n");
}

static void testIsElfArm()
{
    std::printf("[test] isElfArm\n");

    // Build a minimal ELF header with e_machine=0x28 (ARM) at offset 18
    unsigned char armElf[20] = {};
    armElf[0] = 0x7f; armElf[1] = 'E'; armElf[2] = 'L'; armElf[3] = 'F';
    armElf[18] = 0x28; armElf[19] = 0x00; // e_machine = 0x28
    CHECK(isElfArm(armElf, sizeof(armElf)));

    // x86 e_machine = 0x03
    unsigned char x86Elf[20] = {};
    x86Elf[0] = 0x7f; x86Elf[1] = 'E'; x86Elf[2] = 'L'; x86Elf[3] = 'F';
    x86Elf[18] = 0x03; x86Elf[19] = 0x00;
    CHECK(!isElfArm(x86Elf, sizeof(x86Elf)));

    // Not ELF at all
    unsigned char notElf[20] = {};
    CHECK(!isElfArm(notElf, sizeof(notElf)));

    std::printf("[test] isElfArm OK\n");
}

// ===================================================================
// installUpdate tests
// ===================================================================

/// Create a minimal ARM ELF binary stub (just enough to pass isElfArm).
static void createElfStub(const std::string &path)
{
    unsigned char elf[20] = {};
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 1;    // 32-bit
    elf[5] = 1;    // little-endian
    elf[6] = 1;    // EV_CURRENT
    elf[18] = 0x28; elf[19] = 0x00; // e_machine = ARM
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char *>(elf), sizeof(elf));
}

/// Create a fake MiyooFin app directory with user state files.
static std::string createFakeAppDir()
{
    std::string dir = "/tmp/miyoofin_install_test_" + std::to_string(::getpid());
    testMkdirp(dir + "/lib");

    // Current miyoofin binary
    createElfStub(dir + "/miyoofin");

    // launch.sh
    {
        std::ofstream ofs(dir + "/launch.sh");
        ofs << "#!/bin/sh\necho old\n";
    }
    ::chmod((dir + "/launch.sh").c_str(), 0755);

    // A library
    {
        std::ofstream ofs(dir + "/lib/libfoo.so.1");
        ofs << "old library content";
    }

    // User state files that must NOT be touched
    {
        std::ofstream ofs(dir + "/session.txt");
        ofs << "session data";
    }
    testMkdirp(dir + "/downloads");
    {
        std::ofstream ofs(dir + "/downloads/movie.mp4");
        ofs << "downloaded movie";
    }

    return dir;
}

/// Create a real .tar.gz containing a valid MiyooFin archive.
static std::string createFakeArchive(const std::string &appDir)
{
    std::string staging = appDir + "/archive-staging";
    testMkdirp(staging + "/MiyooFin/lib");

    // miyoofin binary (ARM ELF)
    createElfStub(staging + "/MiyooFin/miyoofin");

    // launch.sh
    {
        std::ofstream ofs(staging + "/MiyooFin/launch.sh");
        ofs << "#!/bin/sh\necho new\n";
    }

    // lib/libfoo.so.1
    {
        std::ofstream ofs(staging + "/MiyooFin/lib/libfoo.so.1");
        ofs << "new library content";
    }

    // Create tar.gz
    std::string tarPath = appDir + "/test-update.tar.gz";
    std::string cmd = "cd " + staging + " && tar czf " + tarPath + " MiyooFin/";
    ::system(cmd.c_str());

    testRmrf(staging);
    return tarPath;
}

/// Create an archive with an unsafe entry (path traversal).
static std::string createUnsafeArchive(const std::string &appDir)
{
    std::string staging = appDir + "/archive-staging-unsafe";
    testMkdirp(staging + "/MiyooFin");
    // Note: creating the traversal path is tricky in a controlled way.
    // Instead, create the archive with the unsafe entry using a script.
    createElfStub(staging + "/MiyooFin/miyoofin");

    std::string tarPath = appDir + "/test-unsafe.tar.gz";
    // Create a tar with a malicious entry via a script that creates the
    // directory structure and then archives it.
    std::string script = "cd " + staging + " && "
        "mkdir -p evil_dir && echo evil > evil_dir/payload.txt && "
        "tar czf " + tarPath +
        " --transform='s|evil_dir|MiyooFin/../../evil|' "
        "MiyooFin/miyoofin evil_dir/payload.txt 2>/dev/null || "
        // Fallback: just create a listing that buildInstallPlan will reject
        "tar czf " + tarPath + " MiyooFin/miyoofin";
    ::system(script.c_str());

    testRmrf(staging);
    return tarPath;
}

static void testInstallUpdateBasic()
{
    std::printf("[test] installUpdate basic\n");

    std::string appDir = createFakeAppDir();
    std::string tarPath = createFakeArchive(appDir);
    std::string error;

    // Save original file content for comparison
    std::string origSession = readTestBytes(appDir + "/session.txt");
    std::string origDownload = readTestBytes(appDir + "/downloads/movie.mp4");

    bool ok = installUpdate(appDir, tarPath, "99.0.0", error);
    if (!ok) std::printf("  installUpdate error: %s\n", error.c_str());
    CHECK(ok);
    if (ok) CHECK(error.empty());

    // User files must be byte-identical
    CHECK(readTestBytes(appDir + "/session.txt") == origSession);
    CHECK(readTestBytes(appDir + "/downloads/movie.mp4") == origDownload);

    // Planned files should be replaced
    CHECK(readTestBytes(appDir + "/launch.sh").find("new") != std::string::npos);
    CHECK(readTestBytes(appDir + "/lib/libfoo.so.1") == "new library content");

    // miyoofin should be an ELF binary (the stub we created)
    std::string miyoofinContent = readTestBytes(appDir + "/miyoofin");
    CHECK(miyoofinContent.size() >= 20);
    CHECK(miyoofinContent[0] == 0x7f);
    CHECK(miyoofinContent[1] == 'E');

    // Check file permissions
    struct stat st;
    ::stat((appDir + "/miyoofin").c_str(), &st);
    CHECK((st.st_mode & 0777) == 0755);
    ::stat((appDir + "/launch.sh").c_str(), &st);
    CHECK((st.st_mode & 0777) == 0755);
    ::stat((appDir + "/lib/libfoo.so.1").c_str(), &st);
    CHECK((st.st_mode & 0777) == 0644);

    // Backup should exist
    CHECK(testIsDir(appDir + "/update-backup/99.0.0"));
    CHECK(testPathExists(appDir + "/update-backup/99.0.0/miyoofin"));
    CHECK(testPathExists(appDir + "/update-backup/99.0.0/launch.sh"));

    // update-applied.json should exist
    CHECK(testPathExists(appDir + "/update-applied.json"));
    {
        std::string json = readTestBytes(appDir + "/update-applied.json");
        CHECK(json.find("\"to\": \"99.0.0\"") != std::string::npos);
        CHECK(json.find("\"from\": \"") != std::string::npos);
    }

    testRmrf(appDir);
    ::unlink(tarPath.c_str());
    std::printf("[test] installUpdate basic OK\n");
}

static void testInstallUpdateUnsafeArchive()
{
    std::printf("[test] installUpdate unsafe archive\n");

    std::string appDir = createFakeAppDir();
    std::string tarPath = createUnsafeArchive(appDir);
    std::string error;

    bool ok = installUpdate(appDir, tarPath, "1.0.0", error);
    CHECK(!ok);
    CHECK(!error.empty());

    testRmrf(appDir);
    ::unlink(tarPath.c_str());
    std::printf("[test] installUpdate unsafe archive OK\n");
}

static void testInstallUpdateCancel()
{
    std::printf("[test] installUpdate cancel\n");

    std::string appDir = createFakeAppDir();
    std::string tarPath = createFakeArchive(appDir);
    std::string error;
    std::atomic<bool> cancelled{false};

    // Cancel immediately
    cancelled = true;
    bool ok = installUpdate(appDir, tarPath, "1.0.0", error, &cancelled);
    CHECK(!ok);
    CHECK(error == "cancelled");

    testRmrf(appDir);
    ::unlink(tarPath.c_str());
    std::printf("[test] installUpdate cancel OK\n");
}

static void testInstallUpdateProgress()
{
    std::printf("[test] installUpdate progress\n");

    std::string appDir = createFakeAppDir();
    std::string tarPath = createFakeArchive(appDir);
    std::string error;

    int lastPct = -1;
    bool ok = installUpdate(appDir, tarPath, "1.0.0", error, nullptr,
                            [&](int pct) { lastPct = pct; });
    CHECK(ok);
    CHECK(lastPct == 100);

    testRmrf(appDir);
    ::unlink(tarPath.c_str());
    std::printf("[test] installUpdate progress OK\n");
}

// -------------------------------------------------------------------
// New tests for B1 rollback, M2 hardlinks, M4 416, M3 non-200
// -------------------------------------------------------------------

/// Verify that the backup created during a successful install is
/// byte-identical to the original files. This gives confidence that
/// rollback (B1 fix) would restore correctly.
static void testInstallUpdateBackupByteIdentical()
{
    std::printf("[test] installUpdate backup byte-identical\n");

    std::string appDir = createFakeAppDir();
    std::string tarPath = createFakeArchive(appDir);
    std::string error;

    // Save original file content for comparison
    std::string origMiyoofin = readTestBytes(appDir + "/miyoofin");
    std::string origLaunch = readTestBytes(appDir + "/launch.sh");
    std::string origLib = readTestBytes(appDir + "/lib/libfoo.so.1");

    bool ok = installUpdate(appDir, tarPath, "99.0.0", error);
    CHECK(ok);
    if (!ok) std::printf("  installUpdate error: %s\n", error.c_str());

    // Verify backup matches originals byte-for-byte
    CHECK(readTestBytes(appDir + "/update-backup/99.0.0/miyoofin") == origMiyoofin);
    CHECK(readTestBytes(appDir + "/update-backup/99.0.0/launch.sh") == origLaunch);
    CHECK(readTestBytes(appDir + "/update-backup/99.0.0/lib/libfoo.so.1") == origLib);

    testRmrf(appDir);
    ::unlink(tarPath.c_str());
    std::printf("[test] installUpdate backup byte-identical OK\n");
}

/// Create a tar.gz archive that contains a hardlink entry.
static std::string createHardlinkArchive(const std::string &appDir)
{
    std::string staging = appDir + "/archive-staging-hl";
    testMkdirp(staging + "/MiyooFin/lib");

    // Create the miyoofin binary (ARM ELF)
    createElfStub(staging + "/MiyooFin/miyoofin");

    // Create launch.sh
    {
        std::ofstream ofs(staging + "/MiyooFin/launch.sh");
        ofs << "#!/bin/sh\necho ok\n";
    }
    ::chmod((staging + "/MiyooFin/launch.sh").c_str(), 0755);

    // Create a regular file and a hardlink to it
    {
        std::ofstream ofs(staging + "/MiyooFin/lib/libfoo.so.1");
        ofs << "library content";
    }
    ::link((staging + "/MiyooFin/lib/libfoo.so.1").c_str(),
           (staging + "/MiyooFin/lib/libbar.so.1").c_str());

    std::string tarPath = appDir + "/test-hardlink.tar.gz";
    // Use --no-recursion to ensure hardlinks are recorded properly
    std::string cmd = "cd " + staging + " && tar czf " + tarPath +
                      " --no-recursion MiyooFin/miyoofin MiyooFin/launch.sh"
                      " MiyooFin/lib/libfoo.so.1 MiyooFin/lib/libbar.so.1 2>/dev/null";
    ::system(cmd.c_str());

    testRmrf(staging);
    return tarPath;
}

/// Verify that hardlink entries in an archive are rejected (M2).
static void testInstallUpdateHardlinkRejected()
{
    std::printf("[test] installUpdate hardlink rejected\n");

    std::string appDir = createFakeAppDir();
    std::string tarPath = createHardlinkArchive(appDir);
    std::string error;

    bool ok = installUpdate(appDir, tarPath, "1.0.0", error);
    CHECK(!ok);
    CHECK(error.find("hardlink") != std::string::npos);

    testRmrf(appDir);
    ::unlink(tarPath.c_str());
    std::printf("[test] installUpdate hardlink rejected OK\n");
}

/// Verify that a 404 response does not corrupt a .part file (M3).
static void testDownloadToFile404NoCorruption()
{
    std::printf("[test] downloadToFile 404 does not corrupt .part\n");

    LocalHttpServer server;
    // First response: 404
    server.setResponse("not found", 404);
    server.setSingleShot(true);
    CHECK(server.start());

    std::string dest = "/tmp/miyoofin_test_dl_404_nocorrupt.bin";
    // Pre-create a .part file with valid content
    {
        std::ofstream ofs(dest, std::ios::binary);
        ofs << "valid partial content here";
    }
    std::string before = readTestBytes(dest);

    HttpClient client;
    std::string error;
    bool ok = client.downloadToFile(server.url(), {}, dest, error);

    // Should fail
    CHECK(!ok);
    CHECK(error.find("404") != std::string::npos);

    // The .part file should have been deleted (M3)
    CHECK(!testPathExists(dest));

    server.stop();
    std::printf("[test] downloadToFile 404 does not corrupt .part OK\n");
}

/// Verify that a complete .part file is handled gracefully (M4).
/// The stub server emits a real 416 when Range start >= body size.
/// downloadToFile must leave the .part intact and return success.
static void testDownloadToFileCompletePartSkipDownload()
{
    std::printf("[test] downloadToFile complete .part → skip download\n");

    LocalHttpServer server;
    std::string fullBody = "COMPLETE_FILE_CONTENT_FOR_TESTING_12345";
    server.enableRange(fullBody);
    CHECK(server.start());

    // Pre-create a .part file that matches the full body
    std::string dest = "/tmp/miyoofin_test_dl_complete_part.bin";
    {
        std::ofstream ofs(dest, std::ios::binary);
        ofs << fullBody;
    }

    HttpClient client;
    std::string error;
    std::uint64_t bytes = 0;

    // Request with resumeFrom = full size — stub returns real 416
    bool ok = client.downloadToFile(
        server.url(), {}, dest, error, &bytes,
        {}, nullptr, 300, 15, fullBody.size());

    // M4: 416 should be treated as "already complete"
    CHECK(ok);
    CHECK(testPathExists(dest));
    std::string content = readTestBytes(dest);
    CHECK(content == fullBody);
    CHECK(bytes == fullBody.size());

    ::unlink(dest.c_str());
    server.stop();
    std::printf("[test] downloadToFile complete .part → skip download OK\n");
}

/// Verify that a stale resumeFrom is reset to 0 after the .part is deleted.
/// Scenario: first attempt gets 404 (deletes .part), second attempt must
/// start from 0 with a full download, not append to an empty file.
static void testDownloadToFileStaleResumeReset()
{
    std::printf("[test] downloadToFile stale resumeFrom reset\n");

    std::string fullBody = "FULL_DOWNLOAD_BODY_FOR_RESUME_TEST_9876";
    std::string dest = "/tmp/miyoofin_test_dl_stale_resume.bin";

    // Pre-create a .part file with some old content
    {
        std::ofstream ofs(dest, std::ios::binary);
        ofs << "old_stale_content";
    }

    // Server 1: returns 404 (will delete the .part)
    LocalHttpServer server1;
    server1.setResponse("not found", 404);
    server1.setSingleShot(true);
    CHECK(server1.start());

    HttpClient client;
    std::string error;

    // First attempt with resumeFrom=100 — gets 404, .part deleted
    bool ok1 = client.downloadToFile(
        server1.url(), {}, dest, error, nullptr, {}, nullptr, 300, 15, 100);
    CHECK(!ok1);
    CHECK(error.find("404") != std::string::npos);
    CHECK(!testPathExists(dest));
    server1.stop();

    // Server 2: returns full body
    LocalHttpServer server2;
    server2.setResponse(fullBody, 200);
    CHECK(server2.start());

    // Second attempt — must start from 0 (no Range header)
    std::uint64_t bytes = 0;
    bool ok2 = client.downloadToFile(
        server2.url(), {}, dest, error, &bytes, {}, nullptr, 300, 15, 0);
    CHECK(ok2);
    CHECK(testPathExists(dest));
    std::string content = readTestBytes(dest);
    CHECK(content == fullBody);
    CHECK(bytes == fullBody.size());
    // Server should NOT have received a Range header
    CHECK(server2.lastRangeHeader().empty());

    ::unlink(dest.c_str());
    server2.stop();
    std::printf("[test] downloadToFile stale resumeFrom reset OK\n");
}

/// Verify that a forced mid-install failure triggers rollback that
/// restores every already-installed file from update-backup byte-identical.
static void testInstallUpdateRollbackOnFailure()
{
    std::printf("[test] installUpdate rollback on mid-install failure\n");

    // Set up a minimal appDir with existing files to be backed up
    std::string appDir = "/tmp/miyoofin_test_rollback_" +
                         std::to_string(std::time(nullptr));
    testMkdirp(appDir + "/lib");

    // Create existing files that should be backed up and restored
    std::string origContentA = "ORIGINAL_CONTENT_ALPHA_12345";
    std::string origContentB = "ORIGINAL_CONTENT_BETA_67890";
    {
        std::ofstream ofs(appDir + "/lib/test_a.so", std::ios::binary);
        ofs << origContentA;
    }
    {
        std::ofstream ofs(appDir + "/lib/test_b.so", std::ios::binary);
        ofs << origContentB;
    }

    // Build a tar archive with MiyooFin/lib/test_a.so and
    // MiyooFin/lib/test_c.so (test_c will be the one that fails).
    std::string tarPath = "/tmp/miyoofin_test_rollback.tar.gz";
    {
        // Create staging dir with two files
        std::string staging = "/tmp/miyoofin_test_rollback_staging";
        testMkdirp(staging + "/MiyooFin/lib");
        {
            std::ofstream ofs(staging + "/MiyooFin/lib/test_a.so",
                              std::ios::binary);
            ofs << "NEW_CONTENT_ALPHA";
        }
        {
            std::ofstream ofs(staging + "/MiyooFin/lib/test_c.so",
                              std::ios::binary);
            ofs << "NEW_CONTENT_GAMMA";
        }

        std::string cmd = "tar -czf " + tarPath + " -C " + staging +
                          " MiyooFin/";
        CHECK(::system(cmd.c_str()) == 0);
        testRmrf(staging);
    }

    // Make test_c.so's destination unwritable by pre-creating it as a
    // directory — atomicInstallFile will fail on the rename.
    testMkdirp(appDir + "/lib/test_c.so");

    std::string error;
    bool ok = installUpdate(appDir, tarPath, "9.9.9", error);

    // Install should fail because test_c.so can't be installed
    CHECK(!ok);

    // Rollback must have restored original files byte-identical
    CHECK(testPathExists(appDir + "/lib/test_a.so"));
    CHECK(testPathExists(appDir + "/lib/test_b.so"));
    CHECK(readTestBytes(appDir + "/lib/test_a.so") == origContentA);
    CHECK(readTestBytes(appDir + "/lib/test_b.so") == origContentB);

    testRmrf(appDir);
    ::unlink(tarPath.c_str());
    std::printf("[test] installUpdate rollback on mid-install failure OK\n");
}
