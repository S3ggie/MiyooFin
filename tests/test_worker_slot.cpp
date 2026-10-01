#include "test_support.hpp"

#include "../src/ui/WorkerSlot.hpp"

using miyoofin::CancelToken;
using miyoofin::WorkerSlot;

static void waitReap(WorkerSlot& slot)
{
    for (int i = 0; i < 5000 && !slot.reap(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

static void testRefusesWhileRunningThenReapsOnce()
{
    std::printf("[test] WorkerSlot refuses while running, reaps once\n");
    WorkerSlot slot;
    std::atomic_bool release{false};
    std::atomic_int runs{0};
    CHECK(!slot.busy());
    CHECK(slot.start([&](const CancelToken&) {
        while (!release.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++runs;
    }));
    CHECK(slot.busy());
    CHECK(!slot.start([&](const CancelToken&) { ++runs; }));
    CHECK(!slot.reap());
    release = true;
    waitReap(slot);
    CHECK(!slot.busy());
    CHECK(!slot.reap());
    CHECK(runs.load() == 1);
    std::printf("[test] WorkerSlot refuses while running, reaps once OK\n");
}

static void testFreshTokenAndRestartAfterReap()
{
    std::printf("[test] WorkerSlot fresh token per run\n");
    WorkerSlot slot;
    std::atomic_bool firstSawCancel{false};
    std::atomic_bool secondSawCancel{true};
    CHECK(slot.start([&](const CancelToken& t) { firstSawCancel = t->load(); }));
    slot.cancel(); // cancelling a finished/finishing run must not poison the next
    waitReap(slot);
    CHECK(slot.start([&](const CancelToken& t) { secondSawCancel = t->load(); }));
    waitReap(slot);
    CHECK(!secondSawCancel.load());
    std::printf("[test] WorkerSlot fresh token per run OK\n");
}

static void testStartReclaimsFinishedRunWithoutReap()
{
    std::printf("[test] WorkerSlot start reclaims finished run\n");
    WorkerSlot slot;
    std::atomic_int runs{0};
    CHECK(slot.start([&](const CancelToken&) { ++runs; }));
    for (int i = 0; i < 5000 && runs.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // Completion publishes just after fn returns; retry until reclaimed.
    bool restarted = false;
    for (int i = 0; i < 5000 && !restarted; ++i) {
        restarted = slot.start([&](const CancelToken&) { ++runs; });
        if (!restarted)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(restarted);
    waitReap(slot);
    CHECK(runs.load() == 2);
    std::printf("[test] WorkerSlot start reclaims finished run OK\n");
}

static void testCancelIsNonBlockingAndDestructorJoins()
{
    std::printf("[test] WorkerSlot cancel non-blocking, destructor joins\n");
    std::atomic_bool exited{false};
    std::atomic_bool started{false};
    {
        WorkerSlot slot;
        CHECK(slot.start([&](const CancelToken& t) {
            started = true;
            while (!t->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            exited = true;
        }));
        while (!started.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        slot.cancel();
        CHECK(slot.busy()); // cancel() returned without joining
    }
    CHECK(exited.load()); // destructor cancelled and joined
    std::printf("[test] WorkerSlot cancel non-blocking, destructor joins OK\n");
}

int main()
{
    testRefusesWhileRunningThenReapsOnce();
    testFreshTokenAndRestartAfterReap();
    testStartReclaimsFinishedRunWithoutReap();
    testCancelIsNonBlockingAndDestructorJoins();
    return miyoofin_test::finish("worker_slot");
}
