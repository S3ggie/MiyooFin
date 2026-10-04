#include "test_support.hpp"
#include "cases/test_library_coordinator_support.hpp"

namespace {
// Regression: leaving manual offline with the cold-start reservation still
// unclaimed must re-arm the startup demand before Home's online fetch runs.
// Entering offline clears the demand (so retained live changes still apply),
// but the reservation itself must keep gating the live worker once the session
// returns online.
void testManualOfflineReturnReArmsStartupReservation()
{
    std::printf("[test] LibraryCoordinator offline->online re-arms reservation\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("offline-rearm", db, scope);
    // Online reserve, then go offline before any startup request claims it.
    coordinator->reserveStartupSequence();
    coordinator->start();

    coordinator->setManualOfflineMode(true);
    // Offline must not gate live processing: a user-data-only change applies.
    JellyfinLibraryChangeBatch offlineBatch;
    offlineBatch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(offlineBatch));
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult offlineResult;
    CHECK(coordinator->takeLiveChangeResult(offlineResult));
    CHECK(offlineResult.success && offlineResult.userDataChanged);

    // Returning online with the reservation still unclaimed re-arms the gate.
    coordinator->setManualOfflineMode(false);
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The live worker is gated again until startup claims the sequence.
    JellyfinLibraryChangeBatch onlineBatch;
    onlineBatch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(onlineBatch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Startup claims the sequence; its FullReconcile decision keeps the demand
    // armed across the handoff into the full population Home requests next.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));

    // Completing the sequence releases the retained live change.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator offline->online re-arms reservation OK\n");
}

// Regression: a competing startup/full-population admission that loses the
// serialized slot must not clear the demand owned by the sequence that already
// claimed it.  Ownership and the armed gate are observed through the
// coordinator's deterministic seams; the owner's retained live change stays
// gated after both competitors fail.
void testCompetingAdmissionDoesNotClobberStartupDemand()
{
    std::printf("[test] LibraryCoordinator competing admission keeps demand\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("competing-admission", db, scope);
    coordinator->start();

    // Pause the db worker so the first startup stays executing and keeps
    // owning the serialized slot while the competitors run.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    db->setWorkerPausedForTest(false);
    CHECK(coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // A retained live change must stay deferred behind the owned demand.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Competing startup and full-population admissions both lose the slot.
    CHECK(!coordinator->startStartupSync(false));
    std::uint64_t populationRequest = 0;
    CHECK(!coordinator->requestFullPopulation(populationRequest));

    // Neither competitor may clear the owning sequence's demand/ownership.
    CHECK(coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The owner completes and the retained live change is finally applied.
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator competing admission keeps demand OK\n");
}

// Regression: after startup publishes a FullReconcile decision, the sequence
// hands off to the full population Home requests next.  During that window the
// demand stays armed but no operation actively claims it, so the owner-held
// handoff token must remain releasable.  If Home is torn down or cancelled
// before requesting the population (modelled here by releaseStartupSequence()
// from the owner), retained live changes must resume rather than starve.
void testStartupHandoffReleaseUnblocksLiveChange()
{
    std::printf("[test] LibraryCoordinator startup handoff release resumes live\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-release", db, scope);
    const auto ownerToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    // Handoff state: the active startup claim ended, but Home still owns the
    // armed demand through the separate handoff token.
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Home abandons the pending population before requesting it.
    coordinator->releaseStartupSequence(ownerToken);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    // The retained live change resumes instead of starving forever.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.userDataChanged);
    CHECK(applied.success);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator startup handoff release resumes live OK\n");
}

// Regression: entering manual offline during the startup -> population handoff
// clears the demand so live changes still apply, and leaving offline must
// re-arm it from the still-held handoff token so the population Home requests
// next cannot be preempted by a retained live change.
void testManualOfflineReArmsAcrossStartupHandoff()
{
    std::printf("[test] LibraryCoordinator offline->online re-arms across handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-offline-rearm", db, scope);
    coordinator->reserveStartupSequence();
    coordinator->start();

    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);

    // Offline across the handoff: the demand clears and a live change applies.
    coordinator->setManualOfflineMode(true);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());
    JellyfinLibraryChangeBatch offlineBatch;
    offlineBatch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(offlineBatch));
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult offlineResult;
    CHECK(coordinator->takeLiveChangeResult(offlineResult));
    CHECK(offlineResult.success && offlineResult.userDataChanged);

    // Returning online re-arms the handoff demand from the retained token.
    coordinator->setManualOfflineMode(false);
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The population Home requests next completes the handoff; draining its
    // terminal publication ends the sequence and clears the demand.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator offline->online re-arms across handoff OK\n");
}

// Regression: the startup -> full-population handoff owner belongs to Home's
// pending population request.  A competing startup/full admission that arrives
// in that window must not steal the handoff token, and if it fails to admit it
// must not clear Home's armed demand.  Otherwise an unrelated failure could
// release the cold-start gate before the population Home is about to request
// has run, letting a retained live change preempt it.  The competing admission
// failure is made deterministic by occupying the serialized slot with an
// unrelated full-sync reservation; no sleeps are involved.
void testCompetingAdmissionDuringHandoffKeepsOwner()
{
    std::printf("[test] LibraryCoordinator competing admission during handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-competing", db, scope);
    coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff.  Home owns the
    // armed demand but has not yet requested the population.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // Occupy the slot with an unrelated operation so the competing startup
    // admission deterministically fails instead of starting.
    CHECK(coordinator->beginFullSync());

    // The competing startup must neither consume the handoff owner nor release
    // Home's demand when its admission fails.
    CHECK(!coordinator->startStartupSync(false));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Release the unrelated operation.  Home's intended population now consumes
    // the handoff, completes, and releases the retained live change.
    coordinator->finishFullSync();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator competing admission during handoff OK\n");
}

// Regression (reviewer): during the startup -> population handoff the
// serialized slot is idle, so occupancy alone does not reject a competing
// startup.  The handoff admission gate must reject it explicitly; otherwise it
// is admitted and its terminal path clears Home's handoff and armed demand
// before the intended population runs.  With the slot confirmed idle, the
// competing startup is rejected and publishes nothing, the intended
// FullPopulation still owns and completes the sequence, and the retained live
// change is finally applied.
void testCompetingStartupRejectedWhileHandoffIdle()
{
    std::printf("[test] LibraryCoordinator competing startup rejected at idle handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-idle", db, scope);
    coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff.  Home owns the
    // armed demand but has not yet requested the population.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    // The serialized slot is genuinely idle here, so the competing startup is
    // rejected by the handoff gate rather than by slot occupancy.
    CHECK(coordinator->serializedOperationIdleForTest());

    // The competing startup must be rejected and must publish no result.
    CHECK(!coordinator->startStartupSync(false));
    CHECK(!coordinator->status().startupInFlight);
    CHECK(!coordinator->status().startupResultReady);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Home's intended population consumes the handoff, completes the sequence,
    // and the retained live change is finally applied.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator competing startup rejected at idle handoff OK\n");
}

// Regression (reviewer): releaseStartupSequence() must be owner-scoped.  During
// the startup -> population handoff no operation actively claims the sequence,
// so a stale/non-owner Home destructor must not clear the intended owner's
// handoff or armed demand.  A newer reservation generation supersedes the older
// token; releasing the older token is inert, while the intended continuation
// (the population Home requests next) still consumes the handoff and completes.
void testStaleStartupReleaseKeepsOwnerHandoff()
{
    std::printf("[test] LibraryCoordinator stale startup release keeps owner handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("stale-release-handoff", db, scope);
    const auto staleToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff: the active claim
    // ends but the sequence and its demand are still owned.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // A new owner generation arms its own reservation (as a re-initialised
    // AppSession/Home would); the earlier token is now stale.
    const auto ownerToken = coordinator->reserveStartupSequence();
    CHECK(ownerToken != staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);

    // A stale/non-owner destructor releasing the old token must not clear the
    // active owner's handoff or demand.
    coordinator->releaseStartupSequence(staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // The kNo token from a controller that never reserved is likewise inert.
    coordinator->releaseStartupSequence(library::LibraryCoordinator::kNoStartupSequenceToken);
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The intended population still consumes the handoff, completes the
    // sequence, and releases the retained live change.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    // Terminal completion invalidates the owner token, so even the intended
    // owner's delayed release is now inert rather than resurrecting the
    // completed generation.
    CHECK(coordinator->startupSequenceOwnerToken() ==
          library::LibraryCoordinator::kNoStartupSequenceToken);
    coordinator->releaseStartupSequence(ownerToken);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator stale startup release keeps owner handoff OK\n");
}

// Regression: the unclaimed base reservation is owner-scoped too.  A stale
// token from a superseded generation must not release the newer generation's
// reservation/demand, while the matching owner token still does.
void testStaleStartupReleaseKeepsUnclaimedReservation()
{
    std::printf("[test] LibraryCoordinator stale release keeps unclaimed reservation\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("stale-release-unclaimed", db, scope);
    const auto staleToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // A newer owner generation re-arms the still-unclaimed reservation.
    const auto ownerToken = coordinator->reserveStartupSequence();
    CHECK(ownerToken != staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);

    // The stale token cannot release the newer owner's reservation.
    coordinator->releaseStartupSequence(staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // The matching owner token releases it and the retained live change applies.
    coordinator->releaseStartupSequence(ownerToken);
    CHECK(coordinator->startupSequenceOwnerToken() ==
          library::LibraryCoordinator::kNoStartupSequenceToken);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator stale release keeps unclaimed reservation OK\n");
}

// Regression (reviewer): a reserved-but-unclaimed cold-start sequence must
// survive a failed startup admission caused by another operation occupying the
// serialized slot.  Claim/ownership must be finalized only after admission
// succeeds, so the failed attempt leaves the owner-held reservation, token, and
// armed demand intact for a retry.  beginFullSync() deterministically occupies
// the slot as an unrelated operation; no sleeps are involved.
void testReservedStartupAdmissionFailureKeepsReservation()
{
    std::printf("[test] LibraryCoordinator reserved startup admission failure\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("reserved-admission-fail", db, scope);
    const auto ownerToken = coordinator->reserveStartupSequence();
    coordinator->start();

    // A retained live change is deferred behind the armed reservation.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Occupy the slot with an unrelated operation so startup admission fails.
    CHECK(coordinator->beginFullSync());
    CHECK(!coordinator->startStartupSync(false));

    // The failed admission must not have consumed the reservation: no active
    // claim, the owner token is unchanged, and the demand stays armed.
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Once the unrelated operation releases the slot, the same reservation is
    // admitted and owns the sequence.
    coordinator->finishFullSync();
    CHECK(coordinator->startStartupSync(false));
    CHECK(coordinator->startupSequenceClaimedForTest());
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);

    // The startup -> population handoff completes and releases the retained
    // live change rather than stranding it.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator reserved startup admission failure OK\n");
}

// Regression (reviewer): during the startup -> full-population handoff the
// intended continuation's FullPopulation admission can still fail because an
// unrelated operation already occupies the serialized slot as the same
// FullPopulation kind (beginFullSync).  Consuming the handoff before admission
// would steal Home's owner token and clear the armed demand on that failure.
// This verifies the handoff, token, and demand survive the failed admission and
// are consumed by the later, admitted population.  No sleeps are involved.
void testHandoffPopulationAdmissionFailureKeepsHandoff()
{
    std::printf("[test] LibraryCoordinator handoff population admission failure\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-admission-fail", db, scope);
    const auto ownerToken = coordinator->reserveStartupSequence();
    coordinator->start();

    // Defer a retained live change so the armed demand is observable.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // An unrelated full-population kind operation occupies the slot, so the
    // intended continuation's admission fails.
    CHECK(coordinator->beginFullSync());
    std::uint64_t request = 0;
    CHECK(!coordinator->requestFullPopulation(request));

    // The failed admission must not consume the handoff: no active claim, the
    // handoff is still pending, the owner token is unchanged, and the demand
    // stays armed.
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Once the unrelated operation releases the slot, the intended population
    // consumes the handoff and completes the sequence.
    coordinator->finishFullSync();
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator handoff population admission failure OK\n");
}

void testAdmissionDiagnosticSnapshotSurvivesConcurrentRelease()
{
    std::printf("[test] LibraryCoordinator admission diagnostic snapshot race\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    // Hierarchy owns the serialized slot.  The unconsumed terminal result keeps
    // it held, so no live network peer or sleep is needed.
    MediaItem series;
    series.id = "admission-snapshot-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator.requestSeriesSeasons(series, hierarchyRequest));

    // Arm the seam so the next admission parks between taking its operation
    // snapshot and deriving the diagnostic reason.
    coordinator.setAdmissionSnapshotPauseForTest(true);

    bool admitted = true;
    std::thread admitter([&] { admitted = coordinator.startStartupSync(false); });

    // Spin (no sleep) until the admission has snapshotted the occupied slot.
    for (int i = 0; i < 1000000 && !coordinator.admissionSnapshotPausedForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.admissionSnapshotPausedForTest());

    // Release the hierarchy slot while the admission is parked.  The old
    // implementation re-read the slot here and appended a null reason.
    coordinator.releaseCurrentOperationForTest();
    coordinator.setAdmissionSnapshotPauseForTest(false);

    admitter.join();
    CHECK(!admitted);

    coordinator.stop();
    std::printf("[test] LibraryCoordinator admission diagnostic snapshot race OK\n");
}
} // namespace

int main()
{
    const std::string diagnosticsPath = "/tmp/miyoofin-library-coordinator-diagnostics-" +
                                        std::to_string(static_cast<long long>(::getpid())) + ".log";
    std::remove(diagnosticsPath.c_str());
    uiDiagnostics().start(diagnosticsPath);

    testManualOfflineReturnReArmsStartupReservation();
    testCompetingAdmissionDoesNotClobberStartupDemand();
    testStartupHandoffReleaseUnblocksLiveChange();
    testManualOfflineReArmsAcrossStartupHandoff();
    testCompetingAdmissionDuringHandoffKeepsOwner();
    testCompetingStartupRejectedWhileHandoffIdle();
    testStaleStartupReleaseKeepsOwnerHandoff();
    testStaleStartupReleaseKeepsUnclaimedReservation();
    testReservedStartupAdmissionFailureKeepsReservation();
    testHandoffPopulationAdmissionFailureKeepsHandoff();
    testAdmissionDiagnosticSnapshotSurvivesConcurrentRelease();
    // All producers above have joined.  stop() is the existing logger
    // drain/join barrier, not a timing delay.
    uiDiagnostics().stop();
    const auto diagnostics = miyoofin_test::readTestBytes(diagnosticsPath);

    CHECK(diagnostics.find("[LibraryCoordinator] full_population_rejected phase=admission") !=
          std::string::npos);
    // The admission diagnostic must still name the snapshotted hierarchy slot
    // even though it was released before the reason was derived.
    CHECK(diagnostics.find("reasons=hierarchy_mutation_in_flight") != std::string::npos);
    std::remove(diagnosticsPath.c_str());
    return miyoofin_test::finish("library_coordinator_handoff");
}
