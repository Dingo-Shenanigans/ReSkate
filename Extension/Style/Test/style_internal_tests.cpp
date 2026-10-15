#include "Extension/Style/style_internal.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace dingosdk::style;
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
// Once done, the read's thread writes nothing more.
template <class T> bool ended(const ReadOnce<T> &read) {
    for (int i = 0; i < 500 && !read.done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return read.done();
}
ReadOnce<int> number, broken;
int reads{};
} // namespace

int style_internal_tests() {
    number.start("a number", [] { ++reads; return 7; });
    number.start("a number", [] { ++reads; return 8; });
    check(ended(number) && *number.value() == 7 && reads == 1 && !number.failed(), "a value is read once");
    broken.start("a broken number", []() -> int { throw std::runtime_error("the file is missing"); });
    check(ended(broken) && broken.failed() && !broken.value(), "a failed read has no value");
    if (!failures) std::cout << "style internal tests passed\n";
    return failures ? 1 : 0;
}
