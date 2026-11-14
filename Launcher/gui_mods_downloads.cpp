#include "gui_internal.h"

#include "mod_manager.h"
#include "updater.h"

#include "Engine/Core/Debug/trace.h"
#include "Engine/Core/Log/logging.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <set>

// The Mods panel's downloads: every mod asked for goes in a queue, several are downloaded at once
// (Settings::parallel_downloads) and each is installed as its download ends, one install at a time.
// Nothing covers the page while they run: a bar along its bottom says how far they are, and opens
// the queue, where one can be cancelled or all of them. More can be asked for meanwhile.
namespace dingosdk::launcher_gui::detail {
namespace {

namespace ts = thunderstore;
using Stage = Download::State;

constexpr int download_timeout_ms = 30000;
// How much of a mod's bar its download is; the rest is unpacking it into the Mods folder.
constexpr float download_share = 0.85f;

void log(logging::Level level, const std::string& message) {
    logging::write(level, logging::Channel::launcher, message);
}

bool over(Stage state) { return state != Stage::queued && state != Stage::downloading && state != Stage::installing; }

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Install cancelled.") {}
};

// Downloads one package into Mods/.reskate-download, then installs it over any installed copy
// (which is deleted, its place in mods.json kept). Returns the folder it went into.
std::string install_package(Downloads& queue, Download& job, const fs::path& root) {
    const auto& package = job.package;
    const auto& version = package.latest();
    const auto folder = root / L".reskate-download";
    const auto archive = folder / (wide(package.full_name) + L".zip");
    std::error_code error;
    fs::create_directories(folder, error);
    struct Cleanup {
        fs::path folder, archive;
        // (The folder goes with the last download in it: removing one that is not empty does nothing.)
        ~Cleanup() { std::error_code ignored; fs::remove(archive, ignored); fs::remove(folder, ignored); }
    } cleanup{folder, archive};

    trace::Scope timing("launcher.mod_install", "task");
    timing.tag("package", package.full_name);
    timing.tag("version", version.number);
    {
        FILE* file{};
        if (_wfopen_s(&file, archive.c_str(), L"wb") || !file)
            throw std::runtime_error("Could not write the download into the Mods folder.");
        std::uint64_t received = 0;
        const auto expected = version.file_size;
        job.expected = expected;
        try {
            launcher_update::http_stream(wide(version.download_url), expected ? expected + (1 << 20) : 8ull << 30,
                download_timeout_ms, [&](const char* data, DWORD size) {
                    if (job.cancel) throw Cancelled();
                    if (std::fwrite(data, 1, size, file) != size)
                        throw std::runtime_error("Could not write the download (is the disk full?).");
                    received += size;
                    job.received = received;
                    if (expected) job.progress = download_share * static_cast<float>(static_cast<double>(received) / static_cast<double>(expected));
                });
        } catch (...) {
            std::fclose(file);
            throw;
        }
        std::fclose(file);
        if (expected && received != expected)
            throw std::runtime_error(std::format("The download of {} stopped early ({} of {}).", package.title(),
                size_text(received), size_text(expected)));
    }
    job.progress = download_share;
    job.state = Stage::installing;
    launcher_mods::InstallOptions options;
    options.folder = ts::folder_for(package.full_name);
    options.author = package.owner;
    options.require_content = true;
    // One install at a time: each rewrites mods.json and works in the same staging folder.
    std::lock_guard installing(queue.installing);
    if (job.cancel) throw Cancelled();
    return launcher_mods::install(root, archive, true,
        [&job](float fraction) { job.progress = download_share + (1 - download_share) * fraction; }, job.cancel, options);
}

void run_job(ModsPanel& panel, Download& job, const fs::path& root) {
    auto& queue = panel.downloads;
    std::string folder, error;
    auto state = Stage::done;
    try {
        if (job.store) {
            const bool update = fs::exists(root / wide(ts::folder_for(job.package.full_name)));
            job.update = update;
            folder = install_package(queue, job, root);
            log(logging::Level::info, std::format("Mod {} from Thunderstore: {} v{} into Mods\\{}",
                update ? "updated" : "installed", job.package.full_name, job.package.latest().number, folder));
        } else {
            std::lock_guard installing(queue.installing);
            if (job.cancel) throw Cancelled();
            folder = launcher_mods::install(root, job.source, job.replace, [&job](float fraction) { job.progress = fraction; }, job.cancel);
            log(logging::Level::info, "Mod installed: " + folder);
        }
    } catch (const launcher_mods::AlreadyInstalled& existing) {
        folder = existing.name;
        state = Stage::exists;
    } catch (const std::exception& failure) {
        error = failure.what();
        state = job.cancel ? Stage::cancelled : Stage::failed;
        if (state == Stage::failed) log(logging::Level::warning, "Install of " + job.title + " failed: " + error);
    }
    std::lock_guard lock(queue.mutex);
    job.folder = std::move(folder);
    job.error = std::move(error);
    if (state == Stage::done) job.progress = 1;
    job.state = state;
}

// One of the queue's threads: takes the next mod waiting until none is, then ends.
void work(ModsPanel& panel, fs::path root, std::shared_ptr<std::atomic<bool>> ended) {
    auto& queue = panel.downloads;
    wait_for_conflict_check(panel);   // (it may be reading the mods these replace)
    for (;;) {
        std::shared_ptr<Download> job;
        {
            std::lock_guard lock(queue.mutex);
            for (const auto& waiting : queue.jobs)
                if (waiting->state == Stage::queued) {
                    job = waiting;
                    break;
                }
            if (!job) {
                --queue.working;
                *ended = true;
                return;
            }
            if (job->cancel) {
                job->error = "Install cancelled.";
                job->state = Stage::cancelled;
                continue;
            }
            job->state = job->store ? Stage::downloading : Stage::installing;
        }
        run_job(panel, *job, root);
    }
}

// Puts `jobs` in the queue and sees that enough threads are taking from it. UI thread.
void enqueue(ModsPanel& panel, std::vector<std::shared_ptr<Download>> jobs) {
    auto& queue = panel.downloads;
    if (jobs.empty()) return;
    // The threads that have ended are joined here, where no one waits on them.
    std::erase_if(queue.workers, [](Downloads::Worker& worker) {
        if (!*worker.ended) return false;
        if (worker.thread.joinable()) worker.thread.join();
        return true;
    });
    std::lock_guard lock(queue.mutex);
    // A new lot after the last was done: what that one finished is not listed with it.
    if (!panel.installing) {
        std::erase_if(queue.jobs, [](const std::shared_ptr<Download>& job) { return job->collected; });
        queue.batch = {};
    }
    for (auto& job : jobs) {
        job->id = queue.next++;
        queue.jobs.push_back(std::move(job));
    }
    panel.installing = true;
    panel.message.clear();
    panel.message_error = false;
    const auto waiting = static_cast<int>(std::count_if(queue.jobs.begin(), queue.jobs.end(),
        [](const std::shared_ptr<Download>& job) { return job->state == Stage::queued; }));
    const int wanted = std::min(std::clamp(queue.parallel, 1, max_parallel_downloads), queue.working + waiting);
    while (queue.working < wanted) {
        ++queue.working;
        auto ended = std::make_shared<std::atomic<bool>>(false);
        queue.workers.push_back({std::thread(work, std::ref(panel), panel.root, ended), ended});
    }
}

std::string state_text(const Download& job, Stage state) {
    switch (state) {
    case Stage::queued: return job.cancel ? "Cancelling" : "Waiting";
    case Stage::downloading: {
        if (job.cancel) return "Cancelling";
        const auto expected = job.expected.load(), received = job.received.load();
        return expected ? std::format("Downloading  {} of {}", size_text(received), size_text(expected))
                        : received ? std::format("Downloading  {}", size_text(received)) : std::string("Downloading");
    }
    case Stage::installing: return job.cancel ? "Cancelling" : "Installing";
    case Stage::done: return job.store ? (job.update ? "Updated" : "Installed") : "Installed";
    case Stage::failed: return "Failed";
    case Stage::cancelled: return "Cancelled";
    case Stage::exists: return "Already installed";
    }
    return {};
}

void cancel_all(ModsPanel& panel, const char* how) {
    auto& queue = panel.downloads;
    std::lock_guard lock(queue.mutex);
    bool any = false;
    for (const auto& job : queue.jobs)
        if (!over(job->state) && !job->cancel.exchange(true)) any = true;
    if (any) log(logging::Level::info, std::string("Mod installs cancelled ") + how + ".");
}

} // namespace

Downloads::~Downloads() {
    {
        std::lock_guard lock(mutex);
        for (const auto& job : jobs) job->cancel = true;
    }
    for (auto& worker : workers)
        if (worker.thread.joinable()) worker.thread.join();
}

void start_install(ModsPanel& panel, const fs::path& source, bool replace) {
    auto job = std::make_shared<Download>();
    job->title = utf8(source.filename().wstring());
    job->source = source;
    job->replace = replace;
    enqueue(panel, {std::move(job)});
}

void start_store_install(ModsPanel& panel, std::vector<thunderstore::Package> packages) {
    if (packages.empty()) return;
    auto& queue = panel.downloads;
    // A package brings what it needs with it (a mod pack is nothing else), needed mods first.
    const auto have = installed_versions(panel.list);
    // The installed folders as Windows tells them apart, ignoring case: one installed by hand or by another
    // manager may be spelt differently from Thunderstore's name, and is still a mod the player had.
    const auto folded = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::set<std::string> had;
    std::map<std::string, std::string> version_of;   // folded folder -> its installed version
    for (const auto& [folder, version] : have) {
        had.insert(folded(folder));
        version_of.emplace(folded(folder), version);
    }
    // So a dependency installed under another spelling counts as installed (at its version) and is not
    // downloaded again: each package whose folder is one of those, under Thunderstore's own spelling.
    auto installed = have;
    for (const auto& known : panel.store.packages) {
        const auto folder = ts::folder_for(known.full_name);
        if (installed.contains(folder)) continue;
        if (const auto found = version_of.find(folded(folder)); found != version_of.end()) installed.emplace(folder, found->second);
    }
    // What is already waiting or on its way is not asked for twice.
    std::set<std::string> queued;
    {
        std::lock_guard lock(queue.mutex);
        for (const auto& job : queue.jobs)
            if (job->store && !over(job->state)) queued.insert(job->package.full_name);
    }
    std::vector<std::shared_ptr<Download>> jobs;
    for (const auto& package : packages) {
        std::vector<std::string> brings;
        for (auto& needed : thunderstore::with_dependencies(package, panel.store.packages, installed)) {
            // What it brings in: a dependency that is not installed at all now (one only updated was there before).
            const auto folder = ts::folder_for(needed.full_name);
            if (needed.full_name != package.full_name && !had.contains(folded(folder))) brings.push_back(folder);
            if (!queued.insert(needed.full_name).second) continue;
            auto job = std::make_shared<Download>();
            job->store = true;
            job->title = needed.title();
            job->version = needed.latest().number;
            job->expected = needed.latest().file_size;
            job->package = std::move(needed);
            jobs.push_back(std::move(job));
        }
        // A pack that installs remembers what it brought in, so its uninstall can offer exactly those.
        if (!brings.empty()) queue.brought.emplace_back(ts::folder_for(package.full_name), std::move(brings));
    }
    enqueue(panel, std::move(jobs));
}

void collect_install(ModsPanel& panel, const launcher_app::Session& session) {
    auto& queue = panel.downloads;
    if (!panel.installing) return;
    bool arrived = false, idle = true;
    {
        std::lock_guard lock(queue.mutex);
        for (const auto& job : queue.jobs) {
            const auto state = job->state.load();
            if (!over(state)) {
                idle = false;
                continue;
            }
            if (job->collected) continue;
            // One that is installed already asks whether to replace it, one question at a time.
            if (state == Stage::exists) {
                if (!panel.conflict_name.empty()) {
                    idle = false;
                    continue;
                }
                panel.conflict_name = job->folder;
                panel.conflict_source = job->source;
            }
            job->collected = true;
            auto& batch = queue.batch;
            if (state == Stage::done) {
                arrived = true;
                ++batch.done;
                batch.last = job->store ? std::format("{} {} v{}", job->update ? "Updated" : "Installed", job->title, job->version)
                                        : "Installed " + job->folder;
            } else if (state == Stage::failed) {
                batch.failed.push_back(job->title + ": " + job->error);
            } else if (state == Stage::cancelled) {
                ++batch.cancelled;
            }
        }
    }
    // The folder is read again as mods arrive, so MY MODS has them while the rest come in.
    if (arrived) scan(panel, session);
    if (!idle) return;

    for (const auto& [pack, brings] : queue.brought) {
        std::error_code missing;
        if (!fs::is_directory(panel.root / wide(pack), missing)) continue;
        std::vector<std::string> present;
        for (const auto& folder : brings)
            if (fs::is_directory(panel.root / wide(folder), missing)) present.push_back(folder);
        try {
            launcher_mods::record_brought_in(panel.root, pack, present);
        } catch (const std::exception& failure) {
            log(logging::Level::warning, "Could not note what " + pack + " brought in: " + failure.what());
        }
    }
    queue.brought.clear();
    // What the lot came to. Only what went wrong is said on the page; the bar says the rest.
    const auto& batch = queue.batch;
    const auto total = batch.done + batch.failed.size() + batch.cancelled;
    panel.message.clear();
    panel.message_error = false;
    if (!batch.failed.empty()) {
        // The first few by name and reason; the queue and the log have every one.
        constexpr std::size_t shown = 3;
        panel.message = total == 1 ? batch.failed.front()
            : std::format("{} of {} could not be installed{}. ", batch.failed.size(), total,
                          batch.done ? std::format(" ({} were)", batch.done) : std::string());
        if (total > 1) {
            for (std::size_t i = 0; i < batch.failed.size() && i < shown; ++i) panel.message += (i ? " | " : "") + batch.failed[i];
            if (batch.failed.size() > shown) panel.message += std::format(" | and {} more", batch.failed.size() - shown);
        }
        panel.message_error = true;
    } else if (batch.cancelled) {
        panel.message = batch.done ? std::format("Cancelled. {} mod(s) were installed before that.", batch.done)
                                   : std::string("Install cancelled. The Mods folder is as it was.");
        panel.message_error = true;   // (Play does not follow an update that was called off.)
    }
    queue.summary = batch.done == 0 ? std::string()
        : batch.done == 1 ? batch.last + ". It loads the next time Skate starts."
        : std::format("Installed or updated {} mods. Changes apply the next time Skate starts.", batch.done);
    panel.installing = false;
}

std::string download_label(ModsPanel& panel, std::string_view full_name) {
    auto& queue = panel.downloads;
    if (!panel.installing) return {};
    std::lock_guard lock(queue.mutex);
    for (const auto& job : queue.jobs) {
        if (!job->store || job->package.full_name != full_name) continue;
        switch (job->state.load()) {
        case Stage::queued: return "QUEUED";
        case Stage::downloading: return "DOWNLOADING";
        case Stage::installing: return "INSTALLING";
        default: break;
        }
    }
    return {};
}

float downloads_bar_height(const ModsPanel& panel) {
    return panel.downloads.jobs.empty() ? 0.0f : S(46) + S(10);
}

void downloads_bar(const Fonts& fonts, ModsPanel& panel, ImVec2 position, float width) {
    auto& queue = panel.downloads;
    // What the queue holds, copied out: the rows are drawn without its lock.
    struct Row {
        std::shared_ptr<Download> job;
        Stage state;
        std::string error;
    };
    std::vector<Row> rows;
    {
        std::lock_guard lock(queue.mutex);
        for (const auto& job : queue.jobs) rows.push_back({job, job->state.load(), job->error});
    }
    if (rows.empty()) return;
    std::size_t waiting{}, active{}, finished{}, failed{};
    float progress = 0;
    std::string names;
    for (const auto& row : rows) {
        if (row.state == Stage::queued) ++waiting;
        else if (!over(row.state)) {
            ++active;
            if (names.size() < 80) names += (names.empty() ? "" : ", ") + row.job->title;
        } else {
            ++finished;
            failed += row.state == Stage::failed;
        }
        progress += over(row.state) ? 1.0f : std::clamp(row.job->progress.load(), 0.0f, 1.0f);
    }
    progress /= static_cast<float>(rows.size());
    const bool busy = waiting || active;
    const auto time = static_cast<float>(ImGui::GetTime());

    const float height = S(46);
    ImGui::SetCursorPos(position);
    ImGui::BeginChild("##downloads_bar", ImVec2(width, height), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetWindowPos(), end(at.x + width, at.y + height);
    const float close_width = busy ? 0.0f : S(40);
    // The whole bar opens the queue.
    ImGui::SetCursorPos(ImVec2(0, 0));
    const bool pressed = ImGui::InvisibleButton("##open_queue", ImVec2(width - close_width, height));
    // The press that lands on the bar while the queue is open has already closed it (a popup goes at
    // a click outside it): the release that follows must not open it again.
    if (ImGui::IsItemActivated()) queue.was_open = queue.open;
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    draw->AddRectFilled(at, end, hot ? rgba(38, 42, 50) : rgba(26, 28, 34), S(6));
    draw->AddRect(at, end, busy ? color::blue : color::outline, S(6), 0, S(busy ? 1.5f : 1.0f));
    const float text_y = at.y + (height - fonts.bold->FontSize) * 0.5f;
    const auto headline = busy ? std::format("{} of {}", finished + 1 > rows.size() ? rows.size() : finished + 1, rows.size())
                               : std::string();
    float x = at.x + S(16);
    if (busy) {
        const auto label = std::format("DOWNLOADING  {}", headline);
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(x, text_y), color::text, label.c_str());
        x += fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, label.c_str()).x + S(16);
        const auto percent = std::format("{:.0f}%", progress * 100);
        const float right = end.x - S(16);
        const auto queue_label = std::string(queue.open ? "HIDE QUEUE" : "VIEW QUEUE");
        const float queue_width = ImGui::CalcTextSize(queue_label.c_str()).x;
        const float percent_width = fonts.bold->CalcTextSizeA(fonts.bold->FontSize, FLT_MAX, 0, percent.c_str()).x;
        const float bar_end = right - queue_width - S(20) - percent_width - S(12);
        const float bar_start = std::min(bar_end - S(120), std::max(x, at.x + width * 0.42f));
        // What is coming down now, in the room before the bar.
        draw->PushClipRect(ImVec2(x, at.y), ImVec2(bar_start - S(14), end.y), true);
        draw->AddText(ImVec2(x, at.y + (height - ImGui::GetFontSize()) * 0.5f), color::muted, names.c_str());
        draw->PopClipRect();
        skate_theme::striped_bar(draw, ImVec2(bar_start, at.y + height * 0.5f - S(6)), ImVec2(bar_end, at.y + height * 0.5f + S(6)),
                                 progress, time, g_scale);
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(bar_end + S(12), text_y), color::text, percent.c_str());
        draw->AddText(ImVec2(right - queue_width, at.y + (height - ImGui::GetFontSize()) * 0.5f), hot ? color::text : color::muted, queue_label.c_str());
    } else {
        const auto text = failed ? std::format("{} of {} could not be installed. Open the queue to see why.", failed, rows.size())
            : !queue.summary.empty() ? queue.summary : std::string("Nothing was installed.");
        draw->AddCircleFilled(ImVec2(x + S(6), at.y + height * 0.5f), S(5), failed ? color::danger : color::good);
        draw->PushClipRect(ImVec2(x + S(20), at.y), ImVec2(end.x - close_width - S(110), end.y), true);
        draw->AddText(ImVec2(x + S(20), at.y + (height - ImGui::GetFontSize()) * 0.5f), color::text, text.c_str());
        draw->PopClipRect();
        const auto queue_label = std::string(queue.open ? "HIDE QUEUE" : "VIEW QUEUE");
        draw->AddText(ImVec2(end.x - close_width - S(12) - ImGui::CalcTextSize(queue_label.c_str()).x, at.y + (height - ImGui::GetFontSize()) * 0.5f),
                      hot ? color::text : color::muted, queue_label.c_str());
        // Done with: the bar goes until the next download.
        ImGui::SetCursorPos(ImVec2(width - close_width, 0));
        if (ImGui::InvisibleButton("##dismiss", ImVec2(close_width, height))) {
            std::lock_guard lock(queue.mutex);
            queue.jobs.clear();
            queue.open = false;
        }
        const bool over_close = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dismiss");
        const ImVec2 centre(end.x - close_width * 0.5f, at.y + height * 0.5f);
        const auto cross = over_close ? color::text : color::muted;
        draw->AddLine(ImVec2(centre.x - S(5), centre.y - S(5)), ImVec2(centre.x + S(5), centre.y + S(5)), cross, S(1.6f));
        draw->AddLine(ImVec2(centre.x - S(5), centre.y + S(5)), ImVec2(centre.x + S(5), centre.y - S(5)), cross, S(1.6f));
    }
    ImGui::EndChild();
    if (pressed && !queue.was_open) ImGui::OpenPopup("##downloads_queue");

    // The queue, over the page above the bar: a popup, which stays in front of the page and goes at a
    // click anywhere else or Escape. (A plain window went behind the page as soon as the page took
    // the focus back: it flashed and was gone, 2026-11-14.)
    const float row_height = S(50);
    const float list = std::min(static_cast<float>(rows.size()), 7.0f) * row_height;
    const ImVec2 extent(std::min(width, S(620)), list + S(58));
    const ImVec2 origin = ImGui::GetWindowPos();
    ImGui::SetNextWindowPos(ImVec2(origin.x + position.x + width - extent.x, origin.y + position.y - extent.y - S(8)));
    ImGui::SetNextWindowSize(extent);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(rgba(20, 22, 27, 0.98f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(rgba(20, 22, 27, 0.98f)));
    queue.open = ImGui::BeginPopup("##downloads_queue", ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (queue.open) {
        ImGui::PushFont(fonts.bold);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("DOWNLOADS");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", busy ? std::format("{} at a time", std::clamp(queue.parallel, 1, max_parallel_downloads)).c_str() : "done");
        const float button = S(120);
        ImGui::SameLine(ImGui::GetWindowWidth() - S(16) - button);
        if (busy) {
            if (ImGui::Button("Cancel all", ImVec2(button, 0))) cancel_all(panel, "by Cancel all");
        } else if (ImGui::Button("Clear", ImVec2(button, 0))) {
            std::lock_guard lock(queue.mutex);
            queue.jobs.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::BeginChild("##queue_rows", ImVec2(0, list), ImGuiChildFlags_NavFlattened);
        auto* rows_draw = ImGui::GetWindowDrawList();
        for (const auto& row : rows) {
            const auto& job = *row.job;
            ImGui::PushID(static_cast<int>(job.id));
            const ImVec2 corner = ImGui::GetCursorScreenPos();
            const float inner = ImGui::GetContentRegionAvail().x;
            const bool done = over(row.state);
            const auto shade = row.state == Stage::failed ? color::danger : row.state == Stage::done ? color::good : color::muted;
            // The name and version, then what it is doing.
            rows_draw->PushClipRect(corner, ImVec2(corner.x + inner - S(44), corner.y + row_height), true);
            rows_draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(corner.x, corner.y + S(4)), done && row.state != Stage::done ? color::muted : color::text,
                               (job.version.empty() ? job.title : job.title + "  v" + job.version).c_str());
            const auto status = row.state == Stage::failed && !row.error.empty() ? "Failed: " + row.error : state_text(job, row.state);
            rows_draw->AddText(ImVec2(corner.x, corner.y + S(4) + fonts.bold->FontSize + S(2)), shade, status.c_str());
            rows_draw->PopClipRect();
            if (!done) {
                const float fraction = std::clamp(job.progress.load(), 0.0f, 1.0f);
                const ImVec2 a(corner.x, corner.y + row_height - S(9)), b(corner.x + inner - S(44), corner.y + row_height - S(5));
                rows_draw->AddRectFilled(a, b, rgba(255, 255, 255, 0.08f), S(2));
                if (fraction > 0) rows_draw->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * fraction, b.y), color::blue, S(2));
            }
            // Cancel one that is not done; take one that is off the list.
            ImGui::SetCursorScreenPos(ImVec2(corner.x + inner - S(34), corner.y + (row_height - S(28)) * 0.5f));
            ImGui::BeginDisabled(!done && job.cancel);
            if (ImGui::Button("x", ImVec2(S(28), S(28)))) {
                if (!done) {
                    row.job->cancel = true;
                    log(logging::Level::info, "Mod install cancelled from the queue: " + job.title);
                } else if (row.job->collected) {
                    std::lock_guard lock(queue.mutex);
                    std::erase(queue.jobs, row.job);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", done ? "Take it off the list" : "Cancel this one");
            if (row.state == Stage::failed && !row.error.empty() && ImGui::IsMouseHoveringRect(corner, ImVec2(corner.x + inner - S(44), corner.y + row_height)))
                ImGui::SetTooltip("%s", row.error.c_str());
            ImGui::SetCursorScreenPos(ImVec2(corner.x, corner.y + row_height));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

} // namespace dingosdk::launcher_gui::detail
