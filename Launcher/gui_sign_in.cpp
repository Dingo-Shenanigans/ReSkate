#include "gui_internal.h"

#include <cmath>
#include <cstdint>
#include <span>

// The Steam panels: sign-in, password/Steam Guard prompts and the QR code.
namespace dingosdk::launcher_gui::detail {
#if !RESKATE_STEAM_DOWNLOADS_DISABLED
namespace {

void wipe(std::span<char> buffer) { SecureZeroMemory(buffer.data(), buffer.size()); }

} // namespace

void open_sign_in(Launcher& launcher, Ui& ui, bool validate) {
    ui.sign_in = true;
    ui.sign_in_validate = validate;
    ui.focus = true;
    wipe(ui.password);
    ui.username.fill(0);
    const auto& saved = launcher.settings().steam_username;
    std::copy_n(saved.begin(), std::min(saved.size(), ui.username.size() - 1), ui.username.begin());
}

void sign_in_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui) {
    const auto panel = begin_panel("##sign_in_panel", size, ImVec2(S(480), S(520)));
    auto& settings = launcher.settings();
    panel_title(fonts, "SIGN IN TO STEAM");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("skate. is downloaded from Steam with DepotDownloader, so the account must own it. "
                        "Your password is passed straight to Steam and never saved.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const auto start = [&](bool qr) {
        if (!qr) settings.steam_username = ui.username.data();
        launcher.save();
        launcher.download(ui.sign_in_validate, qr, qr ? std::string() : std::string(ui.password.data()));
        wipe(ui.password);
        ui.sign_in = false;
    };
    push_primary_button();
    const bool qr = ImGui::Button("SCAN A QR CODE WITH THE STEAM APP", ImVec2(-1, S(46)));
    pop_primary_button();
    if (qr) start(true);
    ImGui::Spacing();
    ImGui::PushFont(fonts.caption);
    ImGui::TextDisabled("OR SIGN IN WITH YOUR ACCOUNT");
    ImGui::PopFont();
    if (ui.focus) { ImGui::SetKeyboardFocusHere(); ui.focus = false; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##username", "Steam account name", ui.username.data(), ui.username.size(),
        ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##password", "Password (empty if remembered)", ui.password.data(),
        ui.password.size(), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Checkbox("Remember me on this PC", &settings.steam_remember);
    ImGui::Checkbox("Type a Steam Guard code instead of approving in the app", &settings.steam_prefer_code);
    const std::string_view username(ui.username.data());
    const bool valid = !username.empty() && username.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string_view::npos;
    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    if (ImGui::Button("CANCEL", ImVec2(S(110), 0))) { wipe(ui.password); ui.sign_in = false; }
    ImGui::SameLine(panel.x - S(28) - S(130));
    ImGui::BeginDisabled(!valid);
    push_primary_button();
    const bool sign_in = ImGui::Button("SIGN IN", ImVec2(S(130), 0));
    pop_primary_button();
    if (sign_in || (entered && valid)) start(false);
    ImGui::EndDisabled();
    ImGui::End();
}

void prompt_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, const update::Prompt& prompt, Ui& ui) {
    const auto panel = begin_panel("##prompt_panel", size, ImVec2(S(440), S(300)));
    const bool password = prompt.kind == update::PromptKind::password;
    panel_title(fonts, password ? "STEAM PASSWORD" : "STEAM GUARD");
    ImGui::PushTextWrapPos(0);
    if (prompt.retry) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger), "That code was incorrect. Try again.");
    switch (prompt.kind) {
    case update::PromptKind::password:
        ImGui::TextDisabled("Enter the password for %s.", launcher.settings().steam_username.c_str());
        break;
    case update::PromptKind::authenticator_code:
        ImGui::TextDisabled("Enter the code shown in the Steam app on your phone (Steam Guard).");
        break;
    case update::PromptKind::email_code: {
        const auto at = prompt.text.find("email at ");
        ImGui::TextDisabled("Enter the code Steam emailed to %s.",
            at == std::string::npos ? "you" : prompt.text.c_str() + at + 9);
        break;
    }
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool submit = false;
    if (password) {
        submit = ImGui::InputText("##prompt_password", ui.password.data(), ui.password.size(),
            ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    } else {
        ImGui::PushFont(fonts.heading);
        submit = ImGui::InputTextWithHint("##prompt_code", "XXXXX", ui.code.data(), ui.code.size(),
            ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopFont();
    }
    const char* value = password ? ui.password.data() : ui.code.data();
    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    if (ImGui::Button("CANCEL", ImVec2(S(110), 0))) {
        wipe(ui.password);
        ui.code.fill(0);
        launcher.answer(std::nullopt);
    }
    ImGui::SameLine(panel.x - S(28) - S(130));
    ImGui::BeginDisabled(!*value);
    push_primary_button();
    const bool proceed = ImGui::Button("CONTINUE", ImVec2(S(130), 0));
    pop_primary_button();
    if ((proceed || submit) && *value) {
        launcher.answer(std::string(value));
        wipe(ui.password);
        ui.code.fill(0);
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void qr_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, const std::vector<std::string>& rows) {
    std::size_t first = SIZE_MAX, last = 0;
    for (const auto& row : rows) {
        const auto begin = row.find('#'), end = row.rfind('#');
        if (begin != std::string::npos) { first = std::min(first, begin); last = std::max(last, end); }
    }
    if (first == SIZE_MAX) return;
    const float columns = static_cast<float>(last - first + 1);
    const float module = std::floor(S(230) / columns);
    const float code = module * columns;
    const float quiet = module * 3;
    const auto panel = begin_panel("##qr_panel", size, ImVec2(S(440), code + quiet * 2 + S(200)));
    panel_title(fonts, "SCAN WITH STEAM");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Open the Steam app on your phone, tap the shield (Steam Guard) and scan this code. "
                        "The account must own skate.");
    ImGui::PopTextWrapPos();
    auto* draw = ImGui::GetWindowDrawList();
    const auto window = ImGui::GetWindowPos();
    const ImVec2 origin(window.x + (panel.x - code - quiet * 2) * 0.5f, ImGui::GetCursorScreenPos().y + S(8));
    draw->AddRectFilled(origin, ImVec2(origin.x + code + quiet * 2, origin.y + module * static_cast<float>(rows.size()) + quiet * 2),
        rgba(255, 255, 255));
    for (std::size_t y = 0; y < rows.size(); ++y)
        for (std::size_t x = first; x <= last && x < rows[y].size(); ++x)
            if (rows[y][x] == '#') {
                const ImVec2 cell(origin.x + quiet + static_cast<float>(x - first) * module,
                                  origin.y + quiet + static_cast<float>(y) * module);
                draw->AddRectFilled(cell, ImVec2(cell.x + module, cell.y + module), rgba(0, 0, 0));
            }
    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    if (ImGui::Button("CANCEL", ImVec2(S(110), 0))) launcher.cancel();
    ImGui::End();
}

#endif

// Shown once when the launcher opens without Steam, so nobody reaches the main
// menu as Unknown Player wondering why multiplayer is missing.
void steam_offline_window(const Fonts& fonts, ImVec2 size, Ui& ui) {
    const auto panel = begin_panel("##steam_offline_panel", size, ImVec2(S(440), S(300)));
    panel_title(fonts, "STEAM IS NOT RUNNING");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Steam is not running, or no account is signed in. PLAY still works, but Skate starts in "
                        "offline mode: you play as Unknown Player and multiplayer is hidden.");
    ImGui::Spacing();
    ImGui::TextDisabled("To play as your own account, start Steam, sign in, and open ReSkate again.");
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    if (ImGui::Button("OK", ImVec2(S(110), 0))) ui.steam_offline = false;
    ImGui::End();
}

} // namespace dingosdk::launcher_gui::detail
