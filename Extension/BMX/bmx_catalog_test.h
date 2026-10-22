#pragma once
#include "Engine/Core/Platform/user_data_paths.h"
#include "Engine/Game/BMX/bmx_trick_catalog.h"
#include <Windows.h>
#include <string_view>

namespace dingosdk::bmx::catalog_test {
// Process-local development rehearsal only. Selecting a reference slot is not
// evidence that native rider rendering or a clean landing actually completed.
inline bool configuration_selected(bool development, std::wstring_view profile,
                                   std::wstring_view value) noexcept {
    return development && profile == L"bmx-user-data-chassis-probe" && value == L"superman";
}
inline catalog::TrickId trick() noexcept {
    try {
        if (!user_data::development_requested()) return catalog::TrickId::none;
        const auto root = user_data::override_root();
        if (!root) return catalog::TrickId::none;
        wchar_t value[16]{};
        const auto size = GetEnvironmentVariableW(L"RESKATE_BMX_CATALOG_TEST", value, 16);
        if (!size || size >= 16) return catalog::TrickId::none;
        return configuration_selected(true, root->filename().wstring(), {value, size}) ?
            catalog::TrickId::superman : catalog::TrickId::none;
    } catch (...) { return catalog::TrickId::none; }
}
inline bool enabled() noexcept { return trick() != catalog::TrickId::none; }
}
