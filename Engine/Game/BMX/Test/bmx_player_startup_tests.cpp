#include "Extension/BMX/bmx_player_startup_guard.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

namespace {
void check(bool valid, const char* text) {
    if (!valid) { std::cerr << text << '\n'; std::exit(1); }
}
struct Fixture {
    static constexpr std::uintptr_t object = 0x10000, record = 0x20000, fields = 0x30000;
    static constexpr std::uintptr_t array = 0x40000, array_record = 0x50000, native_bool = 0x140765d8e8;
    std::map<std::uintptr_t, unsigned char> bytes;
    std::uintptr_t mutate_after_read{};
    template<class T> void put(std::uintptr_t address, const T& value) {
        const auto* raw = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) bytes[address + i] = raw[i];
    }
    bool read(std::uintptr_t address, void* target, std::size_t size) {
        auto* raw = static_cast<unsigned char*>(target);
        for (std::size_t i = 0; i < size; ++i) {
            const auto found = bytes.find(address + i);
            if (found == bytes.end()) return false;
            raw[i] = found->second;
        }
        if (address == mutate_after_read) {
            mutate_after_read = 0;
            put<std::uint16_t>(record + 6, 8);
        }
        return true;
    }
    bool valid() {
        return dingosdk::bmx::startup_detail::event_type_contract(object, native_bool,
            [this](std::uintptr_t address, void* target, std::size_t size) { return read(address, target, size); });
    }
    Fixture() {
        using namespace dingosdk::bmx::startup_detail;
        put(object, record); put(record, event_hash); put<std::uint16_t>(record + 4, 2 << 5);
        put<std::uint16_t>(record + 6, 1); put(record + 8, event_guid);
        put<std::uint16_t>(record + 0x2a, 1); put(record + 0x60, fields);
        put(fields, force_field_hash); put<std::uint16_t>(fields + 8, 0); put(fields + 0x10, native_bool);
        put(record + 0x20, array); put(array, array_record); put(array_record + 0x30, object);
    }
};
}
int main() {
    using namespace dingosdk::bmx::startup_detail;
    Fixture f;
    check(f.valid(), "exact GUID/hash/one-bool reflected event contract accepts");
    f.put<std::uint16_t>(Fixture::record + 6, 8); check(!f.valid(), "payload size drift rejects");
    f = Fixture{}; f.put<std::uint16_t>(Fixture::fields + 8, 1); check(!f.valid(), "field offset drift rejects");
    f = Fixture{}; f.put<std::uintptr_t>(Fixture::fields + 0x10, Fixture::native_bool + 8); check(!f.valid(), "lookalike primitive type rejects");
    f = Fixture{}; auto wrong_guid = event_guid; wrong_guid[15] ^= 1; f.put(Fixture::record + 8, wrong_guid); check(!f.valid(), "runtime GUID drift rejects");
    f = Fixture{}; f.put<std::uint16_t>(Fixture::record + 0x2a, 2); check(!f.valid(), "additional payload fields reject");
    f = Fixture{}; f.put<std::uintptr_t>(Fixture::array_record + 0x30, Fixture::object + 8); check(!f.valid(), "array backlink identity rejects");
    f = Fixture{}; f.bytes.erase(Fixture::record + 0x2a); check(!f.valid(), "unreadable metadata rejects");
    f = Fixture{}; f.mutate_after_read = Fixture::fields + 0x10; check(!f.valid(), "metadata recycled during validation rejects");
    check(recovery_world("Levels/Game/DingoLevel_Root/DingoLevel_Root", "levels/game/BAM_LevelRoot/BAM_LevelRoot"), "actual city description accepts");
    check(recovery_world("Levels/Game/DingoLevel_Root/DingoLevel_Root", "Levels/Game/DingoLevel_MPR/DingoLevel_MPR"), "actual MPR description accepts");
    check(!recovery_world("Levels/Game/DingoLevel_Splash/DingoLevel_Splash", ""), "splash rejects despite ready client phase");
    check(!recovery_world("Levels/Game/DingoLevel_Root/DingoLevel_Root", ""), "root without detached world rejects");
    check(!recovery_world("levels/bam_levelroot/bam_levelroot", ""), "incomplete earlier path spelling rejects");
    AttemptGate gate;
    WorldKey key{1,2,3,4,0};
    check(gate.consume(key), "first explicit attempt accepts");
    check(!gate.consume(key), "same loaded world duplicate rejects");
    ++key.generation; check(gate.consume(key), "observed loading generation re-arms");
    --key.generation; check(!gate.consume(key), "return to prior world identity cannot duplicate");
    key.root = 5; check(gate.consume(key), "new native root identity can receive one request");
    key.player = 0; check(!gate.consume(key), "missing canonical player rejects");
    std::cout << "BMX initial-player recovery guard tests passed\n";
}
