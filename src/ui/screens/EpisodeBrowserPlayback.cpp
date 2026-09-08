#include "EpisodeBrowserScreen.hpp"
#include "../../download/DownloadSupport.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../playback/PlaybackRequest.hpp"
#include <cstdio>

namespace miyoofin {

void EpisodeBrowserScreen::startSelectedEpisodePlayback()
{
    printf("[EpisodeBrowserScreen] Play selected: %s\n",
           m_episodes[m_selectedEpisode].title.c_str());
    std::string error;
    PlaybackSource source=m_downloads?resolvePlayback(m_episodes[m_selectedEpisode],*m_downloads):PlaybackSource::Jellyfin;
    if (source==PlaybackSource::UnavailableOffline) return;
    if (PlaybackRequest::writeWithSourceTo(PlaybackRequest::defaultPath(),
            m_episodes[m_selectedEpisode].id,
            "episode",
            m_episodes[m_selectedEpisode].playbackPositionTicks, source==PlaybackSource::Local?"local":"jellyfin", source==PlaybackSource::Local?m_downloads->scope():"",
            error))
    {
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerPaused = true;
            ++m_workerGeneration;
        }
        m_prefetchResumePending = true;
        m_prefetchResumeDelayUpdates = 1;
        m_playbackEpisodeId = m_episodes[m_selectedEpisode].id;
        printf("[EpisodeBrowserScreen] Playback request "
               "written, requesting external playback\n");
        m_stack->requestExternalPlayback(
            source == PlaybackSource::Local
                ? ScreenStack::ExternalPlaybackSource::Local
                : ScreenStack::ExternalPlaybackSource::Jellyfin);
    } else {
        printf("[EpisodeBrowserScreen] Playback request "
               "failed: %s\n", error.c_str());
    }
}

void EpisodeBrowserScreen::updatePlaybackState()
{
    if (advancePrefetchResume(m_prefetchResumePending,
                              m_prefetchResumeDelayUpdates)) {
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerPaused = false;
            ++m_workerGeneration;
        }
        m_workerCv.notify_one();

        std::int64_t resultTicks = 0;
        std::string error;
        if (PlaybackRequest::consumeResult(m_playbackEpisodeId,
                                           resultTicks, error)) {
            const int playedIndex = findEpisodeIndex(m_episodes,
                                                      m_playbackEpisodeId);
            if (playedIndex >= 0) {
                m_episodes[playedIndex].playbackPositionTicks = resultTicks;
                printf("[EpisodeBrowserScreen] Playback position updated: "
                       "%lld\n", (long long)resultTicks);
            }
        }
        m_playbackEpisodeId.clear();
    }
}

} // namespace miyoofin
