#pragma once

#include <curl/curl.h>
#include <unistd.h>
#include <atomic>
#include <string>

namespace miyoofin {
namespace tls_detail {
// -1 unknown, 0 missing, 1 present.  Relaxed atomics are sufficient: every
// racing thread computes the same access() result, so a duplicate probe is
// benign and no thread ever blocks on this check.
inline std::atomic<int>& caBundleCache()
{
    static std::atomic<int> cached{-1};
    return cached;
}
inline void resetCaBundleCacheForTest()
{
    caBundleCache().store(-1, std::memory_order_relaxed);
}
} // namespace tls_detail

// The packaged bundle path never changes at runtime, so probe the filesystem
// once and reuse the verdict instead of an access() syscall per TLS handle.
inline bool tlsCaBundleReady()
{
    int verdict = tls_detail::caBundleCache().load(std::memory_order_relaxed);
    if (verdict < 0) {
        verdict = (::access("cacert.pem", R_OK) == 0) ? 1 : 0;
        tls_detail::caBundleCache().store(verdict, std::memory_order_relaxed);
    }
    return verdict == 1;
}

// launch.sh changes into the installed application directory, where the
// packaged curl/Mozilla bundle is staged as cacert.pem.
inline bool configureTls(CURL* curl, const std::string& url, std::string* error = nullptr)
{
    if (url.compare(0, 8, "https://") != 0)
        return true; // Plain LAN HTTP neither needs nor consults CA data.
    static const char kCaBundle[] = "cacert.pem";
    if (!tlsCaBundleReady()) {
        if (error)
            *error = "HTTPS requires packaged cacert.pem";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, kCaBundle);
    return true;
}

} // namespace miyoofin
