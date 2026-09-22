#include "SyncState.hpp"
#include <cstdio>
#include <cstring>
namespace miyoofin {
namespace {}
std::string SyncStateStore::path(const std::string& r, const std::string& s)
{
    return r + "/library/" + s + "/sync-state.v1";
}
bool SyncStateStore::load(const std::string& p, SyncState& o, std::string* e)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) {
        if (e)
            *e = "not found";
        return false;
    }
    char magic[5] = {};
    long long successful = 0, reconcile = 0;
    bool ok = fscanf(f, "%4s %lld %lld", magic, &successful, &reconcile) == 3 &&
              !strcmp(magic, "MFS1") && successful >= 0 && reconcile >= 0 && fgetc(f) == '\n' &&
              fgetc(f) == EOF;
    fclose(f);
    if (!ok) {
        if (e)
            *e = "invalid sync state";
        return false;
    }
    o.lastSuccessfulMs = successful;
    o.lastReconcileMs = reconcile;
    return true;
}
}
