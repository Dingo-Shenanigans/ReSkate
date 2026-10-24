// What a session's host shares beyond its tuning (trainer_session.h): that it survives the
// trip to a guest, that nothing is sent for the game's own, and that bytes which are not an
// encoding, or values no trainer would hold, do not reach the game.
#include "Extension/Trainer/trainer_classes.h"
#include "Extension/Trainer/trainer_session.h"
#include "Engine/Game/Multiplayer/session_physics.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace dingosdk::trainer;
int failures = 0;
void check(bool condition, const std::string &message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
// The encoding's layout, for the cases that break it: version, table, six numbers, count, rows.
constexpr std::size_t table_at = 1, numbers_at = 5, count_at = 29, rows_at = 31;
constexpr std::size_t header_at_split = 33; // version 2: Underflip speed at 29, then the under part byte
template <class T> void poke(std::vector<std::uint8_t> &bytes, std::size_t at, T value) { std::memcpy(bytes.data() + at, &value, sizeof(T)); }
std::pair<std::uint16_t, float> row(std::size_t index, float value) { return {static_cast<std::uint16_t>(index), value}; }
}

int main() {
    check(encode_session_extras({}).empty(), "the game's own takes no bytes");
    const auto nothing = decode_session_extras({});
    check(nothing && nothing->stock(), "no bytes are the game's own");

    SessionExtras host;
    host.flip_speed = 0.5f;
    host.hippy_height = 3.0f;
    host.nocomply_height = 2.0f;
    host.boneless_height = 1.5f;
    host.offboard_height = 4.0f;
    host.cruise = 8.5f;
    host.classes = {row(0, 2.5f), row(7, -1.0f), row(class_field_count - 1, 12.0f)};
    const auto bytes = encode_session_extras(host);
    check(!bytes.empty() && bytes.size() <= dingosdk::max_physics_extras, "a host's extras fit in a session's");
    const auto guest = decode_session_extras(bytes);
    check(guest && *guest == host, "a guest reads what the host wrote");

    // Every class value changed still fits.
    SessionExtras everything;
    for (std::size_t i = 0; i < class_field_count; ++i) everything.classes.push_back(row(i, static_cast<float>(i) + 0.25f));
    const auto full = encode_session_extras(everything);
    check(full.size() <= dingosdk::max_physics_extras, "every class value fits");
    const auto all = decode_session_extras(full);
    check(all && all->classes.size() == class_field_count && all->classes.back().second == static_cast<float>(class_field_count - 1) + 0.25f,
          "every class value arrives");

    // The writer puts the rows in order and drops what is not a field.
    SessionExtras untidy;
    untidy.classes = {row(9, 1.0f), row(3, 2.0f), row(9, 5.0f), row(class_field_count, 1.0f), row(4, std::numeric_limits<float>::quiet_NaN())};
    const auto tidy = decode_session_extras(encode_session_extras(untidy));
    check(tidy && tidy->classes.size() == 2 && tidy->classes[0].first == 3 && tidy->classes[1].first == 9,
          "rows are written in order, once each, and only for real fields");

    // Only the multipliers changed: no rows.
    SessionExtras heights;
    heights.boneless_height = 2.0f;
    const auto only = decode_session_extras(encode_session_extras(heights));
    check(only && *only == heights && encode_session_extras(heights).size() == rows_at, "multipliers alone travel without rows");

    // Underflip speed: version 2 only when it is not 1, so an older guest still reads a host who leaves it alone.
    check(bytes[0] == 1, "a host with the game's own underflip speed writes version 1");
    SessionExtras underflip = host;
    underflip.underflip_speed = 0.25f;
    const auto underflip_bytes = encode_session_extras(underflip);
    check(underflip_bytes[0] == 2 && underflip_bytes.size() == bytes.size() + 5, "a changed underflip speed writes version 2, 5 bytes longer");
    const auto underflip_guest = decode_session_extras(underflip_bytes);
    check(underflip_guest && *underflip_guest == underflip, "a guest reads the host's underflip speed");
    SessionExtras split = host;
    split.underflip_split = true;
    const auto split_bytes = encode_session_extras(split);
    const auto split_guest = decode_session_extras(split_bytes);
    check(split_bytes[0] == 2 && split_guest && *split_guest == split, "the under part only setting alone travels too (version 2)");
    {
        auto odd = split_bytes;
        odd[header_at_split] = 2;
        check(!decode_session_extras(odd), "an under part byte that is not 0 or 1 is refused");
    }
    SessionExtras underflip_only;
    underflip_only.underflip_speed = 3.0f;
    check(!underflip_only.stock() && !encode_session_extras(underflip_only).empty(), "an underflip speed alone is not the game's own");
    check(!decode_session_extras(std::vector<std::uint8_t>(underflip_bytes.begin(), underflip_bytes.end() - 1)), "a version 2 encoding cut short is refused");

    // Not an encoding.
    const auto refused = [&](std::vector<std::uint8_t> broken) { return !decode_session_extras(broken); };
    check(refused({1}), "a lone byte is refused");
    check(refused(std::vector<std::uint8_t>(bytes.begin(), bytes.end() - 1)), "a short row is refused");
    {
        auto more = bytes;
        more.push_back(0);
        check(refused(more), "bytes after the last row are refused");
        auto version = bytes;
        version[0] = 2;
        check(refused(version), "another version is refused");
        auto count = bytes;
        poke(count, count_at, std::uint16_t{4});
        check(refused(count), "a count that does not match the rows is refused");
        auto order = bytes;
        poke(order, rows_at + 6, std::uint16_t{0}); // the second row's index, now the first's
        check(refused(order), "rows out of order are refused");
        auto beyond = bytes;
        poke<std::uint16_t>(beyond, rows_at + 12, static_cast<std::uint16_t>(class_field_count));
        check(refused(beyond), "a row for a field the table does not have is refused");
        auto nan = bytes;
        poke<float>(nan, rows_at + 2, std::numeric_limits<float>::quiet_NaN());
        check(refused(nan), "a value that is not a number is refused");
        auto infinite = bytes;
        poke<float>(infinite, numbers_at, std::numeric_limits<float>::infinity());
        check(refused(infinite), "a multiplier that is not a number is refused");
        check(refused(std::vector<std::uint8_t>(dingosdk::max_physics_extras + 1, 1)), "more than a session carries is refused");
    }

    // Values no trainer holds are brought into range rather than handed to the game.
    {
        auto wild = bytes;
        poke<float>(wild, numbers_at, 5000.0f);        // flip speed (past flip_high)
        poke<float>(wild, numbers_at + 4, 1.0e9f);     // hippy jump height
        poke<float>(wild, numbers_at + 8, -3.0f);      // no comply height
        poke<float>(wild, numbers_at + 20, 5000.0f);   // auto push speed
        poke<float>(wild, rows_at + 2, 1.0e12f);       // a class value
        const auto held = decode_session_extras(wild);
        check(held && held->flip_speed == flip_high && held->hippy_height == height_high && held->nocomply_height == height_low &&
                  held->cruise == 0.0f && held->classes[0].second == 1.0e6f,
              "multipliers and values are held to what the trainer allows");
    }

    // A host on another game build's table: the multipliers still apply, the rows mean nothing.
    {
        auto other = bytes;
        poke<std::uint32_t>(other, table_at, class_table_id() + 1);
        const auto shared = decode_session_extras(other);
        check(shared && shared->classes.empty() && shared->hippy_height == host.hippy_height && shared->cruise == host.cruise,
              "another table's rows are left out, the multipliers kept");
    }

    if (failures == 0) std::cout << "trainer session extras tests passed\n";
    return failures == 0 ? 0 : 1;
}
