// Precompiled header: the standard-library, POSIX, SDL2 and libcurl headers
// that most translation units include. Parsing them was ~2s of every compile.
//
// System/third-party headers ONLY (no project headers), so this rarely changes
// and never hides a project dependency. It is force-included by the Makefiles
// (-include) and is optional: USE_PCH=0 builds without it, and GCC falls back
// to the plain header if a PCH is ever invalid (-Winvalid-pch reports it).
// Do not put macro definitions here; sources must see the same macro state
// with or without it.
#ifndef MIYOOFIN_PCH_HPP
#define MIYOOFIN_PCH_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <curl/curl.h>

#endif // MIYOOFIN_PCH_HPP
