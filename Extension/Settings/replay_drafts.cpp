#include "replay_drafts.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/media_catalog.h"
#include "Extension/Boot/user_data_redirect.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace dingosdk::replay_drafts {
namespace {
using Address = std::uintptr_t;
namespace mc = addr::media_catalog;
constexpr char user_id[] = "reskate";
enum class Stage { waiting, loading, done, failed };
Stage stage{};
ULONGLONG signed_in_at{};
Address watched{};
std::size_t entries_seen{};

bool matches(Address base, const game::build::Fingerprint& fingerprint) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek(base + fingerprint.rva, bytes) && bytes == fingerprint.bytes;
}
Address pointer(Address at) noexcept {
    Address value{};
    return memory::peek(at, value) ? value : 0;
}
// An engine string: sixteen bytes, the text inline unless the last byte's top bit says heap.
std::string engine_string(Address at) {
    std::array<char, 16> raw{};
    if (!memory::peek(at, raw)) return {};
    if (!(raw[15] & 0x80)) return std::string(raw.data(), strnlen(raw.data(), 15));
    Address heap{};
    std::memcpy(&heap, raw.data(), sizeof(heap));
    std::array<char, 200> text{};
    return memory::peek_cstring(heap, text.data(), text.size()) > 0 ? std::string(text.data()) : std::string{};
}
// What a signed-in user is to the catalogue: an id at + 0x18.
struct LocalUser {
    std::array<unsigned char, 0x18> head{};
    std::array<char, 16> id{};
    std::array<unsigned char, 0x18> tail{};
};
// The game's calls, with a fault in them kept from taking the game down: drafts then stay
// as they were (unlisted).
__declspec(noinline) bool call_add_container(Address base, Address services) noexcept {
    __try {
        reinterpret_cast<Address (*)(Address)>(base + mc::add_container.rva)(services);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool call_user_added(Address base, Address listener, LocalUser** user) noexcept {
    __try {
        reinterpret_cast<void (*)(Address, LocalUser**)>(base + mc::user_added.rva)(listener, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Address container_of(Address base, Address services) noexcept {
    const auto container = reinterpret_cast<Address (*)(Address)>(base + mc::lookup.rva)(services);
    return container && pointer(container) == base + mc::container_vtable &&
           pointer(container + mc::container_listener) == base + mc::listener_vtable ? container : 0;
}
void fail(const char* why) {
    stage = Stage::failed;
    logging::log(logging::Level::warning, logging::Channel::runtime, "Replay drafts: {}; drafts stay unavailable.", why);
}

// A draft's footage is gone when the game closes, and the game lists drafts from what was
// saved this session, so the thumbnails of earlier sessions (about 2 MB each) only pile up.
// They are removed before the catalogue is made. Only a numbered folder whose manifest is a
// replay's (one source.png and a replay length) goes: anything else in the catalogue stays.
void remove_old_thumbnails() {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto folder = redirected_local_app_data() / L"Skate" / L"data" / L"catalog" / std::wstring(user_id, user_id + sizeof(user_id) - 1);
    if (!fs::is_directory(folder, error)) return;
    unsigned removed{};
    std::uintmax_t bytes{};
    for (const auto& item : fs::directory_iterator(folder, error)) {
        const auto name = item.path().filename().wstring();
        if (!item.is_directory(error) || name.empty() || !std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            continue;
        try {
            std::ifstream file(item.path() / L"manifest.json", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const auto manifest = Json::parse(text);
            const auto& files = manifest.at("files");
            if (!files.is_object() || files.size() != 1 || !files.contains("source.png") || !manifest.at("context").contains("v_dur_i"))
                continue;
        } catch (...) { continue; }
        std::uintmax_t size{};
        for (const auto& inside : fs::directory_iterator(item.path(), error))
            if (inside.is_regular_file(error)) size += inside.file_size(error);
        if (fs::remove_all(item.path(), error) && !error) { ++removed; bytes += size; }
    }
    if (removed)
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Replay drafts: {} thumbnail(s) of earlier sessions' drafts removed ({:.1f} MB).", removed, bytes / 1048576.0);
}

void sign_in(Address base) {
    if (!matches(base, mc::lookup) || !matches(base, mc::add_container) || !matches(base, mc::user_added))
        return fail("this build's media catalogue differs");
    const auto services = pointer(base + addr::engine::backend_services);
    if (!services) return; // not made yet
    if (pointer(services) != base + mc::services_vtable) return fail("the backend services are not the game's own");
    auto container = container_of(base, services);
    if (!container) {
        if (!call_add_container(base, services)) return fail("the game faulted registering its media catalogue");
        container = container_of(base, services);
        if (!container) return fail("the game did not register its media catalogue");
    }
    if (pointer(container + mc::container_catalogue)) {
        // A user the game signed in itself: its catalogue is the one to use.
        stage = Stage::done;
        logging::write(logging::Level::info, logging::Channel::runtime, "Replay drafts: the game already has a media catalogue.");
        return;
    }
    try { remove_old_thumbnails(); } catch (...) {}
    static LocalUser user;
    std::memcpy(user.id.data(), user_id, sizeof(user_id));
    user.id[15] = static_cast<char>(15 - (sizeof(user_id) - 1));
    auto* held = &user;
    if (!call_user_added(base, container + mc::container_listener, &held)) return fail("the game faulted making its media catalogue");
    if (!pointer(container + mc::container_catalogue)) return fail("the game did not make its media catalogue");
    stage = Stage::loading;
    signed_in_at = GetTickCount64();
}

// The catalogue's first load ends in a task the services post to themselves. Nothing may be
// running those offline, so after a while the flag is set here: entries of earlier sessions
// are then not listed, which costs nothing (their footage went with that session).
void await_load(Address base) {
    const auto services = pointer(base + addr::engine::backend_services);
    const auto container = services ? container_of(base, services) : 0;
    const auto catalogue = container ? pointer(container + mc::container_catalogue) : 0;
    if (!catalogue) return fail("the media catalogue went away");
    std::uint8_t ready{};
    if (!memory::peek(catalogue + mc::catalogue_ready, ready)) return fail("the media catalogue cannot be read");
    const bool waited = GetTickCount64() - signed_in_at > 5000;
    if (!ready && !waited) return;
    if (!ready) *reinterpret_cast<std::uint8_t*>(catalogue + mc::catalogue_ready) = 1;
    const auto first = pointer(catalogue + mc::catalogue_entries), last = pointer(catalogue + mc::catalogue_entries + 8);
    stage = Stage::done;
    watched = catalogue;
    entries_seen = last >= first ? (last - first) / mc::catalogue_entry_size : 0;
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Replay drafts: media catalogue ready at {} with {} entries{}.", engine_string(catalogue + mc::catalogue_folder),
        last >= first ? (last - first) / mc::catalogue_entry_size : 0,
        ready ? "" : " (its load never reported back; marked ready)");
}
// Says when an entry is made or removed: a saved draft is one more, a deleted one is one fewer.
void watch_entries(Address base) {
    const auto services = pointer(base + addr::engine::backend_services);
    const auto container = services ? container_of(base, services) : 0;
    if (!container || pointer(container + mc::container_catalogue) != watched) { watched = 0; return; }
    const auto first = pointer(watched + mc::catalogue_entries), last = pointer(watched + mc::catalogue_entries + 8);
    const std::size_t count = last >= first ? (last - first) / mc::catalogue_entry_size : 0;
    if (count == entries_seen) return;
    logging::log(logging::Level::info, logging::Channel::runtime, "Replay drafts: the media catalogue went from {} to {} entries.",
        entries_seen, count);
    entries_seen = count;
}
} // namespace

void on_client_tick(std::uintptr_t base, std::uint32_t state) noexcept {
    if (stage == Stage::done && watched && base) {
        try { watch_entries(base); } catch (...) { watched = 0; }
        return;
    }
    if (stage == Stage::done || stage == Stage::failed || !base) return;
    // In a level (13) or its menu (21): the services are long made by then.
    if (state != 13 && state != 21) return;
    try {
        if (stage == Stage::waiting) sign_in(base);
        else await_load(base);
    } catch (...) { stage = Stage::failed; }
}
} // namespace dingosdk::replay_drafts
