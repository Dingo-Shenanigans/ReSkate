#include "Engine/Vfs/material_declaration_refresh.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Core/Platform/shared_file.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace dingosdk;
namespace fs = std::filesystem;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const fs::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) throw std::runtime_error("fixture write failed");
}
}
int main() try {
    const auto root = fs::temp_directory_path() / ("reskate-declaration-refresh-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    const auto file = root / "reskate-items.json";
    write(file, R"({"mods":[{"mod":"Owner","items":["own_test"]},{"mod":"LowerPriority","items":["own_test"]}]})");
    auto sources = mods::read_item_sources_checked(file);
    check(sources && mods::item_source_owner(*sources, "OWN_TEST") == "Owner", "actual ownership reader resolves a case-insensitive item to the highest-priority source");
    check(mods::item_source_owner(*sources, "own_missing").empty(), "unknown ownership grants nothing");

    mods::MaterialDeclarationRefresh refresh;
    std::size_t loads{};
    const auto load = [&] {
        ++loads;
        auto parsed = mods::read_item_sources_checked(file);
        if (!parsed) throw std::runtime_error("partial ownership write");
        mods::MaterialDeclarations value;
        value.sources = std::move(*parsed);
        return value;
    };
    auto signature = std::vector{mods::declaration_file_stamp(file)};
    check(refresh.update(signature, load), "initial declarations are loaded");
    const auto good = refresh.value();
    for (int poll = 0; poll < 100; ++poll) check(!refresh.update(signature, load), "unchanged polls do not reload");
    check(loads == 1, "unchanged polling never invokes the JSON loader");
    signature[0].revision = 1;
    check(refresh.update(signature, load) && loads == 2, "a live apply invalidates ownership even when file metadata is unchanged");
    const auto after_apply = refresh.value();
    write(file, "{partial");
    signature = {mods::declaration_file_stamp(file)};
    bool failed{};
    try { refresh.update(signature, load); } catch (const std::exception&) { failed = true; }
    check(failed && refresh.value() == after_apply && good != after_apply, "partial reads retain the exact last good ownership snapshot");
    write(file, R"({"mods":[]})");
    signature = {mods::declaration_file_stamp(file)};
    check(refresh.update(signature, load) && refresh.value()->sources.empty(), "valid empty ownership differs from failed reads");

    const auto replacement = root / "replacement.json";
    write(replacement, R"({"mods":[{"mod":"NewOwner","items":["own_test"]}]})");
    {
        SharedTextFile held(file);
        // Windows may refuse replacing an open target even with delete sharing,
        // but the read must at least allow the old document to be renamed away.
        const auto retired = root / "retired.json";
        fs::rename(file, retired);
        fs::rename(replacement, file);
        check(held.read(1024) == R"({"mods":[]})", "an open ownership read permits renaming the old document and retains its old file");
    }
    sources = mods::read_item_sources_checked(file);
    check(sources && mods::item_source_owner(*sources, "own_test") == "NewOwner", "the next read sees the replaced ownership document");
    failed = false;
    try { (void)read_shared_text(file, 1); } catch (const std::exception&) { failed = true; }
    check(failed, "shared reads retain their size bound");

    mods::Catalog active;
    mods::Mod running; running.name = "Running"; running.directory = root / "running";
    active.mods.push_back(running);
    mods::variants_applied(active);
    const auto before = mods::active_mod_sources();
    write(root / "mods.json", R"({"mods":[{"name":"Unapplied","enabled":true}]})");
    check(mods::active_mod_sources() == before && before.size() == 1 && before[0].name == "Running",
          "editing saved settings does not activate unapplied mod sources");
    active.mods[0].name = "Applied";
    active.mods[0].directory = root / "applied";
    mods::variants_applied(active);
    check(mods::active_mod_sources()[0].directory == root / "applied", "successful live apply publishes the actual source directory");
    fs::remove(file); fs::remove(root / "retired.json"); fs::remove(root / "mods.json"); fs::remove(root);
    std::cout << "material declaration refresh tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
