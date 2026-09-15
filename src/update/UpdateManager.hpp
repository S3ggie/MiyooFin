#ifndef MIYOOFIN_UPDATE_MANAGER_HPP
#define MIYOOFIN_UPDATE_MANAGER_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace miyoofin {

enum class UpdateStage {
    Idle, Checking, UpToDate, Available, Downloading,
    Verifying, Installing, ReadyToRestart, Error
};

struct UpdateSnapshot {
    UpdateStage stage = UpdateStage::Idle;
    int percent = 0;
    std::string availableVersion;   // "0.2.0"
    std::string error;              // short, user-facing, no URLs/tokens
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesTotal = 0;
};

/// Human-readable status text for a given update snapshot.
std::string updateStatusText(const UpdateSnapshot &snap);

/// Human-readable message for a failed update check.
/// httpCode > 0 means the server answered with that HTTP status;
/// httpCode == 0 means a transport failure (DNS/TLS/connect), for which
/// transportError carries libcurl's short reason.
std::string updateCheckErrorMessage(long httpCode,
                                    const std::string &transportError);

/// Background OTA update manager.  Owns one worker thread; the SDL thread
/// reads only via snapshot() and polls pollDone().
class UpdateManager {
public:
    /// Construct a disabled manager (appDir unknown).
    UpdateManager() = default;

    /// Construct an enabled manager that resolves updates relative to appDir.
    explicit UpdateManager(std::string appDir);

    /// Cancel any in-progress work and join the worker thread.
    ~UpdateManager();

    UpdateManager(const UpdateManager &) = delete;
    UpdateManager &operator=(const UpdateManager &) = delete;

    /// Enable with an application directory (for deferred construction).
    void enable(std::string appDir);

    /// True if the manager has a valid app directory.
    bool enabled() const { return m_enabled; }

    /// Start a background update check.  If a newer version is found the
    /// worker pauses at Available until confirmInstall() is called.
    void checkForUpdates();

    /// Signal the worker to proceed from Available to download+install.
    void confirmInstall();

    /// Request cooperative cancellation.
    void cancel();

    /// True while a worker is running.
    bool busy() const;

    /// Mutex-guarded snapshot for the UI thread.
    UpdateSnapshot snapshot() const;

    /// Returns true once per completed run, then clears the flag.
    bool pollDone();

private:
    void workerRun();
    bool isCancelled() const;
    void setStage(UpdateStage stage);
    void setError(const std::string &msg);

    std::string m_appDir;
    bool m_enabled = false;

    mutable std::mutex m_mutex;
    UpdateSnapshot m_snapshot;

    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_confirmInstall{false};
    std::atomic<bool> m_done{false};
    std::atomic<bool> m_workerRunning{false};

    std::thread m_thread;
};

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_MANAGER_HPP
