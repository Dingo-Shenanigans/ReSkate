// TEST ONLY, built in with -DDINGOSDK_UI_TEST_HOOKS=ON: lists every control the overlay draws, by the ID it already
// has, with its box on the screen, so a game test can find a control by its ID and click it there with a real mouse
// click. Nothing in the overlay changes: these are Dear ImGui's own test-engine hooks (IMGUI_ENABLE_TEST_ENGINE), which
// every ItemAdd and InvisibleButton already calls.
//
// ReSkate.Tricks/research/ui-ids.txt beside the exe, rewritten twice a second while the overlay draws:
//   # frame <n>: <items> items, <named> with an ID name
//   <ID>\t<left>,<top>,<right>,<bottom>      screen pixels, one control a line, in the order they were drawn
#include "imgui.h"
#include "imgui_internal.h"
#include <Windows.h>
#include <array>
#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct ControlList {
    const ImGuiContext* context{}; // the one the lists below are from: the overlay makes a new one at a level load
    int frame{-1};
    std::unordered_map<ImGuiID, ImRect> boxes;          // this frame's, by ID
    std::vector<std::pair<std::string, ImGuiID>> named; // this frame's controls with an ID name, in drawing order
    unsigned items{};
    ULONGLONG written_at{};
};
ControlList& controls() {
    static auto* list = new ControlList;
    return *list;
}
// ReSkate.Tricks/research beside the running exe, as the studio finds ReSkate.Tricks (the launcher's lands beside it).
std::filesystem::path research_folder() {
    std::array<wchar_t, 32768> buffer{};
    if (const auto length = GetEnvironmentVariableW(L"RESKATE_ANIMATION_STUDIO_DIR", buffer.data(), static_cast<DWORD>(buffer.size()));
        length && length < buffer.size())
        return std::filesystem::path(std::wstring(buffer.data(), length)) / L"research";
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), count)).parent_path() / L"ReSkate.Tricks" / L"research";
}
ImVec2 to_screen(ImVec2 client) {
    POINT p{static_cast<LONG>(client.x), static_cast<LONG>(client.y)};
    if (const auto* viewport = ImGui::GetMainViewport(); viewport && viewport->PlatformHandleRaw)
        ClientToScreen(static_cast<HWND>(viewport->PlatformHandleRaw), &p);
    return ImVec2(static_cast<float>(p.x), static_cast<float>(p.y));
}
// When a new frame starts, the last one's controls go to the file, so each list is one whole frame. Timed by the wall
// clock: a new context's own clock starts again from nothing.
void start_frame(ImGuiContext& g) {
    auto& list = controls();
    if (&g == list.context && g.FrameCount == list.frame) return;
    const auto now = GetTickCount64();
    if (&g == list.context && now >= list.written_at + 500) {
        std::string text = std::format("# frame {}: {} items, {} with an ID name\n", list.frame, list.items, list.named.size());
        for (const auto& [name, id] : list.named) {
            const auto box = list.boxes.find(id);
            if (box == list.boxes.end()) continue;
            const auto a = to_screen(box->second.Min), b = to_screen(box->second.Max);
            text += std::format("{}\t{:.0f},{:.0f},{:.0f},{:.0f}\n", name, a.x, a.y, b.x, b.y);
        }
        std::error_code error;
        const auto folder = research_folder();
        std::filesystem::create_directories(folder, error);
        std::ofstream(folder / L"ui-ids.txt", std::ios::binary | std::ios::trunc) << text;
        list.written_at = now;
    }
    list.context = &g;
    list.frame = g.FrameCount;
    list.boxes.clear();
    list.named.clear();
    list.items = 0;
}
// The hooks only run while the context asks for them. Nothing in the overlay sets that, so it is set here on whatever
// context is current: a plain flag, read by ImGui on the render thread. Again and again, as the overlay makes its
// context again at a level load, and its context is current only while a frame is drawn.
const bool hooks_switched_on = [] {
    std::thread([] {
        for (;;) {
            if (auto* context = ImGui::GetCurrentContext(); context && !context->TestEngineHookItems) context->TestEngineHookItems = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
    return true;
}();
} // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    start_frame(*ctx);
    ++controls().items;
    controls().boxes[id] = bb;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    start_frame(*ctx);
    if (label && *label) controls().named.emplace_back(label, id);
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
