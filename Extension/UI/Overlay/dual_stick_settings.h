#pragma once
#include <Windows.h>
#include <array>
#include <map>
#include <string>
#include <cwchar>
#include <utility>

// Optional companion mod settings. Never install hooks or create a configuration
// when the mod is absent. Resolve beside Skate.exe, independently of working dir.
namespace dingosdk::dual_stick {
inline constexpr wchar_t section[] = L"SessionControls";
struct Choice {
    const wchar_t* key;
    const char* label;
    int fallback;
    std::array<int, 4> values;
    std::array<const char*, 4> labels;
    int count;
};
inline constexpr std::array choices{
    Choice{L"ManualCatch", "Manual catch", 0, {0,1}, {"Off","On"},2},
    Choice{L"DpadCameraSide", "D-pad camera sides", 1, {0,1}, {"Off","On"},2},
    Choice{L"SplitGrindControls", "Foot setup grinds", 1, {0,1}, {"Off","On"},2},
    Choice{L"PopOnRelease", "Pop style", 0, {0,1}, {"Flick","Release"},2},
    Choice{L"OneFootFlips", "One-stick flips", 0, {0,1}, {"Blocked","Allowed"},2},
    Choice{L"StickSteering", "Stick steering", 0, {0,1,2}, {"Off","Front foot","Left stick"},3},
    Choice{L"SwitchInvertX", "Switch flick / scoop directions", 0, {0,1}, {"Standard","Reversed"},2},
    Choice{L"SwitchInvertY", "Switch up / down directions", 0, {0,1}, {"Standard","Reversed"},2},
    Choice{L"DisableHoldFakie", "Disable hold-fakie", 1, {0,1}, {"Off","On"},2},
    Choice{L"RevertDoubleTapMs", "Revert double-tap window", 300, {150,300,450,600}, {"150 ms","300 ms","450 ms","600 ms"},4},
    Choice{L"SpinWindUpSteer", "Crouched trigger handling", 0, {0,1}, {"Normal","Light steers"},2},
    Choice{L"SpinWindUpTriggerPercent", "Wind-up trigger strength", 90, {50,65,80,90}, {"50%","65%","80%","90%"},4},
    Choice{L"RevertInput", "Reverts", 2, {0,1,2}, {"Off","Double tap","Both triggers"},3},
    Choice{L"SpinWindUp", "Spin wind-up", 1, {0,1}, {"Off","On"},2},
    Choice{L"BodyFlips", "Body flips", 1, {0,1}, {"Off","On"},2},
    Choice{L"AirControls", "Air controls", 1, {0,1}, {"Off","On"},2},
    Choice{L"Powerslides", "Powerslides", 1, {0,1}, {"Off","On"},2},
    Choice{L"GrindBumpMs", "Grind bump", 250, {0,250}, {"Off","On"},2}
};
struct KeyLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};
using Section = std::map<std::wstring, std::wstring, KeyLess>;
class Settings {
    std::wstring path_;
public:
    explicit Settings(std::wstring path) : path_(std::move(path)) {}
    static std::wstring game_path() {
        std::array<wchar_t, 32768> buffer{};
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!n || n >= buffer.size()) return {};
        std::wstring path(buffer.data(), n);
        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return {};
        return path.substr(0, slash + 1) + L"session_controls.ini";
    }
    bool available() const {
        if (path_.empty()) return false;
        const DWORD a = GetFileAttributesW(path_.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
    int number(const wchar_t* key, int fallback, const wchar_t* from = section) const {
        return static_cast<int>(GetPrivateProfileIntW(from, key, fallback, path_.c_str()));
    }
    std::wstring text(const wchar_t* key, const wchar_t* fallback, const wchar_t* from = section) const {
        std::array<wchar_t, 256> buffer{};
        GetPrivateProfileStringW(from, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path_.c_str());
        return buffer.data();
    }
    bool read(Section& entries, const wchar_t* from = section) const {
        std::array<wchar_t, 32768> buffer{};
        const DWORD n = GetPrivateProfileSectionW(from, buffer.data(), static_cast<DWORD>(buffer.size()), path_.c_str());
        if (n >= buffer.size() - 2) return false;
        entries.clear();
        for (const wchar_t* p = buffer.data(); *p; p += std::wcslen(p) + 1) {
            const wchar_t* eq = std::wcschr(p, L'=');
            if (eq) entries.emplace(std::wstring(p, eq), eq + 1);
        }
        return true;
    }
    bool write(const Section& entries, const wchar_t* to = section) const {
        if (!available()) return false;
        std::wstring buffer;
        for (const auto& [key, value] : entries) {
            buffer += key + L"=" + value;
            buffer.push_back(L'\0');
        }
        buffer.push_back(L'\0');
        if (entries.empty()) buffer.push_back(L'\0');
        if (buffer.size() >= 32766) return false;
        return WritePrivateProfileSectionW(to, buffer.c_str(), path_.c_str()) != FALSE;
    }
    bool set(const wchar_t* key, int value) const {
        return available() && WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), path_.c_str());
    }
    int layout() const {
        return number(L"FrontBackFoot",1) ? 0 : number(L"SwitchStance",1) != 3 ? 3 : number(L"XLFootDirections",0) ? 2 : 1;
    }
    static void layout_keys(Section& entries, int kind) {
        entries[L"FrontBackFoot"] = kind == 0 ? L"1" : L"0";
        entries[L"SwitchStance"] = kind == 0 ? L"1" : L"3";
        entries[L"XLFootDirections"] = kind == 2 ? L"1" : L"0";
        entries[L"SwitchSource"] = L"4";
        entries[L"PresetFeetSwapped"] = L"0";
    }
    bool set_layout(int kind) const {
        Section entries;
        if (kind < 0 || kind > 2 || !available() || !read(entries)) return false;
        layout_keys(entries,kind);
        return write(entries);
    }
    bool preset(bool xl) const {
        Section entries;
        if (!available() || !read(entries)) return false;
        layout_keys(entries,xl ? 2 : 0);
        for (const auto* key : {L"PopOnRelease",L"OneFootFlips"}) entries[key] = xl ? L"1" : L"0";
        entries[L"StickSteering"] = xl ? L"2" : L"0";
        for (const auto* key : {L"SwitchInvertX",L"SwitchInvertY",L"MirrorFlicks"}) entries[key] = L"0";
        return write(entries);
    }
    bool active(bool xl) const {
        const int flag = xl ? 1 : 0;
        return layout() == (xl ? 2 : 0) && number(L"PopOnRelease",0) == flag &&
            number(L"StickSteering",0) == (xl ? 2 : 0) && number(L"OneFootFlips",0) == flag &&
            number(L"SwitchSource",4) == 4 && !number(L"PresetFeetSwapped",0) &&
            !number(L"SwitchInvertX",0) && !number(L"SwitchInvertY",0) && !number(L"MirrorFlicks",0);
    }
    static std::wstring slot_name(int slot) { return L"CustomPreset" + std::to_wstring(slot); }
    bool filled(int slot) const { return slot >= 1 && slot <= 5 && number(L"PresetSaved",0,slot_name(slot).c_str()) == 1; }
    bool save(int slot) const {
        Section entries;
        if (slot < 1 || slot > 5 || !available() || !read(entries)) return false;
        for (const auto& c : choices) entries[c.key] = std::to_wstring(number(c.key,c.fallback));
        for (const auto* key : {L"AutoStance",L"StockGrabAir",L"StockTransitionGround"})
            entries[key] = std::to_wstring(number(key, std::wcscmp(key,L"AutoStance") == 0 ? 1 : 0));
        entries[L"StickSteeringAngleDegrees"] = std::to_wstring(number(L"StickSteeringAngleDegrees",34));
        entries[L"Stance"] = text(L"Stance",L"regular");
        entries[L"SwitchSource"] = std::to_wstring(number(L"SwitchSource",4));
        for (const auto* key : {L"FrontBackFoot",L"SwitchStance",L"XLFootDirections",L"PresetFeetSwapped",L"MirrorFlicks"})
            entries[key] = std::to_wstring(number(key, std::wcscmp(key,L"FrontBackFoot") == 0 || std::wcscmp(key,L"SwitchStance") == 0 ? 1 : 0));
        entries[L"PresetSaved"] = L"1";
        return write(entries,slot_name(slot).c_str());
    }
    bool load(int slot) const {
        Section current, saved;
        if (!available() || !filled(slot) || !read(current) || !read(saved,slot_name(slot).c_str())) return false;
        // Gameplay only: menu, recording and launch preferences remain global.
        // Merge the supported keys so unrelated tuning and unknown future keys survive.
        for (const auto& c : choices) if (saved.contains(c.key)) current[c.key] = saved[c.key];
        for (const auto* key : {L"FrontBackFoot",L"SwitchStance",L"XLFootDirections",L"PresetFeetSwapped",L"MirrorFlicks",L"SwitchSource",L"StockGrabAir",L"StockTransitionGround",L"StickSteeringAngleDegrees"})
            if (saved.contains(key)) current[key] = saved[key];
        return write(current);
    }
};
} // namespace dingosdk::dual_stick
