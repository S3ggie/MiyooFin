#include "Session.hpp"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace miyoofin {

static const char *DEFAULT_PATH = "session.txt";

// -------------------------------------------------------------------
// Simple key=value line-based serialisation.
// -------------------------------------------------------------------

static void writeLine(FILE *f, const char *key, const std::string &val)
{
    if (!val.empty()) {
        fprintf(f, "%s=%s\n", key, val.c_str());
    }
}

static std::string readValue(const std::string &line, const char *key)
{
    size_t klen = std::strlen(key);
    if (line.size() > klen && line.compare(0, klen, key) == 0 && line[klen] == '=') {
        return line.substr(klen + 1);
    }
    return {};
}

// -------------------------------------------------------------------
// Save to an explicit path.
// -------------------------------------------------------------------
bool Session::saveTo(const std::string &path) const
{
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return false;

    writeLine(f, "server_url",  serverUrl);
    writeLine(f, "access_token", accessToken);
    writeLine(f, "user_id",     userId);
    writeLine(f, "user_name",   userName);
    writeLine(f, "device_id",   deviceId);

    std::fclose(f);

    // Restrict permissions on POSIX systems
    ::chmod(path.c_str(), 0600);

    return true;
}

// -------------------------------------------------------------------
// Load from an explicit path.
// -------------------------------------------------------------------
Session Session::loadFrom(const std::string &path)
{
    Session s;
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return s;

    char buf[1024];
    while (std::fgets(buf, sizeof(buf), f)) {
        // Trim trailing newline/CR
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        std::string line(buf);

        std::string v;
        if (!(v = readValue(line, "server_url")).empty())    s.serverUrl   = v;
        if (!(v = readValue(line, "access_token")).empty())  s.accessToken = v;
        if (!(v = readValue(line, "user_id")).empty())       s.userId      = v;
        if (!(v = readValue(line, "user_name")).empty())     s.userName    = v;
        if (!(v = readValue(line, "device_id")).empty())     s.deviceId    = v;
    }

    std::fclose(f);
    return s;
}

// -------------------------------------------------------------------
// Default path wrappers.
// -------------------------------------------------------------------
bool Session::save() const
{
    return saveTo(DEFAULT_PATH);
}

Session Session::load()
{
    return loadFrom(DEFAULT_PATH);
}

bool Session::remove()
{
    return std::remove(DEFAULT_PATH) == 0;
}

} // namespace miyoofin