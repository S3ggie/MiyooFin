#include "UpdateManager.hpp"
#include "UpdateManifest.hpp"
#include "UpdateVersion.hpp"
#include "UpdateInstaller.hpp"
#include "Sha256.hpp"
#include "AppDir.hpp"
#include "../net/HttpClient.hpp"
#include "miyoofin/version.hpp"

#include <cstdio>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace miyoofin {

// -------------------------------------------------------------------
// updateStatusText
// -------------------------------------------------------------------

std::string updateStatusText(const UpdateSnapshot &snap)
{
    switch (snap.stage) {
    case UpdateStage::Idle:
        return "Check for updates";
    case UpdateStage::Checking:
        return "Checking...";
    case UpdateStage::UpToDate:
        return "Up to date (v" + snap.availableVersion + ")";
    case UpdateStage::Available:
        return "v" + snap.availableVersion + " available - A to install";
    case UpdateStage::Downloading:
        return "Downloading " + std::to_string(snap.percent) + "%";
    case UpdateStage::Verifying:
        return "Verifying...";
    case UpdateStage::Installing:
        return "Installing... do not power off";
    case UpdateStage::ReadyToRestart:
        return "Updated to v" + snap.availableVersion + " - A to exit";
    case UpdateStage::Error:
        return "Update failed: " + snap.error + " - A to retry";
    }
    return "Check for updates";
}

// -------------------------------------------------------------------
// updateCheckErrorMessage
// -------------------------------------------------------------------

std::string updateCheckErrorMessage(long httpCode,
                                    const std::string &transportError)
{
    if (httpCode == 404)
        return "no published release yet";
    if (httpCode > 0)
        return "check failed (HTTP " + std::to_string(httpCode) + ")";
    if (!transportError.empty())
        return "network error: " + transportError;
    return "network unreachable";
}

// -------------------------------------------------------------------
// Dev-override helpers (pure, testable without I/O)
// -------------------------------------------------------------------

ManifestSource resolveManifestSource(const std::string &envValue,
                                     const std::string &devFileContents,
                                     const std::string &defaultUrl)
{
    // Priority 1: MIYOOFIN_UPDATE_URL env var (non-empty)
    if (!envValue.empty())
        return {envValue, true};

    // Priority 2: first non-empty, non-comment line from update-dev-url.txt
    if (!devFileContents.empty()) {
        std::istringstream ss(devFileContents);
        std::string line;
        while (std::getline(ss, line)) {
            // Trim leading/trailing whitespace
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(start, end - start + 1);
            if (trimmed.empty() || trimmed[0] == '#') continue;
            return {trimmed, true};
        }
    }

    // Priority 3: production URL
    return {defaultUrl, false};
}

bool isLocalAsset(const std::string &url)
{
    if (url.empty()) return false;
    if (url.compare(0, 7, "file://") == 0) return true;
    // Plain absolute path
    return url[0] == '/';
}

std::string localAssetPath(const std::string &url)
{
    if (url.compare(0, 7, "file://") == 0)
        return url.substr(7);
    return url;
}

// -------------------------------------------------------------------
// UpdateManager
// -------------------------------------------------------------------

static const char *MANIFEST_URL =
    "https://github.com/S3ggie/MiyooFin/releases/latest/download/manifest.json";

UpdateManager::UpdateManager(std::string appDir)
    : m_appDir(std::move(appDir))
    , m_enabled(!m_appDir.empty())
{
}

UpdateManager::~UpdateManager()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

void UpdateManager::enable(std::string appDir)
{
    m_appDir = std::move(appDir);
    m_enabled = !m_appDir.empty();
}

// -------------------------------------------------------------------

bool UpdateManager::isCancelled() const
{
    return m_cancelled.load(std::memory_order_acquire);
}

void UpdateManager::setStage(UpdateStage stage)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.stage = stage;
    if (stage != UpdateStage::Downloading && stage != UpdateStage::Installing)
        m_snapshot.percent = 0;
}

void UpdateManager::setError(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.stage = UpdateStage::Error;
    m_snapshot.error = msg;
    m_snapshot.percent = 0;
}

// -------------------------------------------------------------------

void UpdateManager::checkForUpdates()
{
    if (!m_enabled) {
        setError("updates not available");
        m_done.store(true, std::memory_order_release);
        return;
    }

    // M7: Never join() a live worker from the SDL thread.
    // If a previous worker is still running, this call is a no-op.
    // The only safe join is in the destructor (where the caller
    // expects to wait).
    if (m_thread.joinable() && !busy()) {
        m_thread.join();
    } else if (busy()) {
        // A worker is still active — do nothing, avoid blocking UI.
        return;
    }

    // Reset state
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = UpdateSnapshot{};
    }
    m_cancelled.store(false, std::memory_order_release);
    m_confirmInstall.store(false, std::memory_order_release);
    m_done.store(false, std::memory_order_release);
    m_workerRunning.store(true, std::memory_order_release);

    m_thread = std::thread(&UpdateManager::workerRun, this);
}

void UpdateManager::confirmInstall()
{
    m_confirmInstall.store(true, std::memory_order_release);
}

void UpdateManager::cancel()
{
    m_cancelled.store(true, std::memory_order_release);
    // Wake the thread if it's waiting on confirmation.
    m_confirmInstall.store(true, std::memory_order_release);
}

bool UpdateManager::busy() const
{
    return m_workerRunning.load(std::memory_order_acquire);
}

UpdateSnapshot UpdateManager::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool UpdateManager::pollDone()
{
    return m_done.exchange(false, std::memory_order_acquire);
}

// -------------------------------------------------------------------
// Worker
// -------------------------------------------------------------------

void UpdateManager::workerRun()
{
    // RAII guard: always clear running + set done on exit.
    struct RunningGuard {
        std::atomic<bool> &running;
        std::atomic<bool> &done;
        ~RunningGuard() {
            running.store(false, std::memory_order_release);
            done.store(true, std::memory_order_release);
        }
    } guard{m_workerRunning, m_done};

    // ---------------------------------------------------------------
    // Step 0: Resolve manifest source (env / dev file / production)
    // ---------------------------------------------------------------
    std::string envUrl;
    {
        const char *v = std::getenv("MIYOOFIN_UPDATE_URL");
        if (v) envUrl = v;
    }

    std::string devFileContents;
    if (!m_appDir.empty()) {
        std::string devPath = m_appDir + "/update-dev-url.txt";
        std::ifstream ifs(devPath);
        if (ifs.is_open()) {
            devFileContents.assign(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());
        }
    }

    ManifestSource src = resolveManifestSource(
        envUrl, devFileContents, MANIFEST_URL);
    m_devOverride = src.devOverride;

    // ---------------------------------------------------------------
    // Step 1: Fetch and parse manifest
    // ---------------------------------------------------------------
    setStage(UpdateStage::Checking);

    if (isCancelled()) { setError("cancelled"); return; }

    std::string body;

    if (isLocalAsset(src.url)) {
        // Local file: read directly, no HTTP.
        std::string path = localAssetPath(src.url);
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            setError("manifest not found");
            return;
        }
        body.assign((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
    } else {
        HttpClient client;
        client.setTimeoutSec(10);
        client.setConnectTimeoutSec(10);
        long httpCode = 0;
        std::string error;

        if (!client.get(src.url, body, httpCode, error)) {
            setError(updateCheckErrorMessage(httpCode, error));
            return;
        }
    }

    UpdateManifest manifest;
    if (!parseUpdateManifest(body, manifest, m_devOverride)) {
        setError("invalid manifest");
        return;
    }

    if (isCancelled()) { setError("cancelled"); return; }

    // ---------------------------------------------------------------
    // Step 2: Compare versions
    // ---------------------------------------------------------------
    if (!isNewerThan(manifest.version, VERSION_STR)) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.stage = UpdateStage::UpToDate;
            m_snapshot.availableVersion = manifest.version;
        }
        return;
    }

    // Check minVersion: if current < minVersion, the user must update stepwise.
    if (!manifest.minVersion.empty() &&
        isNewerThan(manifest.minVersion, VERSION_STR)) {
        setError("please update stepwise");
        return;
    }

    // ---------------------------------------------------------------
    // Step 3: Available — wait for user confirmation
    // ---------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.stage = UpdateStage::Available;
        m_snapshot.availableVersion = manifest.version;
    }

    while (!m_confirmInstall.load(std::memory_order_acquire) &&
           !isCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (isCancelled()) { setError("cancelled"); return; }
    m_confirmInstall.store(false, std::memory_order_release);

    // ---------------------------------------------------------------
    // Step 4: Download
    // ---------------------------------------------------------------
    setStage(UpdateStage::Downloading);

    std::string downloadDir = m_appDir + "/update-download";
    ::mkdir(downloadDir.c_str(), 0755);

    // Key .part file by target version so a different release's partial
    // is never resumed (minor).
    std::string partPath = downloadDir + "/MiyooFin-" +
                           manifest.version + ".tar.gz.part";

    // Free-space pre-check: require the download size plus ~8 MiB slack.
    {
        struct statvfs vfs;
        if (statvfs(m_appDir.c_str(), &vfs) == 0) {
            std::uint64_t freeBytes =
                static_cast<std::uint64_t>(vfs.f_bsize) * vfs.f_bavail;
            if (freeBytes < manifest.tarGz.size + 8ULL * 1024 * 1024) {
                setError("disk full");
                return;
            }
        }
    }

    // M4: If .part size equals expected size, skip download and go
    // straight to verify (already-complete from a cancelled download).
    std::uint64_t resumeFrom = 0;
    bool skipDownload = false;
    {
        struct stat st;
        if (stat(partPath.c_str(), &st) == 0) {
            resumeFrom = static_cast<std::uint64_t>(st.st_size);
            if (resumeFrom == manifest.tarGz.size && manifest.tarGz.size > 0) {
                skipDownload = true;
            }
        }
    }

    bool downloaded = skipDownload;

    // Dev-override local asset: copy file instead of HTTP download.
    if (!downloaded && m_devOverride && isLocalAsset(manifest.tarGz.url)) {
        std::string srcPath = localAssetPath(manifest.tarGz.url);
        std::ifstream srcFile(srcPath, std::ios::binary);
        if (!srcFile.is_open()) {
            setError("local tarball not found");
            return;
        }
        std::ofstream dstFile(partPath, std::ios::binary | std::ios::trunc);
        if (!dstFile.is_open()) {
            setError("cannot write update file");
            return;
        }
        dstFile << srcFile.rdbuf();
        if (!srcFile.good() || !dstFile.good()) {
            setError("local copy failed");
            std::remove(partPath.c_str());
            return;
        }
        dstFile.close();
        downloaded = true;
    }

    if (!downloaded) {
        // Up to 3 attempts with cancellable 1s/2s/4s backoff.
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (isCancelled()) { setError("cancelled"); return; }

            std::string dlError;
            HttpClient dlClient;
            dlClient.setTimeoutSec(300);
            dlClient.setConnectTimeoutSec(15);

            const std::uint64_t totalSize = manifest.tarGz.size;
            std::uint64_t bytesReceived = 0;

            bool ok = dlClient.downloadToFile(
                manifest.tarGz.url, {}, partPath, dlError,
                &bytesReceived,
            [this, resumeFrom, totalSize](
                std::uint64_t received, std::uint64_t /*total*/) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_snapshot.bytesReceived = resumeFrom + received;
                    // Use the known total size from the manifest for
                    // the percent scale, not curl's per-request total
                    // which is the remaining portion on resume (minor).
                    m_snapshot.bytesTotal = totalSize;
                    if (m_snapshot.bytesTotal > 0) {
                        m_snapshot.percent = static_cast<int>(
                            m_snapshot.bytesReceived * 100 /
                            m_snapshot.bytesTotal);
                        if (m_snapshot.percent > 100)
                            m_snapshot.percent = 100;
                    }
                },
                &m_cancelled,
                300, 15, resumeFrom);

            if (ok) { downloaded = true; break; }
            if (isCancelled()) { setError("cancelled"); return; }

            // M4: Treat HTTP 416 (Range Not Satisfiable) as "already
            // complete" — the .part file was fully downloaded.
            if (dlError.find("416") != std::string::npos) {
                downloaded = true;
                break;
            }

            // Cancellable backoff: 1s, 2s, 4s.
            const std::uint64_t backoffMs = 1000ULL << attempt;
            for (std::uint64_t i = 0; i < backoffMs && !isCancelled(); i += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Update resume point for next attempt.  If the .part was
            // deleted (e.g. by a 4xx error), reset to 0 so the next
            // attempt opens with "wb" and does a full download.
            {
                struct stat st;
                if (stat(partPath.c_str(), &st) == 0)
                    resumeFrom = static_cast<std::uint64_t>(st.st_size);
                else
                    resumeFrom = 0;
            }
        }
    }

    if (!downloaded) {
        setError("download failed");
        return;
    }

    // ---------------------------------------------------------------
    // Step 5: Verify SHA-256 and size
    // ---------------------------------------------------------------
    setStage(UpdateStage::Verifying);

    {
        std::string shaHex;
        std::string shaError;
        if (!sha256File(partPath, shaHex, shaError, &m_cancelled)) {
            if (isCancelled()) { setError("cancelled"); return; }
            setError("verification failed");
            return;
        }
        if (shaHex != manifest.tarGz.sha256) {
            std::remove(partPath.c_str());
            setError("checksum mismatch");
            return;
        }
    }

    // Size check.
    {
        struct stat st;
        if (stat(partPath.c_str(), &st) == 0) {
            if (static_cast<std::uint64_t>(st.st_size) != manifest.tarGz.size) {
                std::remove(partPath.c_str());
                setError("size mismatch");
                return;
            }
        }
    }

    if (isCancelled()) { setError("cancelled"); return; }

    // ---------------------------------------------------------------
    // Step 6: Install
    // ---------------------------------------------------------------
    setStage(UpdateStage::Installing);

    {
        std::string installError;
        bool ok = installUpdate(
            m_appDir, partPath, manifest.version, installError,
            &m_cancelled,
            [this](int pct) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_snapshot.percent = pct;
            });
        if (!ok) {
            // M5: If installUpdate failed, the error is a real failure
            // (it already rolled back).  Propagate the detail (minor).
            if (!installError.empty() && installError != "cancelled")
                setError("install failed: " + installError);
            else
                setError("install failed");
            return;
        }
    }

    // ---------------------------------------------------------------
    // Step 7: Done — cleanup and report success
    // ---------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.stage = UpdateStage::ReadyToRestart;
        m_snapshot.availableVersion = manifest.version;
        m_snapshot.percent = 100;
    }

    // Minor: clean up after successful install — remove downloaded
    // tarball.  update-backup/ is NOT pruned here; backups accumulate
    // and the user can remove old versions manually.
    std::remove(partPath.c_str());
}

} // namespace miyoofin
