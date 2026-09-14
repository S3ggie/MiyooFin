#include "test_support.hpp"

static std::string readFixture(const std::string &path)
{
    std::string out;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return out;
    char buffer[128];
    std::size_t bytes = 0;
    while ((bytes = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        out.append(buffer, bytes);
    std::fclose(file);
    return out;
}

#include "cases/test_downloads.inc"

int main()
{
    testDownloadInterruptStates(); testHlsDownloadStore();
    testDownloadRestartPersistence(); testDownloadsUiHelpers();
    testDownloadHierarchy(); testDownloadSourceReconciliation();
    testDownloadPlanBatchAccounting(); testHlsSizeEstimates();
    testHlsFailureClassification(); testStartupReconcileSkip();
    testCatalogDbHierarchyPlanning();
    return miyoofin_test::finish("downloads");
}
