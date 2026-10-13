// A test codec exercises DLL discovery without the game's proprietary Oodle DLL.
#include "Engine/Resource/cas_codec.h"
#include <Windows.h>
#include <array>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Pass the directory containing the test Oodle DLL");
        if (GetModuleHandleW(L"oo2core_9_win64.dll"))
            throw std::runtime_error("The fixture must exercise discovery before Oodle is loaded");
        // One byte in an Oodle CAS block. The fixture codec returns that byte unchanged.
        constexpr std::array encoded{
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
            std::byte{0x11}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x2a}};
        const auto decoded = dingosdk::frostbite::decode_cas(encoded, {std::filesystem::path(argv[1])});
        if (decoded.size() != 1 || decoded.front() != std::byte{0x2a})
            throw std::runtime_error("The test codec did not decode the CAS byte");
        std::cout << "Root Oodle fixture loaded and CAS byte decoded\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
