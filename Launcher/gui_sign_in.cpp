#include "gui_internal.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// The Steam panels: sign-in, password/Steam Guard prompts and the QR code.
namespace dingosdk::launcher_gui::detail {
namespace {

void wipe(std::span<char> buffer) { SecureZeroMemory(buffer.data(), buffer.size()); }

// A caption over a box, as the settings have their groups.
void caption(const Fonts& fonts, const char* text) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(5)));
    ImGui::PushFont(fonts.caption);
    ImGui::TextDisabled("%s", text);
    ImGui::PopFont();
    ImGui::PopStyleVar();
}

// The box a group of the panel's rows sits in, `tall` high at the cursor and as wide as the panel's
// room; with `accent`, its left edge in that colour. Leaves the cursor where it was.
void box(float tall, ImU32 accent = 0) {
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos(), end(start.x + ImGui::GetContentRegionAvail().x, start.y + tall);
    draw->AddRectFilled(start, end, rgba(31, 31, 34), S(6));
    draw->AddRect(start, end, color::outline, S(6));
    if (accent) draw->AddRectFilled(start, ImVec2(start.x + S(4), end.y), accent, S(6), ImDrawFlags_RoundCornersLeft);
}

// An on/off row across the panel's room, `tall` high: its label over a line about it, and the
// switch at its right end. The whole row is what is pressed. `line`: a rule over it.
void switch_row(const Fonts& fonts, const char* label, const char* note, bool& value, float tall, bool line) {
    ImGui::PushID(label);
    auto* draw = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorScreenPos(), end(start.x + width, start.y + tall);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    if (ImGui::InvisibleButton("##row", ImVec2(width, tall), ImGuiButtonFlags_EnableNav)) value = !value;
    ImGui::PopStyleVar();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (line) draw->AddLine(ImVec2(start.x + S(1), start.y), ImVec2(end.x - S(1), start.y), color::outline);
    if (hovered) draw->AddRectFilled(ImVec2(start.x + S(1), start.y + S(1)), ImVec2(end.x - S(1), end.y - S(1)), rgba(255, 255, 255, 0.03f));
    const float block = fonts.body->FontSize + (*note ? S(3) + fonts.caption->FontSize : 0.0f);
    const float y = start.y + (tall - block) * 0.5f;
    draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(start.x + S(16), y), color::text, label);
    if (*note) draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(start.x + S(16), y + fonts.body->FontSize + S(3)), color::muted, note);
    const auto mark = switch_size();
    draw_switch(draw, ImVec2(end.x - S(16) - mark.x, start.y + (tall - mark.y) * 0.5f), value, hovered);
    ImGui::PopID();
}

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

// Signing in to Steam, which the game is downloaded from: by a QR code scanned with the Steam app,
// offered first because nothing is typed for it, or by the account's name and password. Each is a
// box of its own, as the settings' groups are.
void sign_in_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, Ui& ui) {
    const auto panel = begin_panel("##sign_in_panel", size, ImVec2(S(540), std::min(S(540), size.y - S(40))));
    auto& settings = launcher.settings();
    auto* draw = ImGui::GetWindowDrawList();
    panel_title(fonts, "SIGN IN TO STEAM");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("skate. is downloaded from Steam, so sign in with the account that owns it.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(6)));
    const auto start = [&](bool qr) {
        if (!qr) settings.steam_username = ui.username.data();
        launcher.save();
        launcher.download(ui.sign_in_validate, qr, qr ? std::string() : std::string(ui.password.data()));
        wipe(ui.password);
        ui.sign_in = false;
    };
    const float inner = ImGui::GetContentRegionAvail().x;

    // ------------------------------------------------ with the Steam app: one row, and its button
    caption(fonts, "WITH THE STEAM APP");
    {
        const float tall = S(64), button = S(170), frame = S(36);
        box(tall, color::blue);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float block = fonts.bold->FontSize + S(4) + fonts.body->FontSize;
        const float y = at.y + (tall - block) * 0.5f;
        draw->AddText(fonts.bold, fonts.bold->FontSize, ImVec2(at.x + S(18), y), color::text, "Scan a QR code");
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(at.x + S(18), y + fonts.bold->FontSize + S(4)), color::muted,
                      "The quick way: nothing to type.");
        ImGui::SetCursorScreenPos(ImVec2(at.x + inner - S(14) - button, at.y + (tall - frame) * 0.5f));
        push_primary_button();
        const bool qr = ImGui::Button("SHOW THE CODE", ImVec2(button, frame));
        pop_primary_button();
        if (qr) start(true);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + tall));
        ImGui::Dummy(ImVec2(inner, S(8)));
    }

    // ------------------------------------------------ with the account: its name and password, and how it is remembered
    caption(fonts, "OR WITH YOUR ACCOUNT");
    {
        const float pad = S(14), field = ImGui::GetFrameHeight(), row = S(48);
        const float fields = pad + field + S(8) + field + pad;
        box(fields + row * 2);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad));
        if (ui.focus) { ImGui::SetKeyboardFocusHere(); ui.focus = false; }
        ImGui::SetNextItemWidth(inner - pad * 2);
        ImGui::InputTextWithHint("##username", "Steam account name", ui.username.data(), ui.username.size(),
            ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetCursorScreenPos(ImVec2(at.x + pad, at.y + pad + field + S(8)));
        ImGui::SetNextItemWidth(inner - pad * 2);
        const bool entered = ImGui::InputTextWithHint("##password", "Password (empty if remembered)", ui.password.data(),
            ui.password.size(), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + fields));
        switch_row(fonts, "Remember me on this PC", "Steam keeps the sign-in, so the password is not asked for again.", settings.steam_remember, row, true);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + fields + row));
        switch_row(fonts, "Type a Steam Guard code", "Instead of approving the sign-in in the Steam app.", settings.steam_prefer_code, row, true);
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + fields + row * 2));
        ImGui::Dummy(ImVec2(inner, S(6)));
        ImGui::PushFont(fonts.caption);
        ImGui::TextDisabled("Your password goes straight to Steam and is never saved.");
        ImGui::PopFont();

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
    }
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

// The QR code to scan with the Steam app: the code on white at the left, and beside it what to
// do with it, a step to a row, with that the launcher is waiting under them.
void qr_window(Launcher& launcher, const Fonts& fonts, ImVec2 size, const std::vector<std::string>& rows) {
    std::size_t first = SIZE_MAX, last = 0;
    for (const auto& row : rows) {
        const auto begin = row.find('#'), end = row.rfind('#');
        if (begin != std::string::npos) { first = std::min(first, begin); last = std::max(last, end); }
    }
    if (first == SIZE_MAX) return;
    const float columns = static_cast<float>(last - first + 1);
    const float module = std::floor(S(236) / columns);
    const float code = module * columns, code_tall = module * static_cast<float>(rows.size());
    const float quiet = module * 3;
    const float card = code + quiet * 2, card_tall = code_tall + quiet * 2;
    const float pad = S(28), gap = S(24), steps_width = S(300);
    const auto panel = begin_panel("##qr_panel", size, ImVec2(pad * 2 + card + gap + steps_width, card_tall + S(196)));
    panel_title(fonts, "SCAN WITH STEAM");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Sign in with the account that owns skate. Nothing is typed here.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(8)));
    auto* draw = ImGui::GetWindowDrawList();

    // ------------------------------------------------ the code, on white with its quiet margin
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(origin, ImVec2(origin.x + card, origin.y + card_tall), rgba(255, 255, 255), S(6));
    for (std::size_t y = 0; y < rows.size(); ++y)
        for (std::size_t x = first; x <= last && x < rows[y].size(); ++x)
            if (rows[y][x] == '#') {
                const ImVec2 cell(origin.x + quiet + static_cast<float>(x - first) * module,
                                  origin.y + quiet + static_cast<float>(y) * module);
                draw->AddRectFilled(cell, ImVec2(cell.x + module, cell.y + module), rgba(0, 0, 0));
            }

    // ------------------------------------------------ what to do with it: a step to a row
    {
        static constexpr std::array<const char*, 3> steps{"Open the Steam app on your phone", "Tap the shield, Steam Guard", "Scan this code and approve"};
        const float row = S(50);
        const ImVec2 box_at(origin.x + card + gap, origin.y), box_end(box_at.x + steps_width, box_at.y + row * static_cast<float>(steps.size()));
        draw->AddRectFilled(box_at, box_end, rgba(31, 31, 34), S(6));
        draw->AddRect(box_at, box_end, color::outline, S(6));
        draw->AddRectFilled(box_at, ImVec2(box_at.x + S(4), box_end.y), color::blue, S(6), ImDrawFlags_RoundCornersLeft);
        for (std::size_t at = 0; at < steps.size(); ++at) {
            const float top = box_at.y + row * static_cast<float>(at);
            if (at) draw->AddLine(ImVec2(box_at.x + S(5), top), ImVec2(box_end.x - S(1), top), color::outline);
            const auto number = std::to_string(at + 1);
            draw->AddText(fonts.heading, fonts.heading->FontSize, ImVec2(box_at.x + S(18), top + (row - fonts.heading->FontSize) * 0.5f), color::blue,
                          number.c_str());
            const ImVec4 clip(box_at.x + S(44), top, box_end.x - S(12), top + row);
            draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(box_at.x + S(44), top + (row - fonts.body->FontSize) * 0.5f), color::text,
                          steps[at], nullptr, 0, &clip);
        }
        // Under them: that it is waiting, and for how long a code is good.
        const float y = box_end.y + S(18);
        skate_theme::status_icon(draw, ImVec2(box_at.x + S(12), y + fonts.body->FontSize * 0.5f), skate_theme::Icon::busy,
                                 static_cast<float>(ImGui::GetTime()), g_scale);
        draw->AddText(fonts.body, fonts.body->FontSize, ImVec2(box_at.x + S(30), y), color::text, "Waiting for the scan...");
        draw->AddText(fonts.caption, fonts.caption->FontSize, ImVec2(box_at.x + S(30), y + fonts.body->FontSize + S(5)), color::muted,
                      "A new code is drawn when this one runs out.", nullptr, steps_width - S(30));
    }

    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(panel.x - pad - S(110));
    if (ImGui::Button("CANCEL", ImVec2(S(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) launcher.cancel();
    ImGui::End();
}

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
    if (launcher_app::under_wine()) {
        ImGui::Spacing();
        ImGui::TextDisabled("On Linux or macOS, start skate. from your Steam library with ReSkate's launch option "
                            "(see the README), so the launcher can find Steam's account.");
    }
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosY(panel.y - S(24) - ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(panel.x - S(28) - S(110));
    if (ImGui::Button("OK", ImVec2(S(110), 0))) ui.steam_offline = false;
    ImGui::End();
}

} // namespace dingosdk::launcher_gui::detail
