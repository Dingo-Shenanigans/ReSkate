#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/module_path.h"
#include "Engine/Core/Platform/path_text.h"
#include <array>
#include <filesystem>
#include <iostream>

namespace {
int failures{};
void check(bool ok, const char *what) {
    if (ok) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
} // namespace

int main() {
    const auto folder = dingosdk::executable_folder();
    check(std::filesystem::exists(folder / "dingosdk_platform_tests.exe"), "the executable folder holds the running executable");
    check(dingosdk::windows_device_name("CON") && dingosdk::windows_device_name("nul") && dingosdk::windows_device_name("Com1") &&
              dingosdk::windows_device_name("lpt0") && dingosdk::windows_device_name("LPT9"),
          "device names, in any case");
    check(!dingosdk::windows_device_name("console") && !dingosdk::windows_device_name("com10") &&
              !dingosdk::windows_device_name("comx") && !dingosdk::windows_device_name("co") && !dingosdk::windows_device_name(""),
          "other names are not device names");
    {
        std::array<float, 3> target{};
        const std::array<float, 3> value{1, 2, 3};
        const auto at = reinterpret_cast<std::uintptr_t>(target.data());
        check(dingosdk::memory::poke(at, value) && target == value, "a write lands");
        check(!dingosdk::memory::poke(0x1000, value), "a low address is refused");
        check(!dingosdk::memory::poke(dingosdk::memory::highest_user_address - 4, value), "an address past the top is refused");
        check(!dingosdk::memory::poke_bytes(at, nullptr, 4) && !dingosdk::memory::poke_bytes(at, &value, 0), "no source or no size");
        void *page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
        check(page && !dingosdk::memory::poke(reinterpret_cast<std::uintptr_t>(page), value), "a read-only page is not written");
        if (page) VirtualFree(page, 0, MEM_RELEASE);
    }
    {
        using dingosdk::memory::peek_pointer, dingosdk::memory::highest_user_address;
        std::array<std::uintptr_t, 3> holder{};
        const auto object = reinterpret_cast<std::uintptr_t>(holder.data());
        holder[1] = object;
        check(peek_pointer(object, sizeof(std::uintptr_t)) == object, "a stored pointer is read");
        check(!peek_pointer(0x1000, 0) && !peek_pointer(0, sizeof(std::uintptr_t)), "an object below 64 KB is refused");
        for (const std::uintptr_t stored : {std::uintptr_t{0}, std::uintptr_t{0x1000}, highest_user_address - 0x8000}) {
            holder[1] = stored;
            check(!peek_pointer(object, sizeof(std::uintptr_t)), "a stored 0, low value or value in the top 64 KB is refused");
        }
        check(!peek_pointer(object, highest_user_address), "an object plus an offset that ends past the top is refused");
        holder[0] = object;
        check(!peek_pointer(object + sizeof(std::uintptr_t), ~std::uintptr_t{} - 7), "an offset that wraps the address is refused");
        void *page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
        check(page && !peek_pointer(reinterpret_cast<std::uintptr_t>(page)), "an unreadable page gives 0");
        if (page) VirtualFree(page, 0, MEM_RELEASE);
    }
    if (!failures) std::cout << "platform tests passed\n";
    return failures ? 1 : 0;
}
