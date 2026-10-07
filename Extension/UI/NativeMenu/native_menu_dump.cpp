#include "native_menu_dump.h"
#include "native_menu_data.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Abi/native_data.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

namespace dingosdk::multiplayer {
namespace {
using namespace menu_data;
constexpr unsigned max_depth = 24, max_elements = 64, max_nodes = 400000;
// Native value kinds used by the model schemas. Everything else prints as bytes.
constexpr unsigned kind_structure = 2, kind_reference = 3, kind_array = 4, kind_text = 7, kind_handle = 18;
constexpr std::uint32_t record_schema = 0x7aab0d43;

std::mutex request_mutex;
std::string request_what;
std::atomic<bool> request_pending{false};
std::atomic<std::uint64_t> request_due{0};

std::string hex(std::uint64_t value, int width) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(width), '0');
    for (int i = width - 1; i >= 0; --i, value >>= 4) out[static_cast<std::size_t>(i)] = digits[value & 15];
    return "0x" + out;
}
std::uint32_t schema_of(Address type) { return read<std::uint32_t>(read<Address>(type)); }

struct Walker {
    const Context& context;
    std::string out;
    std::set<std::pair<Address, Address>> visited;
    std::set<Handle> models;
    unsigned nodes{};

    void indent(unsigned depth) { out.append(std::size_t{depth} * 2 + 2, ' '); }
    void describe(Address type) {
        out += " <" + hex(schema_of(type), 8) + " k" + std::to_string(kind(type)) + ' ' +
            std::to_string(size(type)) + '>';
    }
    void bytes(Address value, unsigned length) {
        out += " = ";
        for (unsigned i = 0; i < std::min(length, 32U); ++i)
            out += hex(read<std::uint8_t>(value + i), 2).substr(2);
    }
    // An asset reference is one pointer. The referenced header carries its real
    // type at +8; named records keep their name at +0x28 and their typed data
    // at +0x20, other assets (widgets, styles, textures) at +0x18.
    void reference(Address value, unsigned depth) {
        const auto asset = read<Address>(value);
        if (!asset) { out += " = null\n"; return; }
        const auto actual = read<Address>(asset + 8);
        if (!actual || kind(actual) != kind_reference) { out += " = " + hex(asset, 16) + "\n"; return; }
        if (schema_of(actual) == record_schema) {
            out += " = record " + string(read<Address>(asset + 0x28), 256) + '\n';
            const auto type = read<Address>(asset + 0x18), data = read<Address>(asset + 0x20);
            if (!type || !data || depth >= max_depth) return;
            indent(depth + 1);
            out += "(record data)";
            describe(type);
            out += '\n';
            walk(type, data, depth + 2);
            return;
        }
        out += " = asset " + string(read<Address>(asset + 0x18), 256) + '\n';
    }
    void handle(Address value, unsigned depth) {
        const auto id = read<Handle>(value);
        out += " = " + hex(id, 16) + '\n';
        if (!id || depth >= max_depth || !models.insert(id).second) return;
        const auto type = context.type_of(id);
        if (!type) return;
        const auto data = game::native_data().models.value(context.manager, id, 0, 0);
        if (!data) return;
        indent(depth + 1);
        out += "(model) -> handle " + hex(id, 16);
        describe(type);
        out += '\n';
        walk(type, data, depth + 2);
    }
    void walk(Address type, Address value, unsigned depth) {
        if (!type || !value || depth > max_depth || ++nodes > max_nodes) return;
        if (!visited.emplace(type, value).second) return;
        try {
            const auto meta = read<Address>(type);
            switch (kind(type)) {
            case kind_structure: {
                const auto count = read<std::uint16_t>(meta + 0x2a);
                if (count > 128) return;
                const auto fields = read<Address>(meta + 0x60);
                for (unsigned i = 0; i < count; ++i) {
                    const auto entry = fields + i * 24;
                    const auto field_type = read<Address>(entry + 16);
                    const auto offset = read<std::uint16_t>(entry + 8);
                    if (!field_type) continue;
                    indent(depth);
                    out += hex(read<std::uint32_t>(entry), 8) + '@' + hex(offset, 4);
                    describe(field_type);
                    leaf(field_type, value + offset, depth);
                }
                return;
            }
            case kind_array: {
                const auto at = read<Address>(value);
                const auto count = at ? read<std::uint32_t>(at - 4) & 0x7fffffffU : 0;
                const auto element = read<Address>(meta + 0x30);
                const auto stride = element ? size(element) : 0;
                out += " [" + std::to_string(count) + "]\n";
                if (!at || !stride || stride > 8192 || count > 4096) return;
                for (unsigned i = 0; i < std::min(count, max_elements); ++i) {
                    indent(depth + 1);
                    out += '[' + std::to_string(i) + ']';
                    describe(element);
                    leaf(element, at + std::size_t{stride} * i, depth + 1);
                }
                return;
            }
            default: return;
            }
        } catch (const std::exception&) { /* Unloaded optional branches are expected. */ }
    }
    // Prints one value in place, then recurses when it has structure.
    void leaf(Address type, Address value, unsigned depth) {
        try {
            switch (kind(type)) {
            case kind_structure: out += '\n'; walk(type, value, depth + 1); return;
            case kind_reference: reference(value, depth); return;
            case kind_array: walk(type, value, depth); return;
            case kind_text: out += " = \"" + string(read<Address>(value), 256) + "\"\n"; return;
            case kind_handle: handle(value, depth); return;
            default: bytes(value, size(type)); out += '\n'; return;
            }
        } catch (const std::exception&) { out += " = <unreadable>\n"; }
    }
};

std::filesystem::path dump_path(const std::string& what, std::uint64_t tick) {
    std::array<wchar_t, 32768> module{};
    if (!GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()))) return {};
    std::wstring tag;
    for (const auto c : what) tag += (c >= '0' && c <= 'z') ? static_cast<wchar_t>(c) : L'-';
    return logging::log_directory(std::filesystem::path(module.data()).parent_path()) /
        (L"ui-dump-" + tag + L'-' + std::to_wstring(tick) + L".txt");
}
} // namespace

std::string request_ui_dump(std::string_view what, unsigned delay_seconds) {
    std::lock_guard lock(request_mutex);
    request_what = what.empty() ? "all" : std::string(what);
    const auto delay = std::min(delay_seconds, 600U);
    request_due.store(GetTickCount64() + delay * 1000ULL, std::memory_order_relaxed);
    request_pending.store(true, std::memory_order_release);
    return "UI dump queued (" + request_what + ")" +
        (delay ? ", firing in " + std::to_string(delay) + "s" : "") +
        ". The file lands in the game's logs folder.";
}

void run_pending_ui_dump(std::uintptr_t base) noexcept {
    if (!request_pending.load(std::memory_order_acquire)) return;
    if (GetTickCount64() < request_due.load(std::memory_order_relaxed)) return;
    std::string what;
    {
        std::lock_guard lock(request_mutex);
        if (!request_pending.exchange(false, std::memory_order_acq_rel)) return;
        what = request_what;
    }
    try {
        const auto ui = read<Address>(base + addr::engine::ui_manager);
        const auto manager = ui ? read<Address>(ui + 0x140) : 0;
        if (!manager || !game::native_data().models.value) {
            logging::write(logging::Level::warning, logging::Channel::ui,
                "UI dump: the native model manager is not available yet.");
            return;
        }
        const Context context(base, manager);
        // "asset:<name>": a loaded data asset by name, from whichever asset domain holds it.
        if (what.starts_with("asset:")) {
            const auto name = what.substr(6);
            const auto find = game::native_data().find_asset;
            std::string text;
            for (std::uint16_t domain = 0; domain < 0xbbf && find; ++domain) {
                if (!read<Address>(base + addr::engine::domain_owners + domain * 8ULL)) continue;
                const auto asset = find(domain, name.c_str());
                if (!asset) continue;
                const auto type = read<Address>(asset + 8);
                Walker walker{context};
                walker.out = "asset " + name + " in domain " + std::to_string(domain) + " at " + hex(asset, 16) +
                             " type <" + hex(type ? schema_of(type) : 0, 8) + " k" + std::to_string(type ? kind(type) : 0) +
                             " " + std::to_string(type ? size(type) : 0) + ">\n";
                Address words[12]{};
                memory::read_bytes(asset, words, sizeof(words));
                walker.out += "header:";
                for (const auto word : words) walker.out += ' ' + hex(word, 16);
                walker.out += '\n';
                // Each header word that points at another object: its first words, to find the data.
                for (unsigned index = 0; index < 12; ++index) {
                    Address inner[6]{};
                    if (!words[index] || !memory::read_bytes(words[index], inner, sizeof(inner))) continue;
                    walker.out += "  +" + hex(index * 8, 2) + " ->";
                    for (const auto word : inner) walker.out += ' ' + hex(word, 16);
                    walker.out += '\n';
                }
                // A record keeps its real type at +0x18 and its data at +0x20.
                try {
                    const auto data_type = words[3], data = words[4];
                    if (data_type && data) {
                        walker.out += "data <" + hex(schema_of(data_type), 8) + " k" + std::to_string(kind(data_type)) + " " +
                                      std::to_string(size(data_type)) + ">\n";
                        walker.walk(data_type, data, 1);
                    }
                } catch (const std::exception &error) {
                    walker.out += std::string("data walk failed: ") + error.what() + '\n';
                }
                text += walker.out;
                break;
            }
            if (text.empty()) text = "asset " + name + " is not loaded in any domain\n";
            const auto path = dump_path("asset", GetTickCount64());
            std::ofstream(path, std::ios::trunc) << text;
            logging::log(logging::Level::info, logging::Channel::ui, "UI dump: asset {} written to {}.", name, path_utf8(path));
            return;
        }
        std::uint32_t only_schema{};
        if (what.size() > 2 && what[0] == '0' && (what[1] == 'x' || what[1] == 'X'))
            only_schema = static_cast<std::uint32_t>(std::stoul(what.substr(2), nullptr, 16));
        const auto filter = only_schema || what == "all" || what == "roots" ? std::string{} : what;
        std::string text;
        unsigned roots{}, written{};
        {
            game::ModelWriteLock lock(manager);
            for (const auto& root : context.roots()) {
                ++roots;
                const auto schema = schema_of(root.model.type);
                if (only_schema && schema != only_schema) continue;
                std::string header = "root " + hex(root.model.handle, 16) + " <" + hex(schema, 8) + ' ' +
                    std::to_string(size(root.model.type)) + ">\n";
                if (what == "roots") { text += header; ++written; continue; }
                Walker walker{context};
                walker.walk(root.model.type, root.value, 0);
                if (!filter.empty() && walker.out.find(filter) == std::string::npos) continue;
                text += header + walker.out;
                ++written;
            }
        }
        const auto path = dump_path(what, GetTickCount64());
        std::ofstream out(path, std::ios::trunc);
        out << "roots " << roots << ", written " << written << ", filter " << (what.empty() ? "all" : what) << "\n"
            << text;
        logging::log(logging::Level::info, logging::Channel::ui,
            "UI dump: {} of {} roots written to {}.", written, roots, path_utf8(path));
    } catch (const std::exception& error) {
        logging::log(logging::Level::warning, logging::Channel::ui, "UI dump failed: {}.", error.what());
    }
}
} // namespace dingosdk::multiplayer
