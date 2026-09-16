#include "test_support.hpp"
#include "cases/test_update.inc"
#include "cases/test_update_installer.inc"
#include "cases/test_update_manager.inc"
#include <curl/curl.h>

int main()
{
    std::setbuf(stdout, nullptr);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // UpdateVersion
    testParseSemVerBasic();
    testParseSemVerMalformed();
    testCompareSemVer();
    testIsNewerThan();

    // UpdateManifest
    testParseManifestValid();
    testParseManifestMissingVersion();
    testParseManifestBadSha();
    testParseManifestMissingSize();
    testParseManifestReorderedKeys();

    // Sha256
    testSha256Empty();
    testSha256Abc();
    testSha256MultiBlock();
    testSha256File();

    // UpdateInstallPlan
    testIsSafeTarEntry();
    testIsWhitelistedAppPath();
    testBuildInstallPlanBinaryLast();
    testBuildInstallPlanMissingBinary();
    testBuildInstallPlanUnsafeEntry();
    testBuildInstallPlanUserStateRejected();

    // AppDir
    testAppDir();

    // parseUpdateManifest — allowNonHttpsAssets
    testParseManifestRejectFileAsset();
    testParseManifestAcceptFileAssetDev();
    testParseManifestAcceptAbsolutePathAssetDev();
    testParseManifestRejectsFileAssetStrict();

    // ELF/ARM helpers
    testIsElfMagic();
    testIsElfArm();

    // downloadToFile
    testDownloadToFileBasic();
    testDownloadToFileProgress();
    testDownloadToFileCancel();
    testDownloadToFileResume();
    testDownloadToFileRedirect();
    testDownloadToFile404();

    // installUpdate
    testInstallUpdateBasic();
    testInstallUpdateUnsafeArchive();
    testInstallUpdateCancel();
    testInstallUpdateProgress();
    testInstallUpdateBackupByteIdentical();
    testInstallUpdateHardlinkRejected();

    // downloadToFile additional
    testDownloadToFile404NoCorruption();
    testDownloadToFileCompletePartSkipDownload();
    testDownloadToFileStaleResumeReset();

    // installUpdate additional
    testInstallUpdateRollbackOnFailure();

    // parseTarListingLine
    testParseTarListingLineBusyBoxRegular();
    testParseTarListingLineGNURegular();
    testParseTarListingLineBusyBoxDirectory();
    testParseTarListingLineSymlink();
    testParseTarListingLineHardlink();
    testParseTarListingLineUnparseable();
    testParseTarListingLinePathWithDatetimeSubstring();

    // UpdateManager
    testUpdateStatusText();
    testUpdateManagerInitialState();
    testUpdateManagerEnabled();
    testUpdateVersionDecision();
    testUpdateCheckErrorMessage();
    testResolveManifestSource();
    testIsLocalAsset();
    testLocalAssetPath();
    testDevOverrideActive();

    return miyoofin_test::finish("update");
}
