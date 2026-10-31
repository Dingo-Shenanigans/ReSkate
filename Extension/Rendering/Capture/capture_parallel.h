#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <future>
#include <thread>

namespace dingosdk::capture {
// A small bounded fan-out for large CPU images. Each task owns disjoint rows;
// all work finishes before returning, so no frame or mapped GPU memory escapes.
template <class Function> void parallel_rows(std::uint32_t height, std::uint32_t width, Function function) {
    static const auto available = std::max(1u, std::thread::hardware_concurrency());
    const auto lanes = std::uint64_t{height} * width >= 262144 ? std::min(4u, available) : 1u;
    std::array<std::future<void>, 3> tasks;
    for (std::uint32_t lane = 1; lane < lanes; ++lane) {
        const auto first = height * lane / lanes, last = height * (lane + 1) / lanes;
        tasks[lane - 1] = std::async(std::launch::async, [=, &function] { function(first, last); });
    }
    function(0, height / lanes);
    for (std::uint32_t lane = 1; lane < lanes; ++lane)
        tasks[lane - 1].get();
}
} // namespace dingosdk::capture
