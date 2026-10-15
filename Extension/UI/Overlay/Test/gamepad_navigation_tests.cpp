#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../gamepad_navigation.h"
#include "../menu_navigation.h"
#include <cassert>
#include <iostream>

static int first_clicks{}, second_clicks{};
static ImGuiWindow* sidebar{}, *content{}, *nested{};
static bool enter_nested{}, open_options{}, focus_sidebar{};
static void menu_frame() {
    using namespace dingosdk::overlay::menu;
    claim_menu_back(true);
    ImGui::NewFrame();
    ImGui::Begin("Back navigation");
    menu_back(sidebar, content);
    ImGui::BeginChild("Sidebar", ImVec2(180, 400));
    sidebar = ImGui::GetCurrentWindow();
    if (focus_sidebar) { focus_menu_panel(sidebar,true); focus_sidebar = false; }
    const bool enter = ImGui::InvisibleButton("Page", ImVec2(160,40), ImGuiButtonFlags_EnableNav);
    ImGui::SetItemDefaultFocus();
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("Content", ImVec2(400,400));
    content = ImGui::GetCurrentWindow();
    if (enter) focus_menu_panel(content,true);
    ImGui::Button("Page tab");
    ImGui::BeginChild("Nested options", ImVec2(300,200));
    nested = ImGui::GetCurrentWindow();
    if (enter_nested) { focus_menu_panel(nested,true); enter_nested = false; }
    ImGui::Button("Setting");
    if (open_options) { ImGui::OpenPopup("Options"); open_options = false; }
    if (ImGui::BeginPopup("Options")) { ImGui::Button("Popup option"); ImGui::EndPopup(); }
    ImGui::EndChild(); ImGui::EndChild(); ImGui::End(); ImGui::Render();
}
static void frame() {
    ImGui::NewFrame();
    ImGui::Begin("Controller navigation");
    // The Insert sidebar, category tabs and option tiles are custom-drawn
    // InvisibleButtons. Their default flags exclude navigation entirely.
    if (ImGui::InvisibleButton("First", ImVec2(160,40), ImGuiButtonFlags_EnableNav)) ++first_clicks;
    ImGui::SetItemDefaultFocus();
    if (ImGui::InvisibleButton("Second", ImVec2(160,40), ImGuiButtonFlags_EnableNav)) ++second_clicks;
    ImGui::End(); ImGui::Render();
}
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigInputTrickleEventQueue = false;
    io.DisplaySize = ImVec2(1000,1000); io.DeltaTime = 1.f/60;
    io.IniFilename = nullptr;
    unsigned char* pixels{}; int width{},height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    using dingosdk::overlay::detail::feed_gamepad_navigation;
    feed_gamepad_navigation(io,true,0x1002,0,0,0,0); frame();
    assert(io.BackendFlags & ImGuiBackendFlags_HasGamepad);
    assert(ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));
    assert(ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown));
    feed_gamepad_navigation(io,true,0,32767,-32768,255,0); frame();
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));
    assert(ImGui::IsKeyDown(ImGuiKey_GamepadLStickRight));
    assert(ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown));
    assert(ImGui::IsKeyDown(ImGuiKey_GamepadL2));
    feed_gamepad_navigation(io,true,0,4000,-4000,20,20); frame();
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadLStickRight));
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown));
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadL2));
    feed_gamepad_navigation(io,true,0x2000,-32768,32767,0,255); frame();
    assert(ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight));
    assert(ImGui::IsKeyDown(ImGuiKey_GamepadLStickLeft));
    feed_gamepad_navigation(io,false,0xffff,-32768,32767,255,255); frame();
    assert(!(io.BackendFlags & ImGuiBackendFlags_HasGamepad));
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight));
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadLStickLeft));
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadR2));
    first_clicks = second_clicks = 0;
    ImGui::SetWindowFocus("Controller navigation");
    for (int i=0;i<3;++i) { feed_gamepad_navigation(io,true,0,0,0,0,0); frame(); }
    feed_gamepad_navigation(io,true,0x0002,0,0,0,0); frame();
    for (int i=0;i<2;++i) { feed_gamepad_navigation(io,true,0,0,0,0,0); frame(); }
    feed_gamepad_navigation(io,true,0x1000,0,0,0,0); frame();
    feed_gamepad_navigation(io,true,0,0,0,0,0); frame();
    assert(first_clicks == 0 && second_clicks == 1);
    ImGui::SetWindowFocus(nullptr);
    for (int i=0;i<3;++i) { feed_gamepad_navigation(io,true,0,0,0,0,0); menu_frame(); }
    focus_sidebar = true;
    menu_frame(); menu_frame();
    assert(ImGui::GetCurrentContext()->NavWindow == sidebar);
    feed_gamepad_navigation(io,true,0x1000,0,0,0,0); menu_frame();
    feed_gamepad_navigation(io,true,0,0,0,0,0); menu_frame();
    assert(ImGui::GetCurrentContext()->NavWindow == content);
    enter_nested = true; menu_frame(); menu_frame();
    assert(ImGui::GetCurrentContext()->NavWindow == nested);
    open_options = true; menu_frame(); menu_frame();
    assert(!ImGui::GetCurrentContext()->OpenPopupStack.empty());
    feed_gamepad_navigation(io,true,0x2000,0,0,0,0); menu_frame();
    assert(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    assert(ImGui::GetCurrentContext()->NavWindow == nested);
    feed_gamepad_navigation(io,true,0,0,0,0,0); menu_frame();
    feed_gamepad_navigation(io,true,0x2000,0,0,0,0); menu_frame();
    assert(ImGui::GetCurrentContext()->NavWindow == content);
    feed_gamepad_navigation(io,true,0,0,0,0,0); menu_frame();
    feed_gamepad_navigation(io,true,0x2000,0,0,0,0); menu_frame();
    assert(ImGui::GetCurrentContext()->NavWindow == sidebar);
    feed_gamepad_navigation(io,true,0,0,0,0,0); menu_frame();
    feed_gamepad_navigation(io,true,0x2000,0,0,0,0); menu_frame();
    assert(ImGui::GetCurrentContext()->NavWindow == sidebar);
    ImGui::DestroyContext();
    std::cout << "Controller navigation: buttons, analog, deadzones, ownership release and D-pad/A widget interaction passed\n";
}
