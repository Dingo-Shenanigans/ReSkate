#include "native_export.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"
#include <Windows.h>
#include <array>
#include <fstream>
#include <stdexcept>

namespace dingosdk::graphics_creator {
namespace {
constexpr auto board_donor_item = "items/cust_board/own_deckgraphic_baker_popsicle_00001";
constexpr auto board_donor_texture =
    "characters/skateboard/licensed/deck/common/baker/graphic/"
    "deck_baker_graphic_00001/2026/deck_baker_graphic_00001_c";

std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> configured{};
    const auto configured_length = GetEnvironmentVariableW(L"RESKATE_GAME_ROOT", configured.data(),
        static_cast<DWORD>(configured.size()));
    if (configured_length && configured_length < configured.size())
        return std::filesystem::path(std::wstring(configured.data(), configured_length));
    std::array<wchar_t, 32768> value{};
    const auto length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) throw std::runtime_error("The Skate installation folder is unavailable.");
    return std::filesystem::path(std::wstring(value.data(), length)).parent_path();
}

std::filesystem::path configured_cli() {
    std::array<wchar_t, 32768> value{};
    const auto length = GetEnvironmentVariableW(L"RESKATE_STUDIO_CLI", value.data(), static_cast<DWORD>(value.size()));
    if (length && length < value.size()) return std::filesystem::path(std::wstring(value.data(), length));

    const auto game = executable_directory();
    const std::array candidates{
        game / L"ReSkate Studio" / L"reskate_cli.exe",
        game.parent_path() / L"ReSkate Studio" / L"reskate_cli.exe",
        game / L"reskate_cli.exe"
    };
    for (const auto& candidate : candidates)
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    return {};
}

std::wstring quoted(const std::filesystem::path& value) {
    return L"\"" + value.wstring() + L"\"";
}
std::wstring quoted(std::string_view value) {
    return L"\"" + std::wstring(value.begin(), value.end()) + L"\"";
}

bool run_encoder(const std::filesystem::path& cli, const std::filesystem::path& game,
                 const std::filesystem::path& image, const std::filesystem::path& output,
                 std::string& problem) {
    auto command = quoted(cli) + L" texture-clone-test " + quoted(game) + L" " +
        quoted(std::string_view{board_donor_texture}) + L" " + quoted(image) + L" 0 " + quoted(output);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(cli.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, cli.parent_path().c_str(), &startup, &process) != FALSE;
    if (!started) {
        problem = "ReSkate Studio's native texture encoder could not be started.";
        return false;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 120000);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
    else GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || code != 0 || !std::filesystem::is_regular_file(output)) {
        problem = wait == WAIT_TIMEOUT ? "The native texture encoder timed out." :
            "ReSkate Studio rejected this graphic during native texture encoding.";
        return false;
    }
    return true;
}

void publish_manifest(const std::filesystem::path& destination, const Json& manifest) {
    std::filesystem::create_directories(destination.parent_path());
    auto temporary = destination;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        const auto text = manifest.dump(2);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) throw std::runtime_error("The native export manifest could not be written.");
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("The native export manifest could not be published.");
    }
}
} // namespace

NativeExportResult stage_native_export(const GraphicDocument& document,
                                       const std::filesystem::path& exported_png) {
    NativeExportResult result;
    result.manifest = exported_png;
    result.manifest.replace_extension(L"native-export.json");
    Json manifest{{"schema", "ReSkate.Create.NativeExport.v1"},
                  {"graphic", document.name},
                  {"source_png", path_utf8(exported_png)},
                  {"source_canvas", Json{{"width", document.canvas.width}, {"height", document.canvas.height}}},
                  {"target", document.target == DesignTarget::board ? "board" : "unsupported"},
                  {"state", "png-only"}};

    if (document.target != DesignTarget::board) {
        result.message = "PNG exported. Native clothing and sticker donors are not wired yet.";
        manifest["message"] = result.message;
        publish_manifest(result.manifest, manifest);
        return result;
    }

    const auto cli = configured_cli();
    if (cli.empty()) {
        result.message = "PNG exported. Install ReSkate Studio or set RESKATE_STUDIO_CLI to stage the native board texture.";
        manifest["message"] = result.message;
        publish_manifest(result.manifest, manifest);
        return result;
    }

    result.attempted = true;
    result.encoded_preview = exported_png;
    result.encoded_preview.replace_extension(L"board-frostbite-preview.png");
    std::string problem;
    result.validated = run_encoder(cli, executable_directory(), exported_png, result.encoded_preview, problem);
    manifest["studio_cli"] = path_utf8(cli);
    manifest["donor_item"] = board_donor_item;
    manifest["donor_texture"] = board_donor_texture;
    manifest["encoded_preview"] = path_utf8(result.encoded_preview);
    manifest["authoring_request"] = Json{
        {"kind", "board_bottomart"},
        {"item_asset", "items/board_bottomart/own_reskate_create_" + document.id},
        {"appearance_asset", "characters/customization/reskate/board_bottomart/reskate_create_" + document.id + "_ap"},
        {"display_name", document.name},
        {"texture_role", "base_color"}
    };
    manifest["state"] = result.validated ? "frostbite-texture-validated" : "frostbite-texture-failed";
    manifest["packaging_state"] = "studio-authoring-required";
    result.message = result.validated ?
        "Board artwork passed Studio's BC7/mip encoder. Native cosmetic authoring and FBMOD packaging are still required." : problem;
    manifest["message"] = result.message;
    publish_manifest(result.manifest, manifest);
    return result;
}

} // namespace dingosdk::graphics_creator
