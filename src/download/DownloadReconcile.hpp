#ifndef MIYOOFIN_DOWNLOAD_RECONCILE_HPP
#define MIYOOFIN_DOWNLOAD_RECONCILE_HPP
#include "DownloadTypes.hpp"
#include "../net/JellyfinApi.hpp"
namespace miyoofin {
enum class SourceCheck { Same, Changed, Missing, Transient, Unauthorized };
// Returns true when stale incomplete bytes must be removed before retrying.
bool reconcileSource(DownloadItem &item, SourceCheck result, const DownloadMediaSource *source=nullptr);
}
#endif
