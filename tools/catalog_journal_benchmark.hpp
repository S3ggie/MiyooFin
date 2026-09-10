#ifndef MIYOOFIN_CATALOG_JOURNAL_BENCHMARK_HPP
#define MIYOOFIN_CATALOG_JOURNAL_BENCHMARK_HPP

#include <string>

namespace miyoofin {

bool catalogBenchmarkProfileSupported(char profile) noexcept;
bool catalogBenchmarkRootIsIsolated(const std::string &root);
bool runCatalogBenchmarkSelfTests(std::string &error);
int runCatalogJournalBenchmark(int argc, char **argv);

} // namespace miyoofin

#endif // MIYOOFIN_CATALOG_JOURNAL_BENCHMARK_HPP
