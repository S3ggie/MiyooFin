#include "test_support.hpp"
#include "cases/test_artwork_episode_support.hpp"
#include "cases/test_artwork_episode_scheduling.inc"

int main()
{
    testSameKeyNotLoadedTwice();
    testAllVisibleCandidatesScheduledPerCycle();
    testEpisodePrefetchScheduler();
    testEpisodeArtworkPreemption();
    testEpisodePrefetchPlaybackResume();
    testPosterDedupPreventsRequeueWhilePending();
    testPosterDedupSuccessEraseAllowsReAdmit();
    testPosterDedupEvictionRecovery();
    testDecodeRetryBoundBelowLimit();
    testDecodeRetryBoundAtLimit();
    testDecodeRetryBoundSuccessResets();
    testDecodeRetryBoundNewKeyIsFresh();
    testBuildPosterJobFromKeyPrimary();
    testBuildPosterJobFromKeyThumb();
    testEvictedKeyResubmittedForDecode();
    testFailedKeySkippedByGate();
    return miyoofin_test::finish("artwork-episode-scheduling");
}
