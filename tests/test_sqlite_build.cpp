#include <cstdio>
#include <cstring>

#include "sqlite3.h"

namespace {

int failures = 0;

void check(bool condition, const char *description)
{
    if (!condition) {
        std::printf("FAIL: %s\n", description);
        ++failures;
    }
}

void checkCompileOption(const char *option)
{
    char description[128];
    std::snprintf(description, sizeof(description), "compile option %s", option);
    check(sqlite3_compileoption_used(option) != 0, description);
}

} // namespace

int main()
{
    check(SQLITE_VERSION_NUMBER == 3053004, "header version number is 3053004");
    check(std::strcmp(SQLITE_VERSION, "3.53.4") == 0, "header version is 3.53.4");
    check(sqlite3_libversion_number() == 3053004, "runtime version number is 3053004");
    check(std::strcmp(sqlite3_libversion(), "3.53.4") == 0, "runtime version is 3.53.4");
    check(sqlite3_threadsafe() == 2, "runtime thread-safety mode is 2");

    checkCompileOption("THREADSAFE=2");
    checkCompileOption("DEFAULT_MEMSTATUS=0");
    checkCompileOption("DQS=0");
    checkCompileOption("OMIT_LOAD_EXTENSION");

    if (failures == 0) {
        std::printf("SQLite 3.53.4 build/configuration checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
