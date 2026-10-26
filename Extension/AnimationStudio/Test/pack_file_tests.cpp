#include "Extension/AnimationStudio/pack_file_transaction.h"
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace dingosdk::anim_studio;
namespace fs = std::filesystem;
namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << text;
}
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
struct Fixture {
    fs::path root = fs::temp_directory_path() / ("reskate-pack-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directory(root); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};
void prepared_files_do_not_touch_the_installed_pack() {
    Fixture f;
    put(f.root / "trick.clip", "old clip"); put(f.root / "pack.json", "old pack");
    {
        PackFileTransaction update(f.root);
        update.write(f.root / "trick.clip", "new clip");
        update.remove(f.root / "pack.json");
        check(read(f.root / "trick.clip") == "old clip" && read(f.root / "pack.json") == "old pack", "Preparing an update changed installed files");
        // Abandon an update (a later write/validation fails): both originals must stay.
    }
    check(read(f.root / "trick.clip") == "old clip" && read(f.root / "pack.json") == "old pack", "An abandoned update lost installed data");
}
void a_complete_update_replaces_and_removes_together() {
    Fixture f;
    put(f.root / "old.clip", "obsolete"); put(f.root / "pack.json", "v1");
    PackFileTransaction update(f.root);
    update.remove(f.root / "old.clip");
    update.remove(f.root / "trick.clip");
    check(update.removing(f.root / "trick.clip"), "A replaced member's name must be reusable");
    update.write(f.root / "trick.clip", "new clip");
    update.write(f.root / "pack.json", "v2");
    update.write(f.root / "presets/locked.json", "new ownership");
    check(!update.removing(f.root / "trick.clip"), "A staged replacement must cancel its deletion");
    update.commit();
    check(!fs::exists(f.root / "old.clip") && read(f.root / "trick.clip") == "new clip" && read(f.root / "pack.json") == "v2" &&
          read(f.root / "presets/locked.json") == "new ownership", "A committed pack update is incomplete");
}
void commit_failure_restores_replaced_removed_and_new_files() {
    Fixture f;
    put(f.root / "a.clip", "old clip"); put(f.root / "b.clip", "old removed clip");
    put(f.root / "z.json", "old metadata");
    PackFileTransaction update(f.root);
    update.write(f.root / "a.clip", "new clip");
    update.remove(f.root / "b.clip");
    update.write(f.root / "c.clip", "newly added clip");
    update.write(f.root / "z.json", "new metadata");
#ifdef _WIN32
    // Permit the backup to read the metadata, but refuse replacing it. Earlier operations will already have run.
    const HANDLE held = CreateFileW((f.root / "z.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(held != INVALID_HANDLE_VALUE, "Could not lock the metadata fixture");
    bool failed = false;
    try { update.commit(); } catch (const std::exception&) { failed = true; }
    CloseHandle(held);
    check(failed, "A locked metadata file should fail the update");
    check(read(f.root / "a.clip") == "old clip" && read(f.root / "b.clip") == "old removed clip" &&
          !fs::exists(f.root / "c.clip") && read(f.root / "z.json") == "old metadata", "Failed update did not restore the old pack");
#endif
}
void invalid_destinations_and_failed_staging_preserve_the_pack() {
    Fixture f;
    put(f.root / "pack.json", "old pack");
    PackFileTransaction update(f.root);
    update.write(f.root / "new.clip", "new clip");
    bool failed = false;
    try { update.write(f.root / "new.clip/child", "cannot stage under a file"); } catch (const std::exception&) { failed = true; }
    check(failed && read(f.root / "pack.json") == "old pack" && !fs::exists(f.root / "new.clip"), "Failed staging changed the installed pack");
    failed = false;
    try { update.remove(f.root / "../outside.clip"); } catch (const std::exception&) { failed = true; }
    check(failed, "A transaction accepted a destination outside its library");
}
void a_backup_failure_leaves_all_installed_files_untouched() {
    Fixture f;
    put(f.root / "a.clip", "old clip");
    fs::create_directory(f.root / "z.json");
    PackFileTransaction update(f.root);
    update.write(f.root / "a.clip", "replacement");
    update.write(f.root / "z.json", "cannot back up a directory as a file");
    bool failed = false;
    try { update.commit(); } catch (const std::exception&) { failed = true; }
    check(failed && read(f.root / "a.clip") == "old clip" && fs::is_directory(f.root / "z.json"),
          "A backup failure applied part of the update");
}
void cancelled_recycling_never_falls_back_to_deletion() {
    Fixture f;
    const auto trick = f.root / "trick.clip", metadata = f.root / "pack.json";
    put(trick, "personal edits"); put(metadata, "installed pack");
    for (const bool accepted : {false, true}) {
        bool failed = false, reached_metadata = false;
        try {
            recycle_pack_files({trick}, [accepted](const auto&) { return accepted; });
            reached_metadata = true;
        } catch (const std::exception&) { failed = true; }
        check(failed == !accepted && reached_metadata == accepted, "Failed recycling must abort before changing pack metadata");
        check(read(trick) == "personal edits" && read(metadata) == "installed pack", "Rejected recycling deleted a remaining file");
    }
    const auto another = f.root / "another.clip";
    put(another, "remaining edits");
    bool failed = false;
    try {
        recycle_pack_files({trick, another}, [&](const auto&) {
            fs::rename(trick, f.root / "already-recycled.clip");
            return false; // Windows can stop after moving only some of the requested files.
        });
    } catch (const std::exception&) { failed = true; }
    check(failed && read(another) == "remaining edits" && read(metadata) == "installed pack" &&
          read(f.root / "already-recycled.clip") == "personal edits", "Partial recycling permanently deleted a remaining trick");
}
}
int main() {
    try {
        prepared_files_do_not_touch_the_installed_pack();
        a_complete_update_replaces_and_removes_together();
        commit_failure_restores_replaced_removed_and_new_files();
        invalid_destinations_and_failed_staging_preserve_the_pack();
        a_backup_failure_leaves_all_installed_files_untouched();
        cancelled_recycling_never_falls_back_to_deletion();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "studio pack file tests passed\n";
}
