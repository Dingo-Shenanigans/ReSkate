#include "Engine/Game/Skater/source_inventory.h"
#include <cstdlib>
#include <iostream>
#include <map>

namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Memory {
    std::map<std::uintptr_t, unsigned char> bytes;
    std::uintptr_t mutate_after{};
    bool mutated{};
    template<class T> void put(std::uintptr_t address, const T& value) {
        const auto* raw = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = raw[i];
    }
    bool read(std::uintptr_t address, void* output, std::size_t size) {
        auto* raw = static_cast<unsigned char*>(output);
        for (std::size_t i = 0; i < size; ++i) {
            const auto found = bytes.find(address + i);
            if (found == bytes.end()) return false;
            raw[i] = found->second;
        }
        if (address == mutate_after && !mutated) {
            mutated = true;
            put<std::uintptr_t>(address, 0);
        }
        return true;
    }
};
using dingosdk::skater::SourceInventoryLayout;
constexpr std::uintptr_t client = 0x10000, context = 0x20000, fixture_manager = 0x30000;
constexpr std::uintptr_t entries = 0x40000, fixture_source = 0x50000, parent = 0x60000;
constexpr std::uintptr_t data = 0x70000, blueprint = 0x80000, fixture_name = 0x90000;
constexpr SourceInventoryLayout layout{0x140100000, 0xa0000, 0xb0000, 0x140200000, 0x140300000, 0x140400000};
Memory fixture() {
    Memory m;
    m.put(client, layout.client_vtable); m.put(client + 8, context);
    m.put<std::uint32_t>(layout.partition_offset_address, 0x80);
    m.put<std::uint32_t>(context + 0x80, 1);
    for (unsigned i = 0; i != 7; ++i) m.put<std::uintptr_t>(layout.managers_address + (7 + i) * 8, i ? 0 : fixture_manager);
    m.put(fixture_manager, layout.manager_vtable); m.put(fixture_manager + 0x18, context);
    m.put(fixture_manager + 0xa0, entries); m.put(fixture_manager + 0xa8, entries + 0x18);
    m.put(entries + 8, fixture_source); m.put(fixture_source, layout.skater_source_vtable);
    m.put(fixture_source + 0x20, context); m.put(fixture_source + 0x30, parent); m.put(fixture_source + 0x38, data);
    m.put<std::uint32_t>(fixture_source + 0x60, 255); m.put<std::uintptr_t>(fixture_source + 0x68, 0);
    m.put(parent + 0x20, context); m.put(data + 0xd0, blueprint | 4);
    m.put(data + 0xe0, std::array<std::uint8_t, 4>{0, 1, 0, 1});
    m.put(blueprint + 0x18, fixture_name);
    constexpr char text[] = "Gameplay/Characters/CharacterBlueprint_Physics_Skater_RSP_CAS";
    for (std::size_t i = 0; i < sizeof(text); ++i) m.put(fixture_name + i, text[i]);
    return m;
}
auto snapshot(Memory& m) {
    return dingosdk::skater::read_client_skater_sources(client, layout,
        [&](std::uintptr_t at, void* out, std::size_t size) { return m.read(at, out, size); });
}
}
int main() {
    auto m = fixture();
    const auto unspawned = snapshot(m);
    check(unspawned.available && unspawned.sources.size() == 1, "Unspawned local entity prevented source enumeration");
    check(unspawned.managers == 1 && unspawned.entries == 1 && unspawned.partition == 1, "Source counts differ");
    const auto& s = unspawned.sources.front();
    check(!s.entity && s.blueprint == blueprint && s.context_matches && s.parent_context_matches && s.source_local_id == 255,
        "Source ownership, tagged blueprint, or source+60 local ID differs");
    check(s.blueprint_name.find("Skater_RSP_CAS") != std::string::npos && s.data_flags_e0[1] == 1,
        "Source metadata differs");
    m.put<std::uintptr_t>(fixture_source + 0x68, 0xc0000); m.put(0xc0000, layout.skater_entity_vtable);
    check(snapshot(m).sources.front().entity_type_matches, "Spawned entity was not observed");
    m = fixture(); m.put<std::uintptr_t>(fixture_manager + 0xa0, 0); m.put<std::uintptr_t>(fixture_manager + 0xa8, 0);
    check(snapshot(m).available && snapshot(m).sources.empty(), "Empty manager must remain diagnosable");
    m = fixture(); m.put<std::uintptr_t>(fixture_source + 0x38, 0);
    check(snapshot(m).available && !snapshot(m).sources.front().blueprint, "Unconfigured source must remain diagnosable");
    m = fixture(); m.put<std::uint32_t>(context + 0x80, 5);
    check(!snapshot(m).available, "Unsupported partition accepted");
    m = fixture(); m.put(fixture_manager + 0x18, parent);
    check(!snapshot(m).available, "Foreign source manager accepted");
    m = fixture(); m.put(fixture_manager + 0xa8, entries + 65 * 0x18);
    check(!snapshot(m).available, "Oversized source collection accepted");
    m = fixture(); m.put<std::uintptr_t>(entries + 8, 0x10);
    check(!snapshot(m).available, "Invalid source pointer accepted");
    m = fixture(); m.mutate_after = fixture_manager + 0xa0;
    const auto changing = snapshot(m);
    check(!changing.available && changing.sources.empty(), "Changing source collection accepted");
    m = fixture();
    for (unsigned i = 0; i < 256; ++i) m.put<char>(fixture_name + i, 'X');
    check(!snapshot(m).available, "Unterminated source blueprint name accepted");
    std::cout << "BMX pre-spawn source inventory checks passed\n";
}
