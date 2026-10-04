#include "test_support.hpp"
#include "cases/test_update_installer.inc"
#include <curl/curl.h>

int main()
{
    std::setbuf(stdout, nullptr);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // HttpClient::downloadToFile and installUpdate
    testIsElfMagic();
    testIsElfArm();
    testDownloadToFileBasic();
    testDownloadToFileProgress();
    testDownloadToFileCancel();
    testDownloadToFileResume();
    testDownloadToFileRedirect();
    testDownloadToFile404();
    testInstallUpdateBasic();
    testInstallUpdateUnsafeArchive();
    testInstallUpdateCancel();
    testInstallUpdateProgress();
    testInstallUpdateBackupByteIdentical();
    testInstallUpdateHardlinkRejected();
    testDownloadToFile404NoCorruption();
    testDownloadToFileCompletePartSkipDownload();
    testDownloadToFileStaleResumeReset();
    testInstallUpdateRollbackOnFailure();
    testInstallUpdateDotComponentsCannotReachUserState();
    testInstallUpdateRejectsSpecialFiles();
    testManifestHostAndVersionValidation();
    testParseTarListingLineBusyBoxRegular();
    testParseTarListingLineGNURegular();
    testParseTarListingLineBusyBoxDirectory();
    testParseTarListingLineSymlink();
    testParseTarListingLineHardlink();
    testParseTarListingLineUnparseable();
    testParseTarListingLinePathWithDatetimeSubstring();
    testExecutableInstallPathsCoverWhitelist();
    testInstallUpdateReporterExecutable();
    return miyoofin_test::finish("update_installer");
}
