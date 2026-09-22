#include "AppDir.hpp"
#include <cstring>
#include <unistd.h>

namespace miyoofin {

bool appDir(std::string& out)
{
    char buf[4096];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        // dirname: find last '/'
        char* slash = std::strrchr(buf, '/');
        if (slash) {
            *slash = '\0';
            out = buf;
            return true;
        }
    }

    // Fallback to cwd
    char* cwd = ::getcwd(buf, sizeof(buf));
    if (cwd) {
        out = cwd;
        return true;
    }

    return false;
}

} // namespace miyoofin
