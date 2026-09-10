#include "../tools/catalog_journal_benchmark.hpp"

#include <cstdio>

int main()
{
    std::string error;
    if (!miyoofin::runCatalogBenchmarkSelfTests(error)) {
        std::fprintf(stderr, "catalog journal benchmark self-test failed: %s\n",
                     error.c_str());
        return 1;
    }
    std::puts("catalog journal benchmark self-test passed");
    return 0;
}
