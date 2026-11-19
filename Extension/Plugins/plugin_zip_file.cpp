#include "plugin_zip_file.h"
#include "plugin_manifest.h"
#include "miniz.h"
#include <algorithm>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>

namespace dingosdk::plugins::zip_file {
namespace {
std::atomic<int> jobs_alive{};

// A file is streamed to this name beside its place, and moved into place once every file is good.
// (The data folder's listing leaves out names ending in ".tmp": a write going on.)
constexpr std::string_view temporary_suffix = ".unpacking.tmp";

// A zip file opened for reading: miniz is built without its own file reading, so it is handed the
// file through this. Its directory is read into memory (a few hundred bytes a file); the files
// themselves are read as they are asked for.
struct Archive {
    std::ifstream in;
    mz_zip_archive zip{};
    bool opened{};
    Archive() = default;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    ~Archive() {
        if (opened) mz_zip_reader_end(&zip);
    }
    static size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t count) {
        auto& in = static_cast<Archive*>(opaque)->in;
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(static_cast<char*>(buffer), static_cast<std::streamsize>(count));
        return static_cast<size_t>(in.gcount());
    }
    // Empty once the directory is read, else why not.
    std::string open(const std::filesystem::path& file) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error)) return "not found";
        const auto size = std::filesystem::file_size(file, error);
        in.open(file, std::ios::binary);
        if (error || !in) return "the file cannot be read";
        zip.m_pRead = read;
        zip.m_pIO_opaque = this;
        // (Nothing is looked up by name, so the directory is left in the zip's own order.)
        if (!mz_zip_reader_init(&zip, size, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) return "it is not a zip";
        opened = true;
        if (mz_zip_reader_get_num_files(&zip) > most_files) return std::format("it holds more than {} files", most_files);
        return {};
    }
};

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}
// Whether a zip's name is one a file of the data folder may have under `folder` (a data path, as
// Request::folder is): what data_path asks of a path a plugin gives.
bool name_fits(std::string_view folder, std::string_view name) {
    if (safe_relative_path(name).empty() || names_a_device(name) || name.ends_with(temporary_suffix)) return false;
    const std::size_t between = folder.empty() || folder.ends_with(':') ? 0 : 1;
    return folder.size() + between + name.size() <= most_path_bytes;
}

// One file to unpack: which it is in the zip, and where it goes.
struct Taken {
    mz_uint index{};
    std::string name;
    std::filesystem::path file, temporary;
    bool written{}, moved{};
};
// What an unpack made, removed again unless it is kept: the files (in place, or still beside it) and
// then the folders it made, the deepest first.
struct Made {
    std::vector<Taken> files;
    std::vector<std::filesystem::path> folders;
    bool keep{};
    ~Made() {
        if (keep) return;
        std::error_code error;
        for (const auto& one : files) {
            if (one.moved) std::filesystem::remove(one.file, error);
            else if (one.written) std::filesystem::remove(one.temporary, error);
        }
        for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) std::filesystem::remove(*folder, error);
    }
};

// The files of the zip the request asks for, each checked: nothing is written for a zip that fails here.
std::string choose(Archive& archive, const Request& request, const Place& place, std::vector<Taken>& taken) {
    const std::set<std::string, std::less<>> wanted(request.only.begin(), request.only.end());
    std::set<std::string, std::less<>> seen, found;
    std::uint64_t total = 0;
    const auto count = mz_zip_reader_get_num_files(&archive.zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive.zip, i, &stat)) return "its list of files is damaged";
        if (stat.m_is_directory) continue;
        const std::string name = stat.m_filename;
        if (!request.all && !wanted.contains(name)) continue;
        found.insert(name);
        if (!name_fits(request.folder, name)) return name + " in it is not a name a file here may have";
        // (Windows takes names that differ only in case for one file.)
        if (!seen.insert(lower(name)).second) return name + " is in it twice";
        if (!stat.m_is_supported) return name + " in it is packed in a way that cannot be read";
        if (stat.m_uncomp_size > most_file_bytes) return std::format("{} in it is larger than {} MB", name, most_file_bytes >> 20);
        total += stat.m_uncomp_size;
        if (total > most_bytes) return std::format("it unpacks to more than {} GB", most_bytes >> 30);
        taken.push_back({i, name, place.folder / std::filesystem::path(name), {}});
    }
    for (const auto& name : request.only)
        if (!request.all && !found.contains(name)) return name + " is not in it";
    // A name that is one file's and a folder of another's cannot be both on disk.
    for (const auto& one : taken) {
        const auto name = lower(one.name);
        for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1))
            if (seen.contains(std::string_view(name).substr(0, slash))) return one.name.substr(0, slash) + " in it is a file and a folder";
    }

    // Where each goes: inside the folder's top whatever is linked into it, and not over what is there.
    std::error_code error;
    const auto top = std::filesystem::weakly_canonical(place.root, error);
    if (error) return "the data folder cannot be read";
    std::set<std::filesystem::path> inside;
    std::size_t fresh = 0;
    for (auto& one : taken) {
        const auto parent = one.file.parent_path();
        if (!inside.contains(parent)) {
            const auto real = std::filesystem::weakly_canonical(parent, error);
            if (error) return "the data folder cannot be read";
            const auto relative = real.lexically_relative(top);
            if (relative.empty() || *relative.begin() == "..") return "outside the data folder";
            inside.insert(parent);
        }
        const auto status = std::filesystem::status(one.file, error);
        if (std::filesystem::is_directory(status)) return one.name + ": a folder is there";
        if (!std::filesystem::exists(status)) ++fresh;
        else if (!request.overwrite) return one.name + " is there already";
        one.temporary = one.file;
        one.temporary += std::filesystem::path(std::string(temporary_suffix));
    }
    if (fresh > place.room) return std::format("the folder has room for {} more files, and these are {}", place.room, fresh);
    return {};
}

// The folder, made with the folders above it that are not there; the ones made are added to `made`.
bool make_folders(const std::filesystem::path& folder, std::vector<std::filesystem::path>& made) {
    std::error_code error;
    std::vector<std::filesystem::path> missing;
    for (auto at = folder; !at.empty() && !std::filesystem::exists(at, error); at = at.parent_path()) {
        missing.push_back(at);
        if (at == at.parent_path()) break;
    }
    for (auto one = missing.rbegin(); one != missing.rend(); ++one) {
        if (std::filesystem::create_directory(*one, error)) made.push_back(*one);
        else if (error || !std::filesystem::is_directory(*one, error)) return false;
    }
    return std::filesystem::is_directory(folder, error);
}

// What miniz writes a file's bytes to, a piece at a time: the file beside its place.
struct Sink {
    std::ofstream out;
    const std::atomic<bool>* stop{};
    bool failed{}, stopped{};
    static size_t write(void* opaque, mz_uint64, const void* buffer, size_t count) {
        auto& sink = *static_cast<Sink*>(opaque);
        if (sink.stop && sink.stop->load(std::memory_order_relaxed)) {
            sink.stopped = true;
            return 0;
        }
        if (!sink.out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(count))) {
            sink.failed = true;
            return 0;
        }
        return count;
    }
};

constexpr const char* was_stopped = "it was stopped";

std::string write(Archive& archive, Made& made, const std::atomic<bool>* stop) {
    std::set<std::filesystem::path> folders;
    for (auto& one : made.files) {
        if (stop && stop->load(std::memory_order_relaxed)) return was_stopped;
        if (const auto parent = one.file.parent_path(); folders.insert(parent).second && !make_folders(parent, made.folders)) return "the folder cannot be made";
        Sink sink{std::ofstream(one.temporary, std::ios::binary | std::ios::trunc), stop};
        if (!sink.out) return "the file cannot be written";
        one.written = true;
        // (miniz checks the CRC as it unpacks, a stored file's too.)
        const bool good = mz_zip_reader_extract_to_callback(&archive.zip, one.index, Sink::write, &sink, 0) != 0;
        sink.out.close();
        if (sink.stopped) return was_stopped;
        if (sink.failed || sink.out.fail()) return "the file cannot be written";
        if (!good) return one.name + " in it is damaged";
    }
    // Every file is whole: into place, in place of what is there.
    for (auto& one : made.files) {
        std::error_code error;
        std::filesystem::rename(one.temporary, one.file, error);
        if (error) return "the file cannot be written";
        one.moved = true;
    }
    return {};
}

// The folders being unpacked into, by a name that is the same for the same folder.
std::mutex busy_mutex;
std::vector<std::string> busy;
std::string busy_name(const std::filesystem::path& folder) {
    auto name = lower(folder.lexically_normal().generic_string());
    while (name.ends_with('/')) name.pop_back();
    return name + "/";
}
bool claim(const std::string& name) {
    std::lock_guard lock(busy_mutex);
    for (const auto& other : busy)
        if (other.starts_with(name) || name.starts_with(other)) return false;
    busy.push_back(name);
    return true;
}
void release(const std::string& name) {
    std::lock_guard lock(busy_mutex);
    if (const auto found = std::find(busy.begin(), busy.end(), name); found != busy.end()) busy.erase(found);
}
}

Job::Job() noexcept { jobs_alive.fetch_add(1, std::memory_order_relaxed); }
Job::~Job() { jobs_alive.fetch_sub(1, std::memory_order_relaxed); }
int Job::alive() noexcept { return jobs_alive.load(std::memory_order_relaxed); }

std::string list(const std::filesystem::path& file, std::vector<Entry>& entries) {
    entries.clear();
    try {
        Archive archive;
        if (auto why = archive.open(file); !why.empty()) return why;
        const auto count = mz_zip_reader_get_num_files(&archive.zip);
        entries.reserve(count);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive.zip, i, &stat)) {
                entries.clear();
                return "its list of files is damaged";
            }
            if (stat.m_is_directory) continue;
            entries.push_back({stat.m_filename, stat.m_uncomp_size, stat.m_comp_size, stat.m_crc32});
        }
        return {};
    } catch (const std::exception&) {
        entries.clear();
        return "the file cannot be read";
    }
}

std::string unpack(const Request& request, const Place& place, std::size_t& count, const std::atomic<bool>* stop) {
    count = 0;
    try {
        Archive archive;
        if (auto why = archive.open(place.zip); !why.empty()) return why;
        Made made;
        if (auto why = choose(archive, request, place, made.files); !why.empty()) {
            made.keep = true; // (nothing was written)
            return why;
        }
        if (auto why = write(archive, made, stop); !why.empty()) return why;
        made.keep = true;
        count = made.files.size();
        return {};
    } catch (const std::bad_alloc&) {
        return "there is not enough memory for it";
    } catch (const std::exception&) {
        return "the file cannot be written";
    }
}

std::string unpack_later(Request request, Place place, std::shared_ptr<Job> job) {
    const auto name = busy_name(place.folder);
    if (!claim(name)) return "that folder is being unpacked into already";
    try {
        // (The thread holds only what it was given.)
        std::thread([request = std::move(request), place = std::move(place), job, name] {
            job->why = unpack(request, place, job->count, &job->abandoned);
            release(name);
            job->done.store(true, std::memory_order_release);
        }).detach();
    } catch (const std::system_error&) {
        release(name);
        return "a thread could not be started for it";
    }
    return {};
}
}
