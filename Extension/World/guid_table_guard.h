#pragma once
#include <cstdint>

// Keeps the game's GUID-keyed record lookup (Engine/Game/Build/<build>/guid_table_lookup.h) from
// spinning forever on a key that is not in its table: the guard walks the table once, and on a
// miss logs the key and gets a handle with no items made, instead of freezing the game.
// A key that is there goes to the game's own lookup unchanged.
namespace dingosdk::guid_table_guard {
bool install(std::uintptr_t image_base) noexcept;
}
