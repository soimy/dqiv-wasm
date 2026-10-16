#pragma once
// std::execution::par needs a parallel-algorithms backend: the MSVC STL ships
// one, libc++ (macOS, iOS, Android, wasm32) does not implement the parallel
// algorithms at all, and libstdc++ needs TBB to provide them.
// Keep the original behaviour where it exists and run sequentially elsewhere.
// Define DQIV_NO_PARALLEL to force sequential execution everywhere.
#include <algorithm>
#include <execution>

#if !defined(DQIV_NO_PARALLEL) && defined(_MSC_VER)
#define DQIV_PARALLEL_FOR_EACH(first, last, body) std::for_each(std::execution::par, first, last, body)
#else
#define DQIV_PARALLEL_FOR_EACH(first, last, body) std::for_each(first, last, body)
#endif
