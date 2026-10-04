#include "test_support.hpp"
#include "cases/test_library_coordinator_support.hpp"

namespace {
void testLegacyFullSyncReservationGatesAndReleases()
{
    std::printf("[test] LibraryCoordinator legacy reservation\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("legacy-reservation", db, scope);
    coordinator->start();

    // The legacy reservation owns the serialized slot without a worker; every
    // other top-level operation must observe it as occupied.
    CHECK(coordinator->beginFullSync());
    CHECK(!coordinator->beginFullSync());
    CHECK(!coordinator->startStartupSync(false));
    std::uint64_t populationRequest = 0;
    CHECK(!coordinator->requestFullPopulation(populationRequest));
    CHECK(!coordinator->requestMaintenance());
    MediaItem series;
    series.id = "legacy-gated-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(!coordinator->requestSeriesSeasons(series, hierarchyRequest));

    // Discarding live results must not release the slot held by a different
    // operation; the model only releases the matching kind.
    coordinator->discardLiveChangeResults();
    CHECK(!coordinator->startStartupSync(false));

    // Releasing the reservation admits the next serialized operation.
    coordinator->finishFullSync();
    CHECK(coordinator->startStartupSync(false));
    const auto startup = takeStartupResult(*coordinator);
    CHECK(startup.success || startup.cancelled ||
          startup.error == CatalogDbErrorCategory::ScopeNotReady);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator legacy reservation OK\n");
}

void testConcurrentLegacyReservationReservesExactlyOnce()
{
    std::printf("[test] LibraryCoordinator concurrent reservation\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    std::mutex startMutex;
    std::condition_variable startCondition;
    int ready = 0;
    bool release = false;
    bool firstAccepted = false;
    bool secondAccepted = false;
    const auto reserve = [&](bool& accepted) {
        {
            std::unique_lock<std::mutex> lock(startMutex);
            ++ready;
            startCondition.notify_all();
            startCondition.wait(lock, [&] { return release; });
        }
        accepted = coordinator.beginFullSync();
    };

    std::thread first(reserve, std::ref(firstAccepted));
    std::thread second(reserve, std::ref(secondAccepted));
    {
        std::unique_lock<std::mutex> lock(startMutex);
        CHECK(startCondition.wait_for(lock, std::chrono::seconds(2), [&] { return ready == 2; }));
        release = true;
        startCondition.notify_all();
    }
    first.join();
    second.join();

    // The single serialized-slot authority admits exactly one reservation.
    CHECK(firstAccepted != secondAccepted);
    coordinator.finishFullSync();
    CHECK(coordinator.beginFullSync());
    coordinator.finishFullSync();

    coordinator.stop();
    std::printf("[test] LibraryCoordinator concurrent reservation OK\n");
}

void testUnconsumedStartupResultRetainsSerializedSlot()
{
    std::printf("[test] LibraryCoordinator result retains serialized slot\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("result-slot", db, scope);
    coordinator->start();

    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    db->setWorkerPausedForTest(false);

    bool published = false;
    for (int i = 0; i < 200000 && !published; ++i) {
        published = coordinator->status().startupResultReady;
        if (!published)
            std::this_thread::yield();
    }
    CHECK(published);

    // An unconsumed startup publication retains the slot for every other
    // operation, not merely for a second startup.
    CHECK(!coordinator->beginFullSync());
    MediaItem series;
    series.id = "result-slot-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(!coordinator->requestSeriesSeasons(series, hierarchyRequest));

    // Consuming the result releases the slot for a different operation.
    library::StartupSyncResult result;
    CHECK(coordinator->takeStartupSyncResult(result));
    CHECK(coordinator->beginFullSync());
    coordinator->finishFullSync();

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator result retains serialized slot OK\n");
}

// A safety reconcile releases the serialized slot as soon as its mutation is
// durable while keeping the terminal result for Home.  Same-kind admission is
// therefore gated on the pending result (not on slot occupancy), and a
// different kind such as hierarchy is admitted with the result unconsumed.
void testUnconsumedSafetyResultAdmitsHierarchyAndSurvives()
{
    std::printf("[test] LibraryCoordinator unconsumed safety result admits hierarchy\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("safety-unconsumed", db, scope);
    coordinator->start();

    CHECK(coordinator->requestSafetyReconcileForTest());
    for (int i = 0; i < 2000000 && !coordinator->safetyReconcileResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator->safetyReconcileResultReadyForTest());
    // The slot is free once the mutation finishes, not merely after Home
    // consumes the result.
    CHECK(!coordinator->status().safetyReconcileInFlight);

    // A second same-kind request is rejected by the pending result while the
    // slot is otherwise free, so it cannot overwrite the publication.
    CHECK(!coordinator->requestSafetyReconcileForTest());

    // A different kind is admitted with the result still unconsumed.
    MediaItem series;
    series.id = "safety-unconsumed-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator->requestSeriesSeasons(series, hierarchyRequest));

    // The retained result is still consumable after the hierarchy admission.
    library::SafetyReconcileResult result;
    CHECK(coordinator->takeSafetyReconcileResult(result));
    CHECK(!coordinator->takeSafetyReconcileResult(result));

    coordinator->cancelHierarchyRequest(hierarchyRequest);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator unconsumed safety result admits hierarchy OK\n");
}

// Hierarchy is admitted while an unconsumed live result retains its
// publication, and the result survives that admission.
void testUnconsumedLiveResultAdmitsHierarchy()
{
    std::printf("[test] LibraryCoordinator unconsumed live result admits hierarchy\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    JellyfinLibraryChangeBatch first;
    first.itemsUpdated.push_back("live-first");
    first.userDataChanged = true;
    CHECK(coordinator.requestLiveChange(first));
    for (int i = 0; i < 2000000 && !coordinator.liveChangeResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.liveChangeResultReadyForTest());

    MediaItem series;
    series.id = "live-unconsumed-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator.requestSeriesSeasons(series, hierarchyRequest));

    // The unconsumed result survives the hierarchy admission.
    library::LiveLibraryChangeResult result;
    CHECK(coordinator.takeLiveChangeResult(result));
    CHECK(result.userDataChanged);

    coordinator.cancelHierarchyRequest(hierarchyRequest);
    coordinator.stop();
    std::printf("[test] LibraryCoordinator unconsumed live result admits hierarchy OK\n");
}

// A queued later live batch must neither overwrite an unconsumed result nor be
// lost once that result is consumed.
void testUnconsumedLiveResultIsNotOverwritten()
{
    std::printf("[test] LibraryCoordinator unconsumed live result not overwritten\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    JellyfinLibraryChangeBatch first;
    first.itemsUpdated.push_back("live-first");
    first.userDataChanged = true;
    CHECK(coordinator.requestLiveChange(first));
    for (int i = 0; i < 2000000 && !coordinator.liveChangeResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.liveChangeResultReadyForTest());

    // Queue a second live batch while the first result is still unconsumed.
    JellyfinLibraryChangeBatch second;
    second.itemsUpdated.push_back("live-second");
    second.userDataChanged = false;
    CHECK(coordinator.requestLiveChange(second));

    // The unconsumed result is the first batch's, not an overwrite.
    library::LiveLibraryChangeResult result;
    CHECK(coordinator.takeLiveChangeResult(result));
    CHECK(result.userDataChanged);

    // After consumption the queued batch is processed and published, so no
    // live change is silently lost.
    for (int i = 0; i < 2000000 && !coordinator.liveChangeResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.liveChangeResultReadyForTest());
    library::LiveLibraryChangeResult next;
    CHECK(coordinator.takeLiveChangeResult(next));
    CHECK(!next.userDataChanged);

    coordinator.stop();
    std::printf("[test] LibraryCoordinator unconsumed live result not overwritten OK\n");
}

// Cold-start precedence: a live change that becomes available while Home's
// startup and initial full population are still pending must not preempt them.
// Startup and population both complete first; the retained live change is then
// processed rather than lost.  Ordering is established with the coordinator's
// condition-variable seams rather than polling: the demand is armed before the
// live worker starts, and waitLiveChangeDeferredForTest proves the worker
// reached the gate without claiming the serialized slot.
void testStartupPopulationPrecedesLiveChangeAtColdStart()
{
    std::printf("[test] LibraryCoordinator cold-start live precedence\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("cold-start-live", db, scope);
    // Home's startup owner reserves cold-start precedence before the live
    // worker starts, so a queued live batch cannot claim the slot first.
    const auto startupToken = coordinator->reserveStartupSequence();
    coordinator->start();

    // The live change is available before Home requests startup.  The
    // reservation makes the live worker defer it deterministically.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));

    // Block until the worker has observed the cold-start gate; no sleeps or
    // yield budgets are involved.
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Startup wins the serialized slot despite the queued live change.
    CHECK(coordinator->startStartupSync(false));
    // A release from the original owner after startup has claimed the sequence
    // must be a no-op: the coordinator now owns the cold-start demand, so the
    // retained live change stays gated across the startup -> population
    // handoff.
    coordinator->releaseStartupSequence(startupToken);
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    // Startup is complete, but its decision requires a full population, so the
    // live change stays deferred across the startup -> population handoff.
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Full population also wins the slot and completes.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));

    // Draining the cold-start sequence released the demand; the retained live
    // change is now applied rather than lost.  Wait on the publication
    // condition variable instead of polling for it.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.userDataChanged);
    CHECK(applied.success);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator cold-start live precedence OK\n");
}

// Regression: an abandoned startup reservation must be releasable by its owner.
// AppSession reserves cold-start precedence before start(), but if Home's
// startup sequence is never run the reservation would otherwise defer every
// live change forever.  Releasing the still-unclaimed reservation must let the
// retained live batch apply.  No sleeps are involved: the gate is observed
// through the coordinator's condition-variable seams.
void testAbandonedStartupReservationReleasesLiveProcessing()
{
    std::printf("[test] LibraryCoordinator abandoned reservation release\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("abandoned-reservation", db, scope);
    const auto reservationToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));

    // The unclaimed reservation deterministically gates the live worker.
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // The owner abandons Home's startup sequence before it ever starts.
    coordinator->releaseStartupSequence(reservationToken);
    // Releasing again is idempotent and must not disturb the resumed worker.
    coordinator->releaseStartupSequence(reservationToken);

    // The retained live change is now applied rather than stranded.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.userDataChanged);
    CHECK(applied.success);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator abandoned reservation release OK\n");
}
} // namespace

int main()
{
    const std::string diagnosticsPath = "/tmp/miyoofin-library-coordinator-diagnostics-" +
                                        std::to_string(static_cast<long long>(::getpid())) + ".log";
    std::remove(diagnosticsPath.c_str());
    uiDiagnostics().start(diagnosticsPath);

    testLegacyFullSyncReservationGatesAndReleases();
    testConcurrentLegacyReservationReservesExactlyOnce();
    testUnconsumedStartupResultRetainsSerializedSlot();
    testUnconsumedSafetyResultAdmitsHierarchyAndSurvives();
    testUnconsumedLiveResultAdmitsHierarchy();
    testUnconsumedLiveResultIsNotOverwritten();
    testStartupPopulationPrecedesLiveChangeAtColdStart();
    testAbandonedStartupReservationReleasesLiveProcessing();
    // All producers above have joined.  stop() is the existing logger
    // drain/join barrier, not a timing delay.
    uiDiagnostics().stop();
    std::remove(diagnosticsPath.c_str());
    return miyoofin_test::finish("library_coordinator_admission");
}
