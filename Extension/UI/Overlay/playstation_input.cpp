#include "playstation_input.h"
#include "input_capture.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dingosdk::overlay::detail {
// Here rather than in input_capture.cpp: this reader is also built on its own, without the
// rest of the overlay.
thread_local unsigned overlay_input_access = 0;
namespace {
struct Pad {
    std::wstring path;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::vector<std::uint8_t> buffer;
    bool pending{}, bluetooth{};
    PlayStationPad kind{};
    std::optional<PlayStationGamepad> latest;
    ULONGLONG latest_time{};
    ~Pad() {
        if (file != INVALID_HANDLE_VALUE) {
            // The buffer must outlive any read the kernel still owns.
            if (pending) { CancelIoEx(file, &overlapped); DWORD ignored; GetOverlappedResult(file, &overlapped, &ignored, TRUE); }
            CloseHandle(file);
        }
        if (event) CloseHandle(event);
    }
};
struct Pads {
    std::mutex mutex;
    std::vector<std::unique_ptr<Pad>> open;
    ULONGLONG next_scan{};
    unsigned generation{};
    // Readers set this when a scan is due; the scanner thread waits on it (null until it runs).
    HANDLE wake{};
};
Pads& pads() { static auto* value = new Pads; return *value; } // process lifetime; never torn down under the loader lock

std::vector<std::wstring> sony_hid_paths() {
    GUID hid{};
    HidD_GetHidGuid(&hid);
    std::vector<wchar_t> list;
    for (int attempt = 0; attempt < 4; ++attempt) {
        ULONG length{};
        if (CM_Get_Device_Interface_List_SizeW(&length, &hid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return {};
        list.assign(length ? length : 1, L'\0');
        const auto result = CM_Get_Device_Interface_ListW(&hid, nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_SUCCESS) break;
        if (result != CR_BUFFER_SMALL) return {};
        list.clear();
    }
    std::vector<std::wstring> paths;
    for (const wchar_t* entry = list.data(); entry && *entry; entry += wcslen(entry) + 1) {
        std::wstring lower(entry);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        // USB: "vid_054c"; Bluetooth: "_vid&0002054c". Confirmed with HidD_GetAttributes on open.
        if (lower.find(L"054c") != std::wstring::npos) paths.emplace_back(entry);
    }
    return paths;
}

std::unique_ptr<Pad> open_pad(const std::wstring& path) {
    auto pad = std::make_unique<Pad>();
    pad->path = path;
    pad->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pad->file == INVALID_HANDLE_VALUE) return {}; // hidden, or held exclusively by DS4Windows and similar
    HIDD_ATTRIBUTES attributes{sizeof(attributes)};
    if (!HidD_GetAttributes(pad->file, &attributes)) return {};
    pad->kind = playstation_pad(attributes.VendorID, attributes.ProductID);
    if (pad->kind == PlayStationPad::none) return {};
    PHIDP_PREPARSED_DATA preparsed{};
    if (!HidD_GetPreparsedData(pad->file, &preparsed)) return {};
    HIDP_CAPS caps{};
    const bool caps_ready = HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
    HidD_FreePreparsedData(preparsed);
    // The controller also exposes vendor collections; only the gamepad one carries input reports.
    if (!caps_ready || caps.UsagePage != 0x01 || caps.Usage != 0x05 || caps.InputReportByteLength < 10) return {};
    pad->bluetooth = playstation_bluetooth(caps.InputReportByteLength);
    pad->buffer.resize(caps.InputReportByteLength);
    pad->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pad->event) return {};
    pad->overlapped.hEvent = pad->event;
    logging::printf(logging::Level::info, logging::Channel::input,
        "PlayStation controller opened: product=%04x %s report=%u",
        attributes.ProductID, pad->bluetooth ? "bluetooth" : "usb", caps.InputReportByteLength);
    return pad;
}

// Drains every queued report so the newest one wins. Returns false once the
// device is gone.
bool drain(Pad& pad) {
    OverlayInputAccess access; // This independent HID stream belongs to freecam/menu.
    for (int reports = 0; reports < 256; ++reports) {
        if (!pad.pending) {
            ResetEvent(pad.event);
            if (!ReadFile(pad.file, pad.buffer.data(), static_cast<DWORD>(pad.buffer.size()), nullptr, &pad.overlapped) &&
                GetLastError() != ERROR_IO_PENDING) return false;
            pad.pending = true;
        }
        DWORD size{};
        if (!GetOverlappedResult(pad.file, &pad.overlapped, &size, FALSE))
            return GetLastError() == ERROR_IO_INCOMPLETE;
        pad.pending = false;
        if (const auto parsed = parse_playstation_report(pad.kind, pad.bluetooth, pad.buffer.data(), size)) {
            pad.latest = parsed;
            pad.latest_time = GetTickCount64();
        }
    }
    return true;
}

// Opens the Sony pads that are not open yet. The enumeration and each open take
// milliseconds, so they run on the scanner thread without the lock: readers, the
// client tick among them, never wait for a scan.
void scan(Pads& p) {
    std::vector<std::wstring> known;
    {
        std::lock_guard lock(p.mutex);
        for (const auto& pad : p.open) known.push_back(pad->path);
    }
    std::vector<std::unique_ptr<Pad>> found;
    for (const auto& path : sony_hid_paths()) {
        if (std::find(known.begin(), known.end(), path) != known.end()) continue;
        if (auto pad = open_pad(path)) found.push_back(std::move(pad));
    }
    if (found.empty()) return;
    std::lock_guard lock(p.mutex);
    for (auto& pad : found) {
        if (std::any_of(p.open.begin(), p.open.end(), [&](const auto& existing) { return existing->path == pad->path; })) continue;
        p.open.push_back(std::move(pad));
        ++p.generation;
    }
}

DWORD WINAPI scanner(void* parameter) noexcept {
    auto& p = *static_cast<Pads*>(parameter);
    for (;;) {
        WaitForSingleObject(p.wake, INFINITE);
        try { scan(p); } catch (...) {}
    }
}
}

PlayStationSample read_playstation_pads() {
    auto& p = pads();
    std::lock_guard lock(p.mutex);
    const auto now = GetTickCount64();
    if (now >= p.next_scan) {
        // Hot-plug: look for new pads every few seconds while someone reads them.
        // Enumeration is one configuration-manager call; opening happens only for
        // Sony paths. Both run on the scanner thread, started by the first read.
        p.next_scan = now + 3000;
        if (!p.wake) {
            p.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            const auto thread = p.wake ? CreateThread(nullptr, 0, scanner, &p, 0, nullptr) : nullptr;
            if (thread) CloseHandle(thread);
            else if (p.wake) {
                CloseHandle(p.wake); // tried again at the next scan
                p.wake = nullptr;
            }
        }
        if (p.wake) SetEvent(p.wake);
    }
    PlayStationSample sample;
    for (auto it = p.open.begin(); it != p.open.end();) {
        if (!drain(**it)) {
            logging::write(logging::Level::info, logging::Channel::input, "PlayStation controller disconnected.");
            it = p.open.erase(it); ++p.generation; continue;
        }
        const auto& pad = **it;
        sample.present = true;
        if (sample.style == ControllerStyle::xbox)
            sample.style = pad.kind == PlayStationPad::dualsense ? ControllerStyle::dualsense : ControllerStyle::dualshock4;
        // Pads stream continuously; silence means asleep or out of range.
        if (pad.latest && now - pad.latest_time < 1000) {
            sample.available = true;
            sample.pad.buttons |= pad.latest->buttons;
            sample.pad.left_trigger = std::max(sample.pad.left_trigger, pad.latest->left_trigger);
            sample.pad.right_trigger = std::max(sample.pad.right_trigger, pad.latest->right_trigger);
            // First responsive pad drives the sticks.
            if (!sample.pad.left_x && !sample.pad.left_y) { sample.pad.left_x = pad.latest->left_x; sample.pad.left_y = pad.latest->left_y; }
            if (!sample.pad.right_x && !sample.pad.right_y) { sample.pad.right_x = pad.latest->right_x; sample.pad.right_y = pad.latest->right_y; }
        }
        ++it;
    }
    sample.generation = p.generation;
    return sample;
}
}
