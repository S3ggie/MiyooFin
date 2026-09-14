#pragma once

namespace miyoofin {

/// Per-command failure-injection spec used by host tests to exercise
/// cancellation and error paths without globals. In device builds the type
/// carries no state and always reports disabled values, so the production
/// command structs stay free of test-only fields.
struct CatalogDbFailureSpec {
#ifdef MIYOOFIN_TEST_BUILD
    int failAfterRows = -1;
    int cancelAfterRows = -1;
    int failAfterWrites = -1;

    static CatalogDbFailureSpec disabled() { return {}; }
#else
    static constexpr int failAfterRows = -1;
    static constexpr int cancelAfterRows = -1;
    static constexpr int failAfterWrites = -1;

    static CatalogDbFailureSpec disabled() { return {}; }
#endif
};

} // namespace miyoofin
