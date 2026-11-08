// The pause menu's Mod Manager page (native_menu.cpp's third page), drawn here as the other two are:
// Thunderstore's mods to browse and install, the installed ones to switch on and off, update and
// uninstall, the Apply that puts the changes in the game, and the installed plugins with their pages.
// Included into hub_page.cpp, inside its namespace, for its controls (item, button, field, Scroller)
// and its focus. The work behind it is Extension/Assets/mod_store.h; nothing here reads the disk or
// the network.
//
// BROWSE is a store's grid: the views down the left (Most popular, Recently updated, New,
// All mods, then Thunderstore's categories),
// a search box, SORT and HIDE INSTALLED over a grid of cards (as many across as fit, six or so), and the card picked shown big at the
// right with its main button (install, update, or switch on or off) and FULL PAGE, which opens the
// mod's page with its README. MY MODS is the Mods folder; UPDATES what Thunderstore has newer; PLUGINS
// the Insert menu's PLUGINS page, a plugin's own page opening full size in the page's body.

namespace ms = dingosdk::mod_store;
namespace ts = dingosdk::thunderstore;

enum ModTab : int { browse_tab, installed_tab, updates_tab, plugins_tab };

struct ModPage {
    int tab{browse_tab};
    std::string search, installed_search;
    // The categories shown only and those hidden (Query), kept in the profile; and whether the
    // panel that picks them is open.
    std::vector<std::string> only, hidden;
    bool categories_loaded{}, categories_open{};
    ms::Sort sort{ms::Sort::popular};
    // BROWSE's rail: what the grid shows (one of the rail_ views or a category's name), and whether
    // what is installed is left out of it.
    std::string rail;
    bool hide_installed{};
    // The card the details beside the grid show: its mod (Namespace-Name) and its place in the grid,
    // so a mod listed twice is only ever one card lit; and whether a card has the D-pad's focus this
    // frame (A is its main action then, and the footer says which).
    std::string picked;
    std::size_t picked_at{};
    bool card_focused{};
    int installed_filter{};             // 0 all, 1 on, 2 off, 3 waiting to be applied
    std::string details;                // the package whose page is open (Namespace-Name); empty: none
    // Where it was opened from, to go back to: the tab, and the control (a card, mostly) pressed.
    int return_tab{browse_tab};
    std::uint32_t return_focus{};
    // B and Escape stay the page's this long after leaving a mod's page with one, so the press
    // that left it does not then close the pause menu (hold_native_menu_back).
    ULONGLONG hold_back_until{};
    std::string selected;               // MY MODS: the mod the panel shows (its folder)
    std::string confirm;                // a mod whose UNINSTALL was pressed once
    ULONGLONG confirm_until{};
    // What the page shows, worked out again only when the listing, the mods or the query change.
    std::shared_ptr<const ms::Listing> listing;
    std::shared_ptr<const ms::Mods> mods;
    unsigned listing_serial{~0U}, mods_serial{~0U};
    std::string query_key;
    ms::Front front;
    // BROWSE's grid, and each card's control id beside it (a mod's name, made unique when it is in
    // the grid twice, so the focus is never on two cards).
    std::vector<const ts::Package*> results, updates;
    std::vector<std::uint32_t> card_ids;
    std::vector<std::pair<std::string, int>> categories;
    std::map<std::string, const ts::Package*, std::less<>> by_folder;
    std::size_t listed{};               // every mod BROWSE lists (All mods, and the search box's hint)
    std::string new_since;              // a mod made on or after this day is NEW (YYYY-MM-DD)
    std::string search_hint, updates_label;
    unsigned progress_serial{};
    ms::Progress progress;              // the queue as it is this frame (mod_manager_page)
    bool visited{};                     // shown since the pause menu last closed (hub_page_pending asks about its changes then)
    // Leaving the pause menu from here with changes not applied: Back is held and the player asked
    // first (offer_leave). `leaving` while that is asked; its answer comes from whichever thread
    // answered (1 apply, 2 later); `declined` once it was later, for the rest of this visit; and when
    // to close the menu for the player after that (GetTickCount64; 0: not to).
    bool leaving{}, declined{};
    std::atomic<int> leave_answer{};
    ULONGLONG close_at{};
    std::string notice;
    bool notice_bad{};
    ULONGLONG notice_until{};
    // The README laid out for the width it was laid out at.
    std::shared_ptr<const ms::Readme> readme;
    float readme_width{};
    struct Line { std::string text; ts::ReadmeLine::Kind kind; bool first{}; };
    std::vector<Line> readme_lines;
    // PLUGINS: the plugin the panel beside the list shows, and the one whose page is open in place of
    // the list (empty: the list). `plugin_focus` gives that page the keyboard focus as it next draws;
    // `plugin_busy`: one of its widgets had B and Escape to itself last frame (held, typed in, or with
    // a popup open). `keys_fed`: the arrow keys went to ImGui last frame (feed_page_keys).
    std::string plugin_selected, plugin_open;
    bool plugin_focus{}, plugin_busy{}, keys_fed{};
};
ModPage &mod_page() {
    static ModPage value;
    return value;
}
const std::uint32_t categories_button = hash("mods-categories-open"), plugin_typing = hash("mods-plugin-page-typing");
// The rail's first rows; every other row is a category's name, which none can be (none starts with
// \x01). Most popular (the page opens on it: a Featured row of the front page's picks went, as the
// most popular mods are much the same list), Recently updated and New every mod in that order; All
// mods every mod in SORT's.
constexpr std::string_view rail_popular = "", rail_updated = "\x01" "updated", rail_new = "\x01" "new",
                           rail_all = "\x01" "all";
struct OrderedView {
    std::string_view key;
    const char *name;
    ms::Sort order;
};
constexpr std::array<OrderedView, 3> ordered_views{{{rail_popular, "Most popular", ms::Sort::popular},
                                                     {rail_updated, "Recently updated", ms::Sort::updated},
                                                     {rail_new, "New", ms::Sort::newest}}};
const OrderedView *ordered_view(std::string_view rail) {
    for (const auto &view : ordered_views)
        if (view.key == rail) return &view;
    return nullptr;
}
// SORT orders All mods and the categories. The three ordered views keep their own order, which is
// what they are; SORT is greyed out on them.
bool sort_applies(const ModPage &m) { return !ordered_view(m.rail); }
ms::Sort grid_sort(const ModPage &m) {
    const auto *view = ordered_view(m.rail);
    return view ? view->order : m.sort;
}
// What the grid's head line calls the rail row shown.
std::string rail_name(const ModPage &m) {
    if (const auto *view = ordered_view(m.rail)) return view->name;
    return m.rail.empty() || m.rail == rail_all ? "All mods" : m.rail;
}

// The categories chosen, kept in the profile as the launcher keeps its own: a name a line.
void load_categories(ModPage &m) {
    if (std::exchange(m.categories_loaded, true)) return;
    const auto read = [](const char *key, std::vector<std::string> &into) {
        const auto saved = dingosdk::profile_runtime::local_value(key);
        if (!saved || !saved->is_string()) return;
        std::string line;
        for (const char c : saved->string() + "\n") {
            if (c != '\n') line += c;
            else if (!line.empty() && into.size() < 64) into.push_back(std::exchange(line, {}));
        }
    };
    read("ModCategoriesOnly", m.only);
    read("ModCategoriesHidden", m.hidden);
}
void save_categories(const ModPage &m) {
    const auto joined = [](const std::vector<std::string> &names) {
        std::string text;
        for (const auto &name : names) text += (text.empty() ? "" : "\n") + name;
        return text;
    };
    dingosdk::profile_runtime::set_local_values({{"ModCategoriesOnly", dingosdk::Json(joined(m.only))}, {"ModCategoriesHidden", dingosdk::Json(joined(m.hidden))}});
}
// What the categories chosen are, in words: "Maps, Cosmetics, not Audio", or ALL.
std::string category_summary(const ModPage &m) {
    std::string text;
    for (const auto &name : m.only) text += (text.empty() ? "" : ", ") + name;
    for (const auto &name : m.hidden) text += (text.empty() ? "not " : ", not ") + name;
    return text.empty() ? std::string("ALL") : text;
}
void mod_say(ModPage &m, std::string text, bool bad = false) {
    m.notice = std::move(text);
    m.notice_bad = bad;
    m.notice_until = GetTickCount64() + 7000;
}
// The page's tabs, for hub_frame's strip: UPDATES says how many there are.
std::vector<std::pair<int, const char *>> mod_tabs() {
    const auto &m = mod_page();
    return {{browse_tab, "BROWSE"}, {installed_tab, "MY MODS"}, {updates_tab, m.updates_label.empty() ? "UPDATES" : m.updates_label.c_str()},
            {plugins_tab, "PLUGINS"}};
}

// ---------------------------------------------------------------- drawing

// `text` broken into lines no wider than `width`; at most `most` (0: any), the last ending in "..."
// when there was more.
std::vector<std::string> wrap_text(ImFont *font, float size, std::string_view text, float width, std::size_t most = 0) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    bool more{};
    while (at < text.size()) {
        if (most && lines.size() >= most) { more = true; break; }
        if (text[at] == '\n' || text[at] == '\r') {
            if (text[at] == '\n') lines.push_back(std::exchange(line, {}));
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\r') ++end;
        const std::string word(text.substr(at, end - at));
        const auto candidate = line.empty() ? word : line + ' ' + word;
        if (!line.empty() && width_of(font, size, candidate) > width) lines.push_back(std::exchange(line, word));
        else line = candidate;
        while (end < text.size() && text[end] == ' ') ++end;
        at = end;
    }
    if (!line.empty()) {
        if (most && lines.size() >= most) more = true;
        else lines.push_back(std::move(line));
    }
    if (more && !lines.empty()) {
        auto &last = lines.back();
        while (!last.empty() && width_of(font, size, last + "...") > width) {
            last.pop_back();
            while (!last.empty() && (static_cast<unsigned char>(last.back()) & 0xc0) == 0x80) last.pop_back();
            if (!last.empty() && static_cast<unsigned char>(last.back()) >= 0xc0) last.pop_back();
        }
        last += "...";
    }
    return lines;
}
// Wrapped text; returns how tall it was.
float paragraph(ImDrawList *draw, ImFont *font, float size, ImVec2 at, float width, ImU32 colour, std::string_view text, std::size_t most = 0) {
    const auto lines = wrap_text(font, size, text, width, most);
    for (std::size_t i = 0; i < lines.size(); ++i)
        draw->AddText(font, size, ImVec2(at.x, at.y + static_cast<float>(i) * size * 1.3f), colour, lines[i].c_str());
    return static_cast<float>(lines.size()) * size * 1.3f;
}
// A small label on a plate; returns its width.
float plate(Ui &ui, ImVec2 at, float height, const std::string &text, ImU32 fill, ImU32 ink) {
    const float u = ui.u, size = height * .62f, w = width_of(ui.bold, size, text) + 24 * u;
    ui.draw->AddRectFilled(at, ImVec2(at.x + w, at.y + height), fill, 4 * u);
    ui.draw->AddText(ui.bold, size, ImVec2(at.x + 12 * u, at.y + (height - size) * .5f), ink, text.c_str());
    return w;
}
std::string upper_case(std::string text) {
    for (auto &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
// A package's icon, or its name's first letter on a tile while the icon is on its way.
void package_icon(Ui &ui, ImVec2 a, ImVec2 b, const ts::Package *package, std::string_view title, bool dim = false) {
    if (package)
        if (const auto id = store_icon(package->latest().icon)) {
            ui.draw->AddImage(id, a, b, ImVec2(0, 0), ImVec2(1, 1), dim ? IM_COL32(150, 150, 150, 255) : IM_COL32_WHITE);
            return;
        }
    theme::tile_rect(ui.draw, a, b, track, static_cast<unsigned>(hash(title)) & 0xfff, ui.u * 2);
    const float size = (b.y - a.y) * .5f;
    const std::string letter = title.empty() ? std::string("?") : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(title.front()))));
    ui.draw->AddText(ui.heading, size, ImVec2(a.x + (b.x - a.x - width_of(ui.heading, size, letter)) * .5f, a.y + (b.y - a.y - size) * .5f),
                     theme::muted, letter.c_str());
}
// A package's picture in a box wider than tall: Thunderstore's icons are square, so the icon whole in
// the middle, on a band of itself darkened to the box's width; the letter tile while it is on its way.
void package_picture(Ui &ui, ImVec2 a, ImVec2 b, const ts::Package *package, std::string_view title) {
    const float w = b.x - a.x, h = b.y - a.y;
    bool loading{};
    const auto texture = package && w > h ? store_icon(package->latest().icon, loading) : ImTextureID{};
    if (!texture && loading) {
        // Still on its way: a spinning circle on the picture's panel. The letter is for a mod whose
        // picture failed or that has none.
        ui.draw->AddRectFilled(a, b, IM_COL32(40, 40, 43, 255));
        const ImVec2 centre((a.x + b.x) * .5f, (a.y + b.y) * .5f);
        const float radius = std::min(w, h) * .14f, start = static_cast<float>(ImGui::GetTime()) * 6.0f;
        ui.draw->PathArcTo(centre, radius, start, start + 4.2f, 32);
        ui.draw->PathStroke(IM_COL32(200, 202, 207, 230), 0, std::max(2.0f, radius * .18f));
        return;
    }
    if (!texture) return package_icon(ui, a, b, package, title);
    const float band = h / w, from = .5f - band * .5f;
    ui.draw->AddImage(texture, a, b, ImVec2(0, from), ImVec2(1, from + band), IM_COL32(95, 95, 95, 255));
    const float x = a.x + (w - h) * .5f;
    ui.draw->AddImage(texture, ImVec2(x, a.y), ImVec2(x + h, b.y));
}
// A mod's state as the Insert menu's MODS library shows it (modding_menu.cpp): a tag in the kit's
// colours (ON blue, OFF grey, an update or a set-aside amber, a conflict or an error red), and the
// line under its name: what is wrong or waiting when something is, else what it is and its version.
struct StateTag {
    const char *text;
    ImU32 fill, ink;
    std::string detail;
    ImU32 detail_colour;
    bool problem; // red or amber, with an edge in that colour on its row
};
StateTag state_tag(const ms::Mod &mod) {
    constexpr ImU32 problem_ink = IM_COL32(255, 138, 138, 255);
    std::string about;
    for (std::size_t i = 0; i < mod.contents.size() && i < 3; ++i) about += (i ? ", " : "") + mod.contents[i];
    if (!mod.version.empty()) about += (about.empty() ? "v" : "  \xc2\xb7  v") + mod.version;
    about += (about.empty() ? "" : "  \xc2\xb7  ") + (mod.author.empty() ? std::string("Unknown author") : "by " + mod.author);
    const auto why = [&](const char *otherwise) { return mod.why.empty() ? std::string(otherwise) : mod.why; };
    switch (mod.state) {
    case ms::State::loaded: return {"ON", accent, theme::paper, about, sub_ink, false};
    case ms::State::apply_to_load: return {"ON", accent, theme::paper, why("Apply your changes to load it."), skate_theme::warning, false};
    case ms::State::disabled: return {"OFF", plate_grey, chip_ink, about, sub_ink, false};
    case ms::State::apply_to_unload: return {"OFF", plate_grey, chip_ink, why("Apply your changes to unload it."), skate_theme::warning, false};
    case ms::State::left_out: return {"CONFLICT", skate_theme::danger, skate_theme::black, why("It changes the same parts of the game as another mod."), problem_ink, true};
    case ms::State::not_loaded: return {"ERROR", skate_theme::danger, skate_theme::black, why("It could not be loaded."), problem_ink, true};
    case ms::State::set_aside: return {"SET ASIDE", skate_theme::warning, skate_theme::black, why("Set aside for another variant."), skate_theme::warning, true};
    case ms::State::update_waiting: return {"UPDATE", skate_theme::warning, skate_theme::black, why("Restart the game to update it."), skate_theme::warning, true};
    case ms::State::removal_waiting: return {"REMOVED", plate_grey, chip_ink, why("Restart the game to remove it."), skate_theme::warning, false};
    }
    return {ms::state_label(mod.state), plate_grey, chip_ink, about, sub_ink, false};
}
ms::Action action_of(const ModPage &m, const ts::Package &package) {
    return m.mods ? ms::action_for(package, m.mods->versions) : ms::Action::install;
}
// The package's mod in the Mods folder, if it is installed.
const ms::Mod *installed_mod(const ModPage &m, const ts::Package &package) {
    if (!m.mods) return nullptr;
    const auto folder = ts::folder_for(package.full_name);
    for (const auto &mod : m.mods->mods)
        if (_stricmp(mod.folder.c_str(), folder.c_str()) == 0) return &mod;
    return nullptr;
}
// Installs (or queues, while another job runs) the packages, saying which.
void start_install(ModPage &m, const std::vector<std::string> &packages) {
    const bool behind = m.progress.busy;
    if (const auto refused = ms::install(packages); !refused.empty()) mod_say(m, refused, true);
    else if (behind) {
        const auto *first = m.listing ? ts::find_package(m.listing->packages, packages.front()) : nullptr;
        mod_say(m, packages.size() == 1 && first ? first->title() + " is in the download queue." : "Added to the download queue.");
    }
}
// What a package's button says and whether it can be pressed, the queue considered: QUEUED while it
// is in it, ADD TO QUEUE while another job runs.
struct QueueButton {
    std::string text;
    bool enabled{};
};
QueueButton queue_button(const ModPage &m, const ts::Package &package, ms::Action action, std::string_view idle) {
    const auto &progress = m.progress;
    if (progress.busy && progress.package == package.full_name)
        return {progress.step == ms::Progress::Step::downloading ? "DOWNLOADING..." : "INSTALLING...", false};
    if (progress.busy && std::find(progress.queued.begin(), progress.queued.end(), package.full_name) != progress.queued.end())
        return {"QUEUED", false};
    if (action == ms::Action::installed && idle != "REINSTALL") return {"INSTALLED", false};
    if (progress.busy) return {"ADD TO QUEUE", true};
    return {std::string(idle), true};
}
// A package's main action, as A on its card and the first button beside the grid do it: INSTALL or
// UPDATE (or what the queue is doing with it), or SWITCH ON / OFF once it is installed and up to date.
struct MainAction {
    std::string text;
    const char *prompt{}; // what the footer's A says for it
    bool enabled{}, primary{};
};
MainAction main_action(const ModPage &m, const ts::Package &package) {
    const auto action = action_of(m, package);
    if (action == ms::Action::installed)
        if (const auto *mine = installed_mod(m, package); mine && !mine->gone)
            return {mine->enabled ? "SWITCH OFF" : "SWITCH ON", mine->enabled ? "Switch off" : "Switch on", true, !mine->enabled};
    const auto does = queue_button(m, package, action, action == ms::Action::update ? "UPDATE" : "INSTALL");
    return {does.text, action == ms::Action::update ? "Update" : "Install", does.enabled, does.enabled};
}
void run_main_action(ModPage &m, const ts::Package &package) {
    if (!main_action(m, package).enabled) return;
    if (action_of(m, package) != ms::Action::installed) start_install(m, {package.full_name});
    else if (const auto *mine = installed_mod(m, package)) ms::set_enabled(mine->folder, !mine->enabled);
}
void open_details(Ui &ui, ModPage &m, const ts::Package &package) {
    m.return_tab = m.tab;
    m.return_focus = ui.h.focus;
    m.tab = browse_tab;
    m.details = package.full_name;
    m.picked = package.full_name;
    m.readme.reset();
    m.readme_lines.clear();
    ui.h.focus_next = hash("mods-detail-action");
}
// Back from a mod's page to where it was opened from, the focus on what opened it.
void close_details(Hub &h, ModPage &m) {
    m.details.clear();
    m.tab = m.return_tab;
    h.focus_next = m.return_focus ? m.return_focus : hash("mods-search");
}
void close_categories(Hub &h, ModPage &m) {
    m.categories_open = false;
    h.focus_next = categories_button;
    h.scrolls[hash("mods-browse")] = 0;
}
// A plugin's page opened in place of PLUGINS' list: its widgets have ImGui's own focus, and none of the
// page's controls is lit behind it.
void open_plugin_page(Hub &h, ModPage &m, const std::string &id) {
    m.plugin_open = m.plugin_selected = id;
    m.plugin_focus = true;
    m.plugin_busy = false;
    h.focus = 0;
    h.ring = false;
}
// Back to the list, the focus on the plugin's row.
void close_plugin_page(Hub &h, ModPage &m) {
    if (h.typing == plugin_typing) set_typing(h, 0);
    h.focus_next = hash(m.plugin_open, 91);
    m.plugin_open.clear();
    m.plugin_busy = false;
}
// Whether B and Escape are the page's (a mod's page, the categories or a plugin's page is open:
// hub_frame), and what they do then. Over a plugin's widget that has them (a popup, a text field) they
// are that widget's.
bool mods_hold_back(const ModPage &m) {
    return (m.tab == browse_tab && (!m.details.empty() || m.categories_open)) || (m.tab == plugins_tab && !m.plugin_open.empty());
}
void mods_back(Hub &h, ModPage &m) {
    if (m.tab == plugins_tab && !m.plugin_open.empty()) {
        if (!m.plugin_busy) close_plugin_page(h, m);
    } else if (m.categories_open) close_categories(h, m);
    else close_details(h, m);
}
// Whether the D-pad, the arrow keys and A are ImGui's navigation's (a plugin's page shows: hub_frame).
bool mods_imgui_nav(const ModPage &m) { return m.tab == plugins_tab && !m.plugin_open.empty(); }

// What was done to a card of BROWSE's grid.
enum class CardPress { none, main, pick, page };
// A package as a card of BROWSE's grid: its picture, what it is to this PC as a tag (INSTALLED, UPDATE,
// NEW), its name, its maker, its downloads and version. A or Enter is its main action; a click picks it
// for the details beside the grid, a double click opens its page; the D-pad picks the card it moves
// onto (browse_grid). A card out of view still takes its place in the focus order, so the D-pad can
// move onto it, but is not drawn: drawing it would ask for its icon, and icons have only so many
// places on the GPU. `at` is its place in the grid, which is what picking it keeps.
CardPress package_card(Ui &ui, ModPage &m, std::uint32_t id, std::size_t at, ImVec2 p, ImVec2 q, float picture_h, const ts::Package &package,
                       const Item *clip) {
    auto &h = ui.h;
    const bool fired = h.fire == id;
    const Hit hit = item(ui, id, p, q, clip);
    if (h.focus == id && h.ring) m.card_focused = true;
    const CardPress press = !hit.press ? CardPress::none : fired ? CardPress::main : ui.doubled ? CardPress::page : CardPress::pick;
    if (press == CardPress::pick || press == CardPress::page) m.picked = package.full_name, m.picked_at = at;
    if (clip && (q.x < clip->a.x || p.x > clip->b.x || q.y < clip->a.y || p.y > clip->b.y)) return press;
    auto *draw = ui.draw;
    const float u = ui.u, inset = 8 * u;
    // White under the D-pad or the keys, a lighter grey under the pointer, as the server browser's rows;
    // the one the details show, while the focus is elsewhere (on them, or on the rail), a blue edge.
    // That one is a place in the grid, not a mod: the same mod twice is not lit twice.
    const auto [focused, pointed] = lit_of(hit);
    theme::tile_rect(draw, p, q, focused ? theme::focus_fill : pointed ? row_hover : tile_dark, id & 0xfff, u * 2);
    if (!focused && m.picked_at == at && m.picked == package.full_name) draw->AddRect(p, q, accent, 0, 0, 5 * u);
    const ImVec2 pa(p.x + inset, p.y + inset), pz(q.x - inset, p.y + picture_h);
    package_picture(ui, pa, pz, &package, package.title());
    // Being installed, or waiting its turn: a strip over the foot of its picture says so.
    const auto &progress = m.progress;
    const bool queued = progress.busy && std::find(progress.queued.begin(), progress.queued.end(), package.full_name) != progress.queued.end();
    if (progress.busy && (progress.package == package.full_name || queued)) {
        const ImVec2 s0(pa.x, pz.y - 44 * u), s1(pz.x, pz.y);
        draw->AddRectFilled(s0, s1, IM_COL32(0, 0, 0, 200));
        if (!queued && progress.fraction >= 0) draw->AddRectFilled(s0, ImVec2(s0.x + (s1.x - s0.x) * std::clamp(progress.fraction, 0.f, 1.f), s1.y), accent);
        const char *text = queued ? "WAITING" : progress.step == ms::Progress::Step::downloading ? "DOWNLOADING" : "INSTALLING";
        draw->AddText(ui.bold, 24 * u, ImVec2(s0.x + 12 * u, s0.y + 10 * u), theme::paper, text);
    }
    // What it is to this PC, in the kit's tag colours: installed blue (as the Insert menu's ON), an
    // update amber (as its UPDATE), and one made lately green.
    const auto action = action_of(m, package);
    const ImVec2 tag_at(pa.x + 8 * u, pa.y + 8 * u);
    if (action == ms::Action::update) plate(ui, tag_at, 36 * u, "UPDATE", skate_theme::warning, skate_theme::black);
    else if (action == ms::Action::installed) plate(ui, tag_at, 36 * u, "INSTALLED", accent, theme::paper);
    else if (!m.new_since.empty() && package.date_created >= m.new_since) plate(ui, tag_at, 36 * u, "NEW", skate_theme::good, skate_theme::black);
    const float tx = p.x + 18 * u, right = q.x - 14 * u;
    const ImU32 ink = focused ? focus_ink : theme::paper, sub = focused ? sub_ink_lit : sub_ink;
    label(draw, ui.bold, 32 * u, ImVec2(tx, pz.y + 12 * u), right, ink, package.title());
    label(draw, ui.body, 26 * u, ImVec2(tx, pz.y + 56 * u), right, sub, "by " + package.owner);
    label(draw, ui.body, 26 * u, ImVec2(tx, pz.y + 92 * u), right, sub, ms::count_text(package.downloads) + " downloads  \xc2\xb7  v" + package.latest().number);
    return press;
}

// Pointed at and clicked with the mouse only: not a stop for the D-pad. A plugin's page's way back
// is one (its D-pad is ImGui's while it shows, and B is the way back there).
struct Pointed {
    bool hover{}, press{};
};
Pointed pointed(Ui &ui, ImVec2 a, ImVec2 b, const Item *clip) {
    const bool over = ui.input && inside(ui.mouse, a, b) && (!clip || inside(ui.mouse, clip->a, clip->b));
    Pointed result{over && !ui.h.ring, over && ui.clicked};
    if (result.press) ui.h.sound = sound_select;
    return result;
}
// A button drawn as button() draws one, for the mouse only (Pointed).
bool pointer_button(Ui &ui, ImVec2 a, ImVec2 b, const std::string &text, Look look, bool enabled, const Item *clip) {
    const float u = ui.u, size = std::min(40 * u, (b.y - a.y) * .42f);
    const auto hit = enabled ? pointed(ui, a, b, clip) : Pointed{};
    ImU32 fill = hit.hover ? raised_hover : plate_grey, ink = enabled ? theme::paper : theme::muted;
    if (look == Look::primary) fill = !enabled ? primary_off : hit.hover ? primary_hover : primary_rest, ink = enabled ? focus_ink : IM_COL32(245, 245, 245, 140);
    theme::tile_rect(ui.draw, a, b, fill, hash(text) & 0xfff, u * 2);
    ui.draw->AddText(ui.bold, size, ImVec2(a.x + (b.x - a.x - width_of(ui.bold, size, text)) * .5f, a.y + (b.y - a.y - size) * .5f), ink, text.c_str());
    return hit.press;
}

// ---------------------------------------------------------------- what the page shows

void refresh_view(ModPage &m) {
    m.listing = ms::listing();
    m.mods = ms::installed();
    const auto mods_serial = m.mods ? m.mods->serial : 0U;
    load_categories(m);
    std::string key = m.search + '\n' + std::to_string(static_cast<int>(grid_sort(m))) + '\n' + m.rail + (m.hide_installed ? "\n-installed" : "");
    for (const auto &name : m.only) key += "\n+" + name;
    for (const auto &name : m.hidden) key += "\n-" + name;
    if (m.listing->serial == m.listing_serial && mods_serial == m.mods_serial && key == m.query_key) return;
    const bool listing_changed = m.listing->serial != m.listing_serial, data_changed = listing_changed || mods_serial != m.mods_serial;
    m.listing_serial = m.listing->serial;
    m.mods_serial = mods_serial;
    m.query_key = key;
    const auto &packages = m.listing->packages;
    const ts::Installed none;
    const auto &have = m.mods ? m.mods->versions : none;
    if (listing_changed || m.front.shelves.empty()) {
        m.front = ms::front(packages, have, ms::today());
        m.categories = ms::categories(packages);
        m.by_folder.clear();
        for (const auto &package : packages) m.by_folder[ts::folder_for(package.full_name)] = &package;
        m.new_since = ms::days_before(ms::today(), 14);
    }
    ms::Query query{m.search, m.only, m.hidden, grid_sort(m)};
    query.pinned_first = !ordered_view(m.rail);
    m.results = ms::search(packages, query, have);
    // The rail's category (any, on the views before the categories) and HIDE INSTALLED narrow it.
    const bool any_category = m.rail.empty() || m.rail.front() == '\x01';
    std::erase_if(m.results, [&](const ts::Package *package) {
        return (!any_category && !package->in_category(m.rail)) || (m.hide_installed && ms::action_for(*package, have) != ms::Action::install);
    });
    // Each card's id: its mod's name, and if that mod is already in the grid (nothing lists one twice
    // today, but a view that did must not light two cards), the name again with another salt.
    m.card_ids.clear();
    std::set<std::uint32_t> taken;
    for (const auto *package : m.results) {
        auto id = hash(package->full_name, 41);
        for (std::uint64_t again = 1; !taken.insert(id).second; ++again) id = hash(package->full_name, 41 + again * 7919);
        m.card_ids.push_back(id);
    }
    if (data_changed) {
        m.listed = ms::search(packages, {}, have).size();
        m.search_hint = "Search " + std::to_string(m.listed) + (m.listed == 1 ? " mod" : " mods");
        m.updates.clear();
        for (const auto &package : packages)
            if (ts::update_available(package, have)) m.updates.push_back(&package);
        m.updates_label = m.updates.empty() ? std::string("UPDATES") : "UPDATES  \xc2\xb7  " + std::to_string(m.updates.size());
    }
}
const ts::Package *package_named(const ModPage &m, std::string_view full_name) {
    return m.listing ? ts::find_package(m.listing->packages, full_name) : nullptr;
}

// ---------------------------------------------------------------- BROWSE

// The views down the left, as rows: Most popular, Recently updated
// and New, then All mods and the listing's own categories with how many mods each has, the biggest
// first. Pressing one shows it; the
// one shown is a light row with the kit's blue edge, the focus white as everywhere.
void category_rail(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, row_h = 84 * u;
    Scroller list(ui, hash("mods-rail"), a, z);
    const auto row = [&](std::string_view key, const std::string &name, std::size_t count) {
        const auto id = hash(key, 81);
        bool drawn{};
        const float top = list.next(row_h, 8 * u, drawn);
        const ImVec2 p(a.x, top), q(z.x - 14 * u, top + row_h);
        const Hit hit = item(ui, id, p, q, &list.box);
        list.reveal(id, top, row_h);
        const bool chosen = m.rail == key;
        if (hit.press && !chosen) m.rail = std::string(key), ui.h.scrolls[hash("mods-browse")] = 0;
        if (!drawn) return;
        const auto [focused, pointed] = lit_of(hit);
        theme::tile_rect(draw, p, q, focused ? theme::focus_fill : chosen ? card_light : pointed ? row_hover : tile_dark, id & 0xfff, u * 2);
        if (chosen) draw->AddRectFilled(p, ImVec2(p.x + 8 * u, q.y), accent);
        const bool dark = focused || chosen;
        float right = q.x - 22 * u;
        if (count) {
            const auto text = std::to_string(count);
            right -= width_of(ui.body, 28 * u, text);
            draw->AddText(ui.body, 28 * u, ImVec2(right, p.y + (row_h - 28 * u) * .5f), dark ? sub_ink_lit : sub_ink, text.c_str());
            right -= 16 * u;
        }
        label(draw, ui.bold, 32 * u, ImVec2(p.x + 28 * u, p.y + (row_h - 32 * u) * .5f), right, dark ? focus_ink : theme::paper, name);
    };
    for (const auto &view : ordered_views) row(view.key, view.name, 0);
    row(rail_all, "All mods", m.listed);
    for (const auto &[name, count] : m.categories) row(name, name, static_cast<std::size_t>(count));
    list.end();
}

// The bar over the grid: the search box, SORT, HIDE INSTALLED, and CATEGORIES (a panel of its own:
// categories shown only or hidden, kept in the profile).
void browse_bar(Ui &ui, ModPage &m, ImVec2 a, float right, float bar) {
    const float u = ui.u, gap = 16 * u, sort_w = 560 * u, filter_w = 400 * u;
    const float hide_w = width_of(ui.bold, 32 * u, "HIDE INSTALLED") + 40 * u;
    const float search_w = std::max(360 * u, right - a.x - sort_w - hide_w - filter_w - gap * 3);
    field(ui, hash("mods-search"), a, ImVec2(a.x + search_w, a.y + bar), m.search, m.search_hint.empty() ? "Search mods" : m.search_hint.c_str(), false, 64);
    float x = a.x + search_w + gap;
    // On the views with an order of their own it says that order, greyed out (sort_applies).
    const bool sorts = sort_applies(m);
    const std::string order = sorts ? ms::sort_label(m.sort) : ms::sort_label(grid_sort(m));
    if (button(ui, hash("mods-sort"), ImVec2(x, a.y), ImVec2(x + sort_w, a.y + bar), "SORT", Look::tile, sorts, order))
        m.sort = static_cast<ms::Sort>((static_cast<std::size_t>(m.sort) + 1) % ms::sort_count);
    x += sort_w + gap;
    if (chip(ui, hash("mods-hide-installed"), x, a.y, a.y + bar, "HIDE INSTALLED", m.hide_installed))
        m.hide_installed = !m.hide_installed, ui.h.scrolls[hash("mods-browse")] = 0;
    x += gap - 10 * u; // (chip() moved on 10 units past itself)
    const auto chosen = m.only.size() + m.hidden.size();
    if (button(ui, categories_button, ImVec2(x, a.y), ImVec2(std::max(x + 200 * u, right), a.y + bar), "CATEGORIES",
               m.categories_open ? Look::chosen : Look::tile, true, chosen ? std::to_string(chosen) : std::string("ALL"))) {
        if (m.categories_open) close_categories(ui.h, m);
        else {
            m.categories_open = true;
            ui.h.focus_next = m.categories.empty() ? hash("mods-categories-done") : hash(m.categories.front().first, 71);
        }
    }
}

// The categories, all of them, as tiles: pressing one shows only it (with any others shown only),
// again hides it, again leaves it either way, as the launcher's category menu does. Made for the
// D-pad and A as much as the mouse.
void categories_panel(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, inset = 40 * u;
    theme::tile_rect(draw, a, z, tile_dark, 0x3c1, u * 2);
    draw->AddText(ui.bold, 44 * u, ImVec2(a.x + inset, a.y + inset + 6 * u), theme::paper, "CATEGORIES");
    const float bh = 92 * u, bw = 400 * u;
    const ImVec2 done(z.x - inset - bw, a.y + inset), clear(done.x - 20 * u - bw, a.y + inset);
    label(draw, ui.body, 30 * u, ImVec2(a.x + inset, a.y + inset + 78 * u), clear.x - 30 * u, sub_ink,
          "Press a category to show only it, again to hide it, and again for either. Mods in any category shown are listed.");
    if (button(ui, hash("mods-categories-clear"), clear, ImVec2(clear.x + bw, clear.y + bh), "CLEAR", Look::plain, !m.only.empty() || !m.hidden.empty())) {
        m.only.clear();
        m.hidden.clear();
        save_categories(m);
    }
    if (button(ui, hash("mods-categories-done"), done, ImVec2(done.x + bw, done.y + bh), "DONE", Look::primary)) close_categories(ui.h, m);
    const ImVec2 la(a.x + inset, a.y + inset + 150 * u), lz(z.x - inset, z.y - inset);
    const float spacing = 16 * u, tile_h = 104 * u;
    const auto columns = std::max<std::size_t>(1, static_cast<std::size_t>((lz.x - la.x + spacing) / (600 * u + spacing)));
    const float tile_w = (lz.x - la.x - 20 * u - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    Scroller list(ui, hash("mods-categories"), la, lz);
    const auto has = [](const std::vector<std::string> &names, const std::string &name) { return std::find(names.begin(), names.end(), name) != names.end(); };
    for (std::size_t i = 0; i < m.categories.size(); i += columns) {
        bool drawn{};
        const float top = list.next(tile_h, spacing, drawn);
        for (std::size_t j = i; j < std::min(m.categories.size(), i + columns); ++j) {
            const auto &[name, count] = m.categories[j];
            const auto id = hash(name, 71);
            const ImVec2 p(la.x + static_cast<float>(j - i) * (tile_w + spacing), top), q(p.x + tile_w, top + tile_h);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, top, tile_h);
            const int state = has(m.only, name) ? 1 : has(m.hidden, name) ? 2 : 0;
            if (hit.press) {
                std::erase(m.only, name);
                std::erase(m.hidden, name);
                if (state == 0) m.only.push_back(name);
                else if (state == 1) m.hidden.push_back(name);
                save_categories(m);
            }
            if (!drawn) continue;
            // As the chips: shown only on the blue, hidden on a dark red, white under the D-pad or the
            // keys and lighter under the pointer.
            const auto [focused, pointed] = lit_of(hit);
            theme::tile_rect(draw, p, q, focused ? theme::focus_fill : state == 1 ? (pointed ? primary_hover : primary_rest) : state == 2 ? IM_COL32(86, 30, 30, 245)
                                    : pointed ? row_hover : raised, id & 0xfff, u * 2);
            // Its state on the left, as a mark: a tick shown only, a cross hidden, nothing either way.
            const ImVec2 mark(p.x + 30 * u, p.y + tile_h * .5f);
            const ImU32 tick = focused || state == 1 ? skate_theme::black : theme::paper, cross = focused ? skate_theme::danger : IM_COL32(255, 138, 138, 255);
            if (state == 1) {
                draw->AddLine(ImVec2(mark.x - 14 * u, mark.y), ImVec2(mark.x - 3 * u, mark.y + 12 * u), tick, 7 * u);
                draw->AddLine(ImVec2(mark.x - 3 * u, mark.y + 12 * u), ImVec2(mark.x + 18 * u, mark.y - 14 * u), tick, 7 * u);
            } else if (state == 2) {
                draw->AddLine(ImVec2(mark.x - 14 * u, mark.y - 14 * u), ImVec2(mark.x + 14 * u, mark.y + 14 * u), cross, 7 * u);
                draw->AddLine(ImVec2(mark.x - 14 * u, mark.y + 14 * u), ImVec2(mark.x + 14 * u, mark.y - 14 * u), cross, 7 * u);
            } else draw->AddCircle(mark, 14 * u, focused ? sub_ink_lit : sub_ink, 20, 3 * u);
            std::string upper = name;
            for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            const auto counted = std::to_string(count);
            const float count_w = width_of(ui.body, 30 * u, counted);
            label(draw, ui.bold, 36 * u, ImVec2(p.x + 64 * u, p.y + (tile_h - 36 * u) * .5f), q.x - count_w - 40 * u,
                  focused || state == 1 ? focus_ink : state == 2 && !pointed ? theme::muted : theme::paper, upper);
            draw->AddText(ui.body, 30 * u, ImVec2(q.x - count_w - 24 * u, p.y + (tile_h - 30 * u) * .5f),
                          focused ? sub_ink_lit : state == 1 ? IM_COL32(70, 70, 74, 255) : chip_ink, counted.c_str());
        }
    }
    list.end();
}

void package_details(Ui &ui, ModPage &m, const ts::Package &package, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, inset = 40 * u;
    const auto &version = package.latest();
    const auto action = action_of(m, package);
    const ms::Mod *mine = installed_mod(m, package);
    // ---- the left column: the picture and what to do, fitted to the window. The picture is as big
    // as the buttons under it leave room for; in a window too short for that, the buttons go two to a row.
    const float left_w = std::min(760 * u, (z.x - a.x) * .3f);
    const ImVec2 la(a.x, a.y), lz(a.x + left_w, z.y);
    theme::tile_rect(draw, la, lz, tile_dark, 0x3a1, u * 2);
    const bool changes = m.mods && m.mods->changes && !live_mods::busy();
    const bool mine_here = mine && !mine->gone;
    const int count = 2 + (changes ? 1 : 0) + (mine_here ? 2 : 0); // the action, BACK, and the rest
    const float bh = 88 * u, spacing = 18 * u, inner = left_w - inset * 2, room = lz.y - la.y - inset * 2;
    int columns = 1;
    const auto rows = [&] { return (count + columns - 1) / columns; };
    float icon = std::min(inner, room - static_cast<float>(rows()) * (bh + spacing));
    if (icon < 240 * u) {
        columns = 2;
        icon = std::min(inner, room - static_cast<float>(rows()) * (bh + spacing));
    }
    float y = la.y + inset;
    if (icon >= 120 * u) {
        const float ix = la.x + (left_w - icon) * .5f;
        package_icon(ui, ImVec2(ix, y), ImVec2(ix + icon, y + icon), &package, package.title());
        y += icon + spacing;
    }
    const float bw = (inner - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    int placed{};
    const auto place = [&]() {
        const ImVec2 p(la.x + inset + static_cast<float>(placed % columns) * (bw + spacing), y + static_cast<float>(placed / columns) * (bh + spacing));
        ++placed;
        return std::pair{p, ImVec2(p.x + bw, p.y + bh)};
    };
    {
        // INSTALL, UPDATE or REINSTALL; while the queue is busy, what it is doing with this mod (how far
        // is at the foot of the page).
        const auto [p, q] = place();
        const auto does = queue_button(m, package, action, action == ms::Action::install ? "INSTALL" : action == ms::Action::update ? "UPDATE" : "REINSTALL");
        if (button(ui, hash("mods-detail-action"), p, q, does.text, does.enabled && action != ms::Action::installed ? Look::primary : Look::plain, does.enabled))
            start_install(m, {package.full_name});
    }
    if (changes) {
        const auto [p, q] = place();
        if (button(ui, hash("mods-detail-apply"), p, q, "APPLY CHANGES", Look::primary, !m.progress.busy)) mod_say(m, ms::apply());
    }
    if (mine_here) {
        {
            const auto [p, q] = place();
            if (button(ui, hash("mods-detail-toggle"), p, q, mine->enabled ? "SWITCH OFF" : "SWITCH ON", Look::plain)) ms::set_enabled(mine->folder, !mine->enabled);
        }
        const auto [p, q] = place();
        const bool confirming = m.confirm == mine->folder && GetTickCount64() < m.confirm_until;
        if (button(ui, hash("mods-detail-remove"), p, q, confirming ? "PRESS AGAIN" : "UNINSTALL", Look::plain,
                   mine->state != ms::State::removal_waiting)) {
            if (confirming) {
                m.confirm.clear();
                if (const auto refused = ms::uninstall(mine->folder); !refused.empty()) mod_say(m, refused, true);
            } else m.confirm = mine->folder, m.confirm_until = GetTickCount64() + 4000;
        }
    }
    {
        const auto [p, q] = place();
        if (button(ui, hash("mods-detail-back"), p, q, "BACK TO MODS", Look::plain)) close_details(ui.h, m);
    }

    // ---- the right: what it is, and its README
    const ImVec2 ra(lz.x + 28 * u, a.y), rz(z.x, z.y);
    theme::tile_rect(draw, ra, rz, tile_dark, 0x3a2, u * 2);
    const float left = ra.x + inset, edge = rz.x - inset;
    y = ra.y + inset;
    label(draw, ui.heading, 72 * u, ImVec2(left, y), edge, theme::paper, package.title());
    y += 92 * u;
    std::string facts = "by " + package.owner + "   v" + version.number + "   " + ms::count_text(package.downloads) + " downloads";
    if (version.file_size) facts += "   " + ms::size_text(version.file_size);
    facts += "   updated " + ms::date_text(package.date_updated);
    if (package.rating) facts += "   " + std::to_string(package.rating) + " likes";
    label(draw, ui.body, 32 * u, ImVec2(left, y), edge, sub_ink, facts);
    y += 54 * u;
    float x = left;
    if (mine) {
        const auto tag = state_tag(*mine);
        x += plate(ui, ImVec2(x, y), 42 * u, tag.text, tag.fill, tag.ink) + 12 * u;
        if (mine->version != version.number && !mine->version.empty())
            x += plate(ui, ImVec2(x, y), 42 * u, "YOU HAVE v" + mine->version, plate_grey, theme::paper) + 12 * u;
    }
    for (std::size_t i = 0; i < package.categories.size() && i < 5; ++i) {
        std::string upper = package.categories[i];
        for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        x += plate(ui, ImVec2(x, y), 42 * u, upper, plate_grey, chip_ink) + 12 * u;
    }
    y += 66 * u;
    if (package.deprecated) {
        label(draw, ui.bold, 30 * u, ImVec2(left, y), edge, IM_COL32(255, 138, 138, 255), "Its author marked it deprecated: it is no longer looked after.");
        y += 48 * u;
    }
    if (!version.dependencies.empty()) {
        std::string needs;
        std::size_t named{};
        for (const auto &dependency : version.dependencies) {
            const auto name = ts::split_dependency(dependency).first;
            if (name.ends_with("-ReSkate") || name == "ReSkate-ReSkate") continue;
            if (named++ < 6) needs += (needs.empty() ? "" : ", ") + name;
        }
        if (named > 6) needs += " and " + std::to_string(named - 6) + " more";
        if (!needs.empty()) {
            y += paragraph(draw, ui.body, 30 * u, ImVec2(left, y), edge - left, sub_ink, "Brings what it needs with it: " + needs, 2) + 14 * u;
        }
    }
    y += 6 * u;
    draw->AddRectFilled(ImVec2(left, y), ImVec2(edge, y + 3 * u), track);
    y += 24 * u;
    // The README, scrolled with the wheel, or by moving the focus down its paragraphs.
    if (!m.readme) m.readme = ms::readme(package);
    const ImVec2 ba(left, y), bz(edge, rz.y - inset);
    if (!m.readme) {
        draw->AddText(ui.body, 32 * u, ba, theme::muted, "Reading its page on Thunderstore...");
        return;
    }
    if (m.readme->failed || m.readme->text.lines.empty()) {
        paragraph(draw, ui.body, 32 * u, ba, bz.x - ba.x, soft, m.readme->failed ? version.description + "\n\n(Its page on Thunderstore could not be read.)" : version.description);
        return;
    }
    const float text_w = bz.x - ba.x - 30 * u;
    if (m.readme_width != text_w || m.readme_lines.empty()) {
        m.readme_width = text_w;
        m.readme_lines.clear();
        using Kind = ts::ReadmeLine::Kind;
        for (const auto &line : m.readme->text.lines) {
            const float size = line.kind == Kind::heading ? 44 * u : 32 * u;
            ImFont *font = line.kind == Kind::heading ? ui.bold : ui.body;
            const float indent = line.kind == Kind::bullet ? 40 * u : line.kind == Kind::code ? 28 * u : 0;
            if (line.kind == Kind::gap || line.kind == Kind::rule || line.text.empty()) {
                m.readme_lines.push_back({{}, line.kind, true});
                continue;
            }
            bool first = true;
            for (auto &piece : wrap_text(font, size, line.text, text_w - indent)) m.readme_lines.push_back({std::move(piece), line.kind, std::exchange(first, false)});
        }
        if (m.readme->text.cut) m.readme_lines.push_back({"(The rest is on its Thunderstore page.)", ts::ReadmeLine::Kind::text, true});
    }
    Scroller list(ui, hash("mods-readme"), ba, bz);
    // A focus stop every few lines, so a controller can read down it.
    std::size_t since_stop = 99;
    for (std::size_t i = 0; i < m.readme_lines.size(); ++i) {
        const auto &line = m.readme_lines[i];
        using Kind = ts::ReadmeLine::Kind;
        const float size = line.kind == Kind::heading ? 44 * u : 32 * u;
        const float height = line.kind == Kind::gap ? 20 * u : line.kind == Kind::rule ? 30 * u : size * 1.35f + (line.kind == Kind::heading && line.first ? 16 * u : 0);
        bool shown{};
        const float top = list.next(height, 0, shown);
        if (line.first && line.kind != Kind::gap && line.kind != Kind::rule && ++since_stop >= 4) {
            since_stop = 0;
            const auto stop = hash("mods-readme-line", i);
            const Hit hit = item(ui, stop, ImVec2(ba.x, top), ImVec2(bz.x - 20 * u, top + height), &list.box);
            list.reveal(stop, top, std::min(height * 4, bz.y - ba.y));
            if (hit.focus && ui.h.ring) draw->AddRectFilled(ImVec2(ba.x, top), ImVec2(ba.x + 6 * u, top + height), theme::paper);
        }
        if (!shown) continue;
        const float text_top = top + (line.kind == Kind::heading && line.first ? 16 * u : 0);
        switch (line.kind) {
        case Kind::heading: draw->AddText(ui.bold, size, ImVec2(ba.x + 16 * u, text_top), theme::paper, line.text.c_str()); break;
        case Kind::bullet:
            if (line.first) draw->AddCircleFilled(ImVec2(ba.x + 28 * u, text_top + size * .55f), 6 * u, chip_ink, 12);
            draw->AddText(ui.body, size, ImVec2(ba.x + 56 * u, text_top), soft, line.text.c_str());
            break;
        case Kind::code:
            draw->AddRectFilled(ImVec2(ba.x + 16 * u, top), ImVec2(bz.x - 24 * u, top + height), IM_COL32(10, 10, 10, 200));
            draw->AddText(ui.body, size, ImVec2(ba.x + 44 * u, text_top), IM_COL32(190, 220, 255, 255), line.text.c_str());
            break;
        case Kind::rule: draw->AddRectFilled(ImVec2(ba.x + 16 * u, top + 13 * u), ImVec2(bz.x - 24 * u, top + 16 * u), track); break;

        case Kind::gap: break;
        default: draw->AddText(ui.body, size, ImVec2(ba.x + 16 * u, text_top), soft, line.text.c_str()); break;
        }
    }
    list.end();
}

// The details beside the grid: the card picked, big, what it is, its facts on plates, as much of its
// description as fits, and its buttons: its main action, SWITCH ON / OFF beside an update, and FULL
// PAGE, its page with the README (package_details).
void browse_panel(Ui &ui, ModPage &m, const ts::Package *package, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, inset = 32 * u;
    theme::tile_rect(draw, a, z, tile_dark, 0x3a5, u * 2);
    if (!package) {
        draw->AddText(ui.body, 32 * u, ImVec2(a.x + inset, a.y + inset), theme::muted, "Pick a mod to see it here.");
        return;
    }
    const auto &version = package->latest();
    const auto *mine = installed_mod(m, *package);
    const float picture_h = std::min((z.x - a.x) * .56f, (z.y - a.y) * .36f);
    package_picture(ui, ImVec2(a.x + 8 * u, a.y + 8 * u), ImVec2(z.x - 8 * u, a.y + 8 * u + picture_h), package, package->title());
    const float left = a.x + inset, edge = z.x - inset;
    float y = a.y + 8 * u + picture_h + 24 * u;
    label(draw, ui.heading, 56 * u, ImVec2(left, y), edge, theme::paper, package->title());
    y += 72 * u;
    std::string about = "by " + package->owner + "  \xc2\xb7  v" + version.number;
    if (!package->categories.empty()) about += "  \xc2\xb7  " + package->categories.front();
    label(draw, ui.body, 28 * u, ImVec2(left, y), edge, sub_ink, about);
    y += 50 * u;
    // Its facts on plates, onto another line when they do not fit on one.
    const float plate_h = 38 * u;
    float x = left;
    const auto fact = [&](const std::string &text, ImU32 fill, ImU32 ink) {
        if (x > left && x + width_of(ui.bold, plate_h * .62f, text) + 24 * u > edge) x = left, y += plate_h + 10 * u;
        x += plate(ui, ImVec2(x, y), plate_h, text, fill, ink) + 10 * u;
    };
    if (mine) {
        const auto tag = state_tag(*mine);
        fact(tag.text, tag.fill, tag.ink);
    }
    fact(ms::count_text(package->downloads) + " DOWNLOADS", plate_grey, chip_ink);
    if (version.file_size) fact(ms::size_text(version.file_size), plate_grey, chip_ink);
    fact("UPDATED " + upper_case(ms::date_text(package->date_updated)), plate_grey, chip_ink);
    if (mine && !mine->version.empty() && mine->version != version.number) fact("YOU HAVE V" + mine->version, plate_grey, theme::paper);
    y += plate_h + 24 * u;
    // Its buttons, from the foot up.
    const float bh = 84 * u, between = 14 * u;
    float by = z.y - inset - bh;
    if (button(ui, hash("mods-panel-page"), ImVec2(left, by), ImVec2(edge, by + bh), "FULL PAGE", Look::plain)) open_details(ui, m, *package);
    by -= bh + between;
    if (action_of(m, *package) == ms::Action::update && mine && !mine->gone) {
        if (button(ui, hash("mods-panel-toggle"), ImVec2(left, by), ImVec2(edge, by + bh), mine->enabled ? "SWITCH OFF" : "SWITCH ON", Look::plain))
            ms::set_enabled(mine->folder, !mine->enabled);
        by -= bh + between;
    }
    const auto does = main_action(m, *package);
    if (button(ui, hash("mods-panel-action"), ImVec2(left, by), ImVec2(edge, by + bh), does.text, does.primary ? Look::primary : Look::plain, does.enabled))
        run_main_action(m, *package);
    // As much of the description as fits above them.
    if (package->deprecated) {
        y += paragraph(draw, ui.bold, 28 * u, ImVec2(left, y), edge - left, IM_COL32(255, 138, 138, 255), "Its author marked it deprecated: it is no longer looked after.", 2) + 10 * u;
    }
    const auto lines = static_cast<std::size_t>(std::clamp((by - 20 * u - y) / (30 * u * 1.3f), 0.f, 14.f));
    if (lines) paragraph(draw, ui.body, 30 * u, ImVec2(left, y), edge - left, soft, version.description, lines);
}

// The grid, as many cards across as fit at about 330 units each (four to eight), scrolled down, under a line saying what it is: the rail row's mods
// (refresh_view) that the search, the categories chosen and HIDE INSTALLED leave, in its order.
void browse_grid(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    auto &h = ui.h;
    const float u = ui.u, spacing = 20 * u;
    // More cards a row than the five it began with, so a view shows more mods at once.
    const float room = z.x - a.x - 24 * u;
    const std::size_t across = std::clamp<std::size_t>(static_cast<std::size_t>((room + spacing) / (330 * u + spacing)), 4, 8);
    const float card_w = (room - spacing * static_cast<float>(across - 1)) / static_cast<float>(across), picture_h = card_w * .62f,
                card_h = picture_h + 140 * u;
    const auto &cards = m.results;
    // Which card the details show, settled before any is drawn, so the blue edge is on that card from
    // the frame the focus moves: the one the D-pad or the keys are on (not the pointer passing over,
    // or the details would change under it on its way to them); else the one picked, if it is still
    // in its place; else the first place its mod has now (the order changed); else the first card.
    if (h.ring)
        for (std::size_t i = 0; i < cards.size(); ++i)
            if (m.card_ids[i] == h.focus) m.picked = cards[i]->full_name, m.picked_at = i;
    if (m.picked_at >= cards.size() || cards[m.picked_at]->full_name != m.picked) {
        const auto found = std::find_if(cards.begin(), cards.end(), [&](const ts::Package *package) { return package->full_name == m.picked; });
        if (found == cards.end()) m.picked = cards.empty() ? std::string() : cards.front()->full_name, m.picked_at = 0;
        else m.picked_at = static_cast<std::size_t>(found - cards.begin());
    }
    Scroller list(ui, hash("mods-browse"), a, z);
    bool shown{};
    const float head = list.next(56 * u, 10 * u, shown);
    std::string count = rail_name(m) + "  \xc2\xb7  " + std::to_string(cards.size()) + (cards.size() == 1 ? " mod" : " mods");
    if (!m.search.empty()) count += " matching \"" + m.search + "\"";
    if (!m.only.empty() || !m.hidden.empty()) count += "  \xc2\xb7  " + category_summary(m);
    if (shown) label(draw, ui.body, 32 * u, ImVec2(a.x, head + 8 * u), z.x, theme::muted, count);
    for (std::size_t i = 0; i < cards.size(); i += across) {
        const float top = list.next(card_h, spacing, shown);
        for (std::size_t j = i; j < std::min(cards.size(), i + across); ++j) {
            // (Rows out of view still take their place in the focus order, near it, for the D-pad.)
            if (!shown && (top + card_h < a.y - card_h * 2 || top > z.y + card_h * 2)) continue;
            const auto &package = *cards[j];
            const auto id = m.card_ids[j];
            const ImVec2 p(a.x + static_cast<float>(j - i) * (card_w + spacing), top);
            const auto press = package_card(ui, m, id, j, p, ImVec2(p.x + card_w, top + card_h), picture_h, package, &list.box);
            if (press == CardPress::main) run_main_action(m, package);
            else if (press == CardPress::page) open_details(ui, m, package);
            list.reveal(id, top, card_h);
        }
    }
    if (cards.empty()) {
        const float top = list.next(60 * u, 0, shown);
        draw->AddText(ui.body, 36 * u, ImVec2(a.x, top), theme::muted, "Nothing matches. Try other words, another category, or ALL.");
    }
    list.end();
}

void browse_page(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, gap = 32 * u, bar = 96 * u;
    m.card_focused = false;
    if (!m.details.empty()) {
        if (const auto *package = package_named(m, m.details)) return package_details(ui, m, *package, a, z);
        m.details.clear();
    }
    const float rail_w = 540 * u, panel_w = std::clamp((z.x - a.x) * .24f, 760 * u, 1000 * u);
    const ImVec2 ma(a.x + rail_w + gap, a.y), mz(z.x - panel_w - gap, z.y);
    category_rail(ui, m, a, ImVec2(a.x + rail_w, z.y));
    browse_bar(ui, m, ma, mz.x, bar);
    const ImVec2 ca(ma.x, ma.y + bar + 24 * u);
    if (m.categories_open) return categories_panel(ui, m, ca, z);
    const auto &listing = *m.listing;
    if (listing.packages.empty()) {
        const auto line = !listing.error.empty() && !listing.fetching ? "Thunderstore could not be reached: " + listing.error
                          : std::string("Getting the mods from Thunderstore...");
        label(draw, ui.body, 36 * u, ImVec2(ca.x, ca.y + 30 * u), z.x, theme::muted, line);
        if (!listing.fetching && button(ui, hash("mods-retry"), ImVec2(ca.x, ca.y + 110 * u), ImVec2(ca.x + 500 * u, ca.y + 206 * u), "TRY AGAIN", Look::primary))
            ms::refresh();
        return;
    }
    browse_grid(ui, m, ca, mz);
    browse_panel(ui, m, package_named(m, m.picked), ImVec2(z.x - panel_w, a.y), z);
}

// ---------------------------------------------------------------- MY MODS

// A mod as the Insert menu's MODS library rows show it: its picture, its name with what it is and
// its version (or what is wrong) under it, its state as a tag at the right end, and a switch.
void installed_page(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, gap = 28 * u, bar = 96 * u, list_width = (z.x - a.x) * .62f;
    if (!m.mods) {
        draw->AddText(ui.body, 40 * u, ImVec2(a.x, a.y + 30 * u), theme::muted, "Reading the Mods folder...");
        return;
    }
    const auto &mods = *m.mods;
    // ---- the bar: search and which to show, as the server browser's chips
    {
        const float search_width = std::min(900 * u, list_width * .45f);
        field(ui, hash("mods-installed-search"), a, ImVec2(a.x + search_width, a.y + bar), m.installed_search, "Search your mods", false, 64);
        float x = a.x + search_width + 20 * u;
        const auto filter = [&](const char *text, int which) {
            if (chip(ui, hash(text, 23), x, a.y + 12 * u, a.y + bar - 12 * u, text, m.installed_filter == which))
                m.installed_filter = which, ui.h.scrolls[hash("mods-installed")] = 0;
        };
        filter("ALL", 0);
        filter("ON", 1);
        filter("OFF", 2);
        filter("NOT APPLIED", 3);
    }
    const auto lower = [](std::string text) {
        for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const auto wanted = lower(m.installed_search);
    std::vector<const ms::Mod *> rows;
    for (const auto &mod : mods.mods) {
        if (m.installed_filter == 1 && !mod.enabled) continue;
        if (m.installed_filter == 2 && mod.enabled) continue;
        if (m.installed_filter == 3 && mod.state != ms::State::apply_to_load && mod.state != ms::State::apply_to_unload) continue;
        if (!wanted.empty() && lower(mod.title).find(wanted) == std::string::npos && lower(mod.author).find(wanted) == std::string::npos &&
            lower(mod.folder).find(wanted) == std::string::npos)
            continue;
        rows.push_back(&mod);
    }
    if (std::none_of(rows.begin(), rows.end(), [&](const ms::Mod *mod) { return mod->folder == m.selected; }))
        m.selected = rows.empty() ? std::string() : rows.front()->folder;
    const auto package_of = [&](const std::string &folder) -> const ts::Package * {
        const auto found = m.by_folder.find(folder);
        return found == m.by_folder.end() ? nullptr : found->second;
    };

    // ---- the list
    const ImVec2 la(a.x, a.y + bar + gap), lz(a.x + list_width, z.y);
    const float row_height = 120 * u, row_gap = 10 * u, pad = 24 * u;
    {
        Scroller list(ui, hash("mods-installed"), la, lz);
        for (const auto *mod : rows) {
            const auto id = hash(mod->folder, 51);
            bool drawn{};
            const float top = list.next(row_height, row_gap, drawn);
            const ImVec2 p(la.x, top), q(lz.x - 20 * u, top + row_height);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, top, row_height);
            if (hit.focus && (ui.h.ring || ui.moved || ui.clicked)) m.selected = mod->folder;
            // Pressing a row switches it on or off.
            if (hit.press && !mod->gone) ms::set_enabled(mod->folder, !mod->enabled);
            if (!drawn) continue;
            // White under the D-pad or the keys, a lighter grey under the pointer; the one the panel
            // shows has the kit's blue edge, and one with a problem an edge in its tag's colour.
            const auto [focused, pointed] = lit_of(hit);
            const auto tag = state_tag(*mod);
            theme::tile_rect(draw, p, q, focused ? theme::focus_fill : pointed ? row_hover : tile_dark, 0xa00 + (id & 0xff), u * 2);
            if (m.selected == mod->folder) draw->AddRectFilled(p, ImVec2(p.x + 8 * u, q.y), focused ? focus_ink : accent);
            else if (tag.problem) draw->AddRectFilled(p, ImVec2(p.x + 8 * u, q.y), tag.fill);
            const float icon = row_height - 24 * u;
            package_icon(ui, ImVec2(p.x + pad, p.y + 12 * u), ImVec2(p.x + pad + icon, p.y + 12 * u + icon), package_of(mod->folder), mod->title, !mod->enabled);
            const float tx = p.x + pad * 2 + icon, middle = p.y + row_height * .5f;
            const float tag_x = q.x - pad - row_tag(ui, q.x - pad, middle, tag.text, on_row(focused, tag.fill), tag.ink);
            // The switch: on or off at a glance, on the blue when on as the ON tag is.
            const float sw = 84 * u, sx = tag_x - sw - 24 * u, sy = middle - 22 * u;
            if (!mod->gone) {
                draw->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + 44 * u), mod->enabled ? on_row(focused, accent) : focused ? sub_ink : track, 22 * u);
                draw->AddCircleFilled(ImVec2(mod->enabled ? sx + sw - 22 * u : sx + 22 * u, sy + 22 * u), 17 * u, theme::paper, 20);
            }
            label(draw, ui.bold, 40 * u, ImVec2(tx, p.y + 18 * u), sx - 20 * u, focused ? focus_ink : mod->enabled || pointed ? theme::paper : theme::muted, mod->title);
            label(draw, ui.body, 28 * u, ImVec2(tx, p.y + 70 * u), sx - 20 * u, focused ? sub_ink_lit : tag.detail_colour, tag.detail);
        }
        if (rows.empty()) {
            bool shown{};
            const float top = list.next(60 * u, 0, shown);
            draw->AddText(ui.body, 38 * u, ImVec2(la.x + pad, top), theme::muted,
                          !mods.present ? "There is no Mods folder yet: install a mod from BROWSE." : mods.mods.empty() ? "No mods installed yet. Find some in BROWSE." : "No mod matches.");
        }
        list.end();
    }
    if (!mods.issue.empty()) label(draw, ui.body, 30 * u, ImVec2(la.x, z.y + 6 * u), lz.x, skate_theme::danger, "mods.json: " + mods.issue);

    // ---- beside it: the chosen mod
    const ImVec2 da(lz.x + gap, la.y), dz(z.x, z.y);
    theme::tile_rect(draw, da, dz, tile_dark, 0x7a, u * 2);
    const float inset = 40 * u, left = da.x + inset, edge = dz.x - inset;
    const ms::Mod *mod{};
    for (const auto *row : rows)
        if (row->folder == m.selected) mod = row;
    if (!mod) {
        draw->AddText(ui.body, 36 * u, ImVec2(left, da.y + inset), theme::muted, "Pick a mod to see it here.");
        return;
    }
    const auto *package = package_of(mod->folder);
    const auto tag = state_tag(*mod);
    float y = da.y + inset;
    const float icon = 220 * u;
    package_icon(ui, ImVec2(left, y), ImVec2(left + icon, y + icon), package, mod->title, !mod->enabled);
    label(draw, ui.heading, 52 * u, ImVec2(left + icon + 30 * u, y + 4 * u), edge, theme::paper, mod->title);
    label(draw, ui.body, 30 * u, ImVec2(left + icon + 30 * u, y + 76 * u), edge, sub_ink,
          (mod->author.empty() ? std::string("Unknown author") : "by " + mod->author) + (mod->version.empty() ? std::string() : "   v" + mod->version));
    plate(ui, ImVec2(left + icon + 30 * u, y + 130 * u), 44 * u, tag.text, tag.fill, tag.ink);
    y += icon + 30 * u;
    if (!mod->why.empty()) y += paragraph(draw, ui.body, 30 * u, ImVec2(left, y), edge - left, tag.problem ? tag.detail_colour : soft, mod->why, 3) + 16 * u;
    if (!mod->description.empty()) y += paragraph(draw, ui.body, 30 * u, ImVec2(left, y), edge - left, soft, mod->description, 4) + 16 * u;
    const float bh = 88 * u;
    float by = dz.y - inset - bh;
    if (!mod->gone) {
        const bool confirming = m.confirm == mod->folder && GetTickCount64() < m.confirm_until;
        if (button(ui, hash("mods-installed-remove"), ImVec2(left, by), ImVec2(edge, by + bh), confirming ? "PRESS AGAIN TO UNINSTALL" : "UNINSTALL",
                   Look::plain, mod->state != ms::State::removal_waiting)) {
            if (confirming) {
                m.confirm.clear();
                if (const auto refused = ms::uninstall(mod->folder); !refused.empty()) mod_say(m, refused, true);
            } else m.confirm = mod->folder, m.confirm_until = GetTickCount64() + 4000;
        }
        by -= bh + 16 * u;
    }
    if (package) {
        if (button(ui, hash("mods-installed-page"), ImVec2(left, by), ImVec2(edge, by + bh), "ITS PAGE", Look::plain))
            open_details(ui, m, *package);
        by -= bh + 16 * u;
        if (ts::update_available(*package, mods.versions)) {
            const auto does = queue_button(m, *package, ms::Action::update, "UPDATE TO v" + package->latest().number);
            if (button(ui, hash("mods-installed-update"), ImVec2(left, by), ImVec2(edge, by + bh), does.text, does.enabled ? Look::primary : Look::plain, does.enabled))
                start_install(m, {package->full_name});
        }
        by -= bh + 16 * u;
    }
    if (!mod->gone && button(ui, hash("mods-installed-toggle"), ImVec2(left, by), ImVec2(edge, by + bh), mod->enabled ? "SWITCH OFF" : "SWITCH ON",
                             mod->enabled ? Look::plain : Look::primary))
        ms::set_enabled(mod->folder, !mod->enabled);
}

// ---------------------------------------------------------------- UPDATES

// What Thunderstore has newer, in the server browser's look: a row a mod with columns for the
// version it goes from and to, when it was updated and its size, and UPDATE on the blue at the end.
void updates_page(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, gap = 28 * u, bar = 96 * u, pad = 24 * u, pad_x = 30 * u, head = 46 * u;
    const auto progress = ms::progress();
    const auto count = m.updates.size();
    // ---- the bar: how many, and UPDATE ALL
    const std::string title = count == 0 ? "EVERYTHING IS UP TO DATE" : count == 1 ? "1 MOD HAS AN UPDATE" : std::to_string(count) + " MODS HAVE UPDATES";
    draw->AddText(ui.bold, 44 * u, ImVec2(a.x, a.y + (bar - 44 * u) * .5f), theme::paper, title.c_str());
    if (count) {
        std::vector<std::string> all;
        std::uint64_t bytes{};
        for (const auto *package : m.updates) all.push_back(package->full_name), bytes += package->latest().file_size;
        std::erase_if(all, [&](const std::string &name) { return std::find(progress.queued.begin(), progress.queued.end(), name) != progress.queued.end() ||
                                                                (progress.busy && progress.package == name); });
        if (button(ui, hash("mods-update-all"), ImVec2(z.x - 640 * u, a.y), ImVec2(z.x, a.y + bar),
                   all.empty() ? std::string("ALL QUEUED") : (progress.busy ? "QUEUE ALL  (" : "UPDATE ALL  (") + ms::size_text(bytes) + ")",
                   all.empty() ? Look::plain : Look::primary, !all.empty()))
            start_install(m, all);
    }
    const ImVec2 la(a.x, a.y + bar + gap), lz(z.x, z.y);
    const float row_height = 120 * u, row_gap = 10 * u, right = lz.x - 20 * u, icon = row_height - 24 * u, column_gap = 40 * u;
    // The columns, from the right: what pressing it does, the size, when it was updated, the version it
    // goes from and to; the name has what is left. A narrow page folds the date and size in under the name.
    const float chip_left = right - pad_x - 340 * u, size_right = chip_left - column_gap, size_left = size_right - 200 * u;
    const float date_right = size_left - column_gap, date_left = date_right - 320 * u;
    const float name_left = la.x + pad * 2 + icon;
    const bool wide = date_left - column_gap - 440 * u - column_gap - name_left >= 600 * u;
    const float change_right = wide ? date_left - column_gap : chip_left - column_gap, change_left = change_right - 440 * u;
    const float name_right = change_left - column_gap;
    if (count) {
        const float size = 24 * u, y = la.y + (head - size) * .5f - 4 * u;
        draw->AddText(ui.bold, size, ImVec2(name_left, y), sub_ink, "MOD");
        draw->AddText(ui.bold, size, ImVec2(change_left, y), sub_ink, "VERSION");
        if (wide) {
            draw->AddText(ui.bold, size, ImVec2(date_left, y), sub_ink, "UPDATED");
            draw->AddText(ui.bold, size, ImVec2(size_right - width_of(ui.bold, size, "SIZE"), y), sub_ink, "SIZE");
        }
    }
    Scroller list(ui, hash("mods-updates"), ImVec2(la.x, la.y + (count ? head : 0)), lz);
    const auto installed_version = [&](const ts::Package &package) -> std::string {
        if (!m.mods) return {};
        const auto found = m.mods->versions.find(ts::folder_for(package.full_name));
        return found == m.mods->versions.end() ? std::string() : found->second;
    };
    for (const auto *package : m.updates) {
        const auto id = hash(package->full_name, 61);
        bool drawn{};
        const float top = list.next(row_height, row_gap, drawn);
        const ImVec2 p(la.x, top), q(right, top + row_height);
        const Hit hit = item(ui, id, p, q, &list.box);
        list.reveal(id, top, row_height);
        const auto does_now = queue_button(m, *package, ms::Action::update, "UPDATE");
        if (hit.press && does_now.enabled) start_install(m, {package->full_name});
        if (!drawn) continue;
        const auto [focused, pointed] = lit_of(hit);
        const ImU32 ink = focused ? focus_ink : theme::paper, sub = focused ? sub_ink_lit : sub_ink;
        theme::tile_rect(draw, p, q, focused ? theme::focus_fill : pointed ? row_hover : tile_dark, 0xb00 + (id & 0xff), u * 2);
        package_icon(ui, ImVec2(p.x + pad, p.y + 12 * u), ImVec2(p.x + pad + icon, p.y + 12 * u + icon), package, package->title());
        // The name, with its maker (and, on a narrow page, the date and size) under it.
        const std::string when = ms::date_text(package->date_updated), bytes = package->latest().file_size ? ms::size_text(package->latest().file_size) : std::string("-");
        label(draw, ui.bold, 40 * u, ImVec2(name_left, p.y + 18 * u), name_right, ink, package->title());
        label(draw, ui.body, 28 * u, ImVec2(name_left, p.y + 70 * u), name_right, sub,
              "by " + package->owner + (wide ? std::string() : "  \xc2\xb7  " + when + "  \xc2\xb7  " + bytes));
        // The version change: the one installed in grey, the new one in white.
        const float cell = 34 * u, cell_y = p.y + (row_height - cell) * .5f;
        {
            const auto have = installed_version(*package);
            const std::string from = "v" + (have.empty() ? std::string("?") : have) + "  >  ";
            const float from_w = std::min(width_of(ui.body, cell, from), change_right - change_left);
            label(draw, ui.body, cell, ImVec2(change_left, cell_y), change_right, sub, from);
            label(draw, ui.bold, cell, ImVec2(change_left + from_w, cell_y), change_right, ink, "v" + package->latest().number);
        }
        if (wide) {
            label(draw, ui.body, 32 * u, ImVec2(date_left, cell_y), date_right, ink, when);
            draw->AddText(ui.body, 32 * u, ImVec2(size_right - width_of(ui.body, 32 * u, bytes), cell_y), ink, bytes.c_str());
        }
        // What pressing it does: UPDATE (or ADD TO QUEUE) on the blue, or what it is doing on grey.
        const auto &does = does_now.text;
        const float chip_size = 32 * u;
        const ImVec2 c(chip_left, p.y + 30 * u), d(right - pad_x, q.y - 30 * u);
        theme::tile_rect(draw, c, d, does_now.enabled ? (focused ? skate_theme::black : primary_rest) : focused ? track_lit : track, 0x52, u * 2);
        label(draw, ui.bold, chip_size, ImVec2(c.x + std::max(16 * u, (d.x - c.x - width_of(ui.bold, chip_size, does)) * .5f), c.y + (d.y - c.y - chip_size) * .5f), d.x - 16 * u,
              does_now.enabled ? (focused ? theme::paper : focus_ink) : focused ? focus_ink : theme::paper, does);
    }
    if (!count) {
        bool shown{};
        const float top = list.next(60 * u, 0, shown);
        draw->AddText(ui.body, 38 * u, ImVec2(la.x, top), theme::muted,
                      m.listing->packages.empty() ? "Getting the mods from Thunderstore..." : "Every mod you have from Thunderstore is its newest version.");
    }
    list.end();
}

// ---------------------------------------------------------------- PLUGINS

// PLUGINS, as the Insert menu's (plugins_page.cpp): plugin developer mode first, then every installed
// plugin a row (its name; who made it and the kinds of thing it can do, or what it asks for; PAGE > when
// it has a page; ON, OFF or ASKS), and beside the list the one picked: everything it may do, each kind
// in its risk's colour, and the buttons that switch it or answer it. A opens a plugin's page and X
// switches it. One waiting for an answer is only ever answered beside the list, under what it asks
// for: A and X take the focus to TURN ON there, never switch it blind.
void plugin_list_tab(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    namespace pl = dingosdk::plugins;
    auto *draw = ui.draw;
    auto &h = ui.h;
    const float u = ui.u, gap = 28 * u, pad = 24 * u, list_width = (z.x - a.x) * .62f;
    const auto installed = pl::installed();
    const auto pages = plugin_ui::latest_pages();
    const auto has_page = [&](const std::string &id) {
        return pages && std::any_of(pages->begin(), pages->end(), [&](const plugin_ui::PageEntry &page) { return page.plugin == id; });
    };
    const std::uint32_t pressed = ui.input && !h.typing ? ui.pad.buttons & ~h.pad_before : 0;
    if (std::none_of(installed.begin(), installed.end(), [&](const pl::Installed &plugin) { return plugin.id == m.plugin_selected; }))
        m.plugin_selected = installed.empty() ? std::string() : installed.front().id;
    const auto answer = [&](const std::string &id) {
        m.plugin_selected = id;
        h.focus_next = hash("mods-plugin-turn-on");
    };

    // ---- the list
    const ImVec2 la(a.x, a.y), lz(a.x + list_width, z.y);
    const float row_height = 120 * u, row_gap = 10 * u;
    {
        Scroller list(ui, hash("mods-plugins"), la, lz);
        // Plugin developer mode, as a row with its switch.
        {
            const auto id = hash("mods-plugin-dev-mode");
            bool drawn{};
            const float top = list.next(row_height, row_gap, drawn);
            const ImVec2 p(la.x, top), q(lz.x - 20 * u, top + row_height);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, top, row_height);
            const bool on = pl::developer_mode();
            if (hit.press && !pl::set_developer_mode(!on)) mod_say(m, "Plugin dev mode could not be saved.", true);
            if (drawn) {
                const auto [focused, pointed] = lit_of(hit);
                theme::tile_rect(draw, p, q, focused ? theme::focus_fill : pointed ? row_hover : tile_dark, id & 0xfff, u * 2);
                const float sw = 84 * u, sx = q.x - pad - sw, sy = p.y + row_height * .5f - 22 * u;
                draw->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + 44 * u), on ? on_row(focused, accent) : focused ? sub_ink : track, 22 * u);
                draw->AddCircleFilled(ImVec2(on ? sx + sw - 22 * u : sx + 22 * u, sy + 22 * u), 17 * u, theme::paper, 20);
                label(draw, ui.bold, 40 * u, ImVec2(p.x + pad, p.y + 18 * u), sx - 20 * u, focused ? focus_ink : theme::paper, "PLUGIN DEV MODE");
                label(draw, ui.body, 28 * u, ImVec2(p.x + pad, p.y + 70 * u), sx - 20 * u, focused ? sub_ink_lit : sub_ink,
                      "Updates to plugins you already approved won't ask again.");
            }
        }
        {
            bool drawn{};
            const float top = list.next(64 * u, 0, drawn);
            const auto asking = std::count_if(installed.begin(), installed.end(), [](const pl::Installed &plugin) { return plugin.approval == pl::Approval::asking; });
            if (drawn) {
                const std::string heading = "INSTALLED  \xc2\xb7  " + std::to_string(installed.size());
                group_heading(ui, ImVec2(la.x, top + 22 * u), heading);
                if (asking)
                    label(draw, ui.bold, 28 * u, ImVec2(la.x + width_of(ui.bold, 28 * u, heading) + 40 * u, top + 22 * u), lz.x - 20 * u, skate_theme::warning,
                          asking == 1 ? std::string("A plugin is waiting for your OK.") : std::to_string(asking) + " plugins are waiting for your OK.");
            }
        }
        for (const auto &plugin : installed) {
            const auto id = hash(plugin.id, 91);
            const bool waiting = plugin.approval == pl::Approval::asking, on = plugin.approval == pl::Approval::on, page = has_page(plugin.id);
            bool drawn{};
            const float top = list.next(row_height, row_gap, drawn);
            const ImVec2 p(la.x, top), q(lz.x - 20 * u, top + row_height);
            const Hit hit = item(ui, id, p, q, &list.box);
            list.reveal(id, top, row_height);
            if (hit.focus && (h.ring || ui.moved || ui.clicked)) m.plugin_selected = plugin.id;
            if (hit.press) {
                m.plugin_selected = plugin.id;
                if (waiting) answer(plugin.id);
                else if (page) open_plugin_page(h, m, plugin.id);
                else h.focus_next = hash("mods-plugin-switch");
            } else if (h.focus == id && h.ring && (pressed & 0x4000)) {
                h.sound = sound_select;
                if (waiting) answer(plugin.id);
                else pl::set_enabled(plugin.id, !on);
            }
            if (!drawn) continue;
            // As MY MODS' rows: white under the D-pad or the keys, a lighter grey under the pointer; the
            // one the panel shows has the kit's blue edge, and one waiting for an answer an amber one.
            const auto [focused, pointed] = lit_of(hit);
            theme::tile_rect(draw, p, q, focused ? theme::focus_fill : pointed ? row_hover : tile_dark, 0xc00 + (id & 0xff), u * 2);
            if (m.plugin_selected == plugin.id) draw->AddRectFilled(p, ImVec2(p.x + 8 * u, q.y), focused ? focus_ink : accent);
            else if (waiting) draw->AddRectFilled(p, ImVec2(p.x + 8 * u, q.y), skate_theme::warning);
            const float icon = row_height - 24 * u;
            package_icon(ui, ImVec2(p.x + pad, p.y + 12 * u), ImVec2(p.x + pad + icon, p.y + 12 * u + icon), nullptr, plugin.title, !on && !waiting);
            const float tx = p.x + pad * 2 + icon, middle = p.y + row_height * .5f;
            const char *tag = waiting ? "ASKS" : on ? "ON" : "OFF";
            float right = q.x - pad - row_tag(ui, q.x - pad, middle, tag, waiting ? skate_theme::warning : on ? on_row(focused, accent) : plate_grey,
                                               waiting ? skate_theme::black : on ? theme::paper : chip_ink) - 24 * u;
            if (page && !waiting) {
                const char *side = "PAGE  \xe2\x80\xba";
                right -= width_of(ui.body, 28 * u, side);
                draw->AddText(ui.body, 28 * u, ImVec2(right, middle - 14 * u), focused ? sub_ink_lit : sub_ink, side);
                right -= 24 * u;
            }
            std::string kinds;
            for (const auto &group : plugin.can) kinds += (kinds.empty() ? "" : ", ") + group.category;
            std::string detail;
            if (waiting) detail = kinds.empty() ? std::string("Asks to run") : "Asks for: " + kinds;
            else detail = (plugin.author.empty() ? std::string() : "by " + plugin.author) + (kinds.empty() || plugin.author.empty() ? "" : "  \xc2\xb7  ") + kinds;
            label(draw, ui.bold, 40 * u, ImVec2(tx, p.y + 18 * u), right, focused ? focus_ink : on || waiting || pointed ? theme::paper : theme::muted, plugin.title);
            label(draw, ui.body, 28 * u, ImVec2(tx, p.y + 70 * u), right, focused ? sub_ink_lit : waiting ? skate_theme::warning : sub_ink, detail);
        }
        if (installed.empty()) {
            bool shown{};
            const float top = list.next(120 * u, 0, shown);
            paragraph(draw, ui.body, 32 * u, ImVec2(la.x + pad, top), lz.x - la.x - pad * 2, theme::muted,
                      "No plugin is installed. A plugin is a mod with a reskate-plugin.json: a script that runs inside the game and can add pages, "
                      "modes and tools. Find them in BROWSE.");
        }
        list.end();
    }

    // ---- beside it: the plugin picked
    const ImVec2 da(lz.x + gap, la.y), dz(z.x, z.y);
    theme::tile_rect(draw, da, dz, tile_dark, 0x7c, u * 2);
    const float inset = 40 * u, left = da.x + inset, edge = dz.x - inset;
    const pl::Installed *plugin{};
    for (const auto &each : installed)
        if (each.id == m.plugin_selected) plugin = &each;
    if (!plugin) {
        draw->AddText(ui.body, 36 * u, ImVec2(left, da.y + inset), theme::muted, "Pick a plugin to see what it may do.");
        return;
    }
    const bool waiting = plugin->approval == pl::Approval::asking, on = plugin->approval == pl::Approval::on, page = has_page(plugin->id);
    float y = da.y + inset;
    label(draw, ui.heading, 52 * u, ImVec2(left, y), edge, theme::paper, plugin->title);
    y += 76 * u;
    label(draw, ui.body, 30 * u, ImVec2(left, y), edge, sub_ink, plugin->author.empty() ? std::string("Unknown author") : "by " + plugin->author);
    y += 52 * u;
    plate(ui, ImVec2(left, y), 42 * u, waiting ? "WAITING FOR YOUR OK" : on ? "ON" : "OFF", waiting ? skate_theme::warning : on ? accent : plate_grey,
          waiting ? skate_theme::black : on ? theme::paper : chip_ink);
    y += 70 * u;
    // Its buttons at the foot: TURN ON and KEEP OFF for one waiting for an answer, under what it asks
    // for; else TURN ON / OFF, and OPEN PAGE when it has one.
    const float bh = 88 * u, between = 16 * u;
    float by = dz.y - inset - bh;
    if (waiting) {
        const float half = (edge - left - between) * .5f;
        if (button(ui, hash("mods-plugin-turn-on"), ImVec2(left, by), ImVec2(left + half, by + bh), "TURN ON", Look::primary)) pl::set_enabled(plugin->id, true);
        if (button(ui, hash("mods-plugin-keep-off"), ImVec2(edge - half, by), ImVec2(edge, by + bh), "KEEP OFF", Look::plain)) pl::set_enabled(plugin->id, false);
    } else {
        if (page) {
            if (button(ui, hash("mods-plugin-open"), ImVec2(left, by), ImVec2(edge, by + bh), "OPEN PAGE", Look::plain)) open_plugin_page(h, m, plugin->id);
            by -= bh + between;
        }
        if (button(ui, hash("mods-plugin-switch"), ImVec2(left, by), ImVec2(edge, by + bh), on ? "TURN OFF" : "TURN ON", on ? Look::plain : Look::primary))
            pl::set_enabled(plugin->id, !on);
    }
    // What it may do, scrolled with the wheel, or by moving the focus down its kinds.
    group_heading(ui, ImVec2(left, y), waiting ? "IT ASKS TO" : "WHAT IT MAY DO");
    y += 52 * u;
    Scroller list(ui, hash("mods-plugin-can"), ImVec2(left, y), ImVec2(edge, by - 24 * u));
    if (plugin->can.empty()) {
        bool shown{};
        const float top = list.next(40 * u, 0, shown);
        if (shown) draw->AddText(ui.body, 30 * u, ImVec2(left, top), soft, "Nothing past what every plugin may do.");
    }
    for (std::size_t g = 0; g < plugin->can.size(); ++g) {
        const auto &group = plugin->can[g];
        const ImU32 colour = group.risk == pl::Risk::red ? IM_COL32(255, 138, 138, 255) : group.risk == pl::Risk::amber ? skate_theme::warning : chip_ink;
        bool shown{};
        const float top = list.next(46 * u, 4 * u, shown);
        const auto stop = hash(plugin->id, 300 + g);
        const Hit hit = item(ui, stop, ImVec2(left, top), ImVec2(edge - 20 * u, top + 46 * u), &list.box);
        list.reveal(stop, top, 46 * u);
        if (hit.focus && h.ring) draw->AddRectFilled(ImVec2(left, top), ImVec2(left + 6 * u, top + 46 * u), theme::paper);
        if (shown) draw->AddText(ui.bold, 32 * u, ImVec2(left + 16 * u, top + 6 * u), colour, group.category.c_str());
        for (const auto &phrase : group.phrases) {
            bool first = true;
            for (const auto &line : wrap_text(ui.body, 28 * u, phrase, edge - left - 80 * u)) {
                const float at = list.next(28 * u * 1.3f, 0, shown);
                if (!shown) continue;
                if (std::exchange(first, false)) draw->AddCircleFilled(ImVec2(left + 34 * u, at + 16 * u), 5 * u, chip_ink, 12);
                draw->AddText(ui.body, 28 * u, ImVec2(left + 56 * u, at), soft, line.c_str());
            }
        }
        list.next(0, 14 * u, shown);
    }
    list.end();
}

// The arrow keys, Enter and Escape to ImGui while a plugin's page has its navigation (State::hub_nav):
// the overlay does not take the keyboard from the game for that, so they are read here, as hub_frame
// reads them for the page's own focus, and let go when the page closes.
void feed_page_keys(ModPage &m, bool on) {
    if (!on && !m.keys_fed) return;
    static constexpr std::array<std::pair<int, ImGuiKey>, 6> keys{{{VK_UP, ImGuiKey_UpArrow}, {VK_DOWN, ImGuiKey_DownArrow}, {VK_LEFT, ImGuiKey_LeftArrow},
                                                                   {VK_RIGHT, ImGuiKey_RightArrow}, {VK_RETURN, ImGuiKey_Enter}, {VK_ESCAPE, ImGuiKey_Escape}}};
    auto &io = ImGui::GetIO();
    OverlayInputAccess access;
    for (const auto &[code, key] : keys) io.AddKeyEvent(key, on && (GetAsyncKeyState(code) & 0x8000) != 0);
    m.keys_fed = on;
}

// A plugin's page, full size in place of the list, drawn by the Insert menu's own code for plugin pages
// (menu::pause_menu_plugin_page), so a plugin's page is the same in both menus. Its widgets are ImGui's,
// with ImGui's focus: while it shows, the pad and the keys go to ImGui's navigation (hub_frame skips
// its own, State::hub_nav), and B, Escape or < PLUGINS come back to the list, the focus on its row.
void plugin_page_tab(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto &h = ui.h;
    const float u = ui.u, strip = 84 * u, back_w = 380 * u;
    std::string title = m.plugin_open;
    if (const auto pages = plugin_ui::latest_pages())
        for (const auto &page : *pages)
            if (page.plugin == m.plugin_open) title = page.title;
    // The way back for the pointer; the pad's is B.
    if (pointer_button(ui, a, ImVec2(a.x + back_w, a.y + strip), "< PLUGINS", Look::plain, true, nullptr)) return close_plugin_page(h, m);
    label(ui.draw, ui.heading, 52 * u, ImVec2(a.x + back_w + 36 * u, a.y + (strip - 52 * u) * .5f), z.x, theme::paper, title);
    // The kit's controls at about the page's own size: px(1) is 2.2 of the page's units, so a row of
    // the kit (48) is about as tall as one of the page's rows (120).
    const bool take_focus = ui.input && std::exchange(m.plugin_focus, false);
    const auto shown = menu::pause_menu_plugin_page(state().menu, m.plugin_open, ImVec2(a.x, a.y + strip + 24 * u), z, u * 2.2f, ui.input, take_focus);
    if (!shown.shown) {
        mod_say(m, title + " has no page now: it was turned off, or took its page away.");
        return close_plugin_page(h, m);
    }
    m.plugin_busy = shown.busy;
    if (shown.moved) h.sound = sound_navigate;
    // Typing in one of its text fields takes the keyboard from the game for that long, as the page's
    // own text boxes do (set_typing); the field gives it back with Enter or Escape.
    if (shown.typing && !h.typing) set_typing(h, plugin_typing);
    else if (!shown.typing && h.typing == plugin_typing) set_typing(h, 0);
}

// Changes not applied that are worth asking about: a reading of the mods from after the last apply,
// and nothing being applied or installed.
bool unapplied(const ModPage &m) {
    return m.mods && m.mods->settled && m.mods->changes && !live_mods::busy() && !m.progress.busy;
}
// The question about them (offer_apply, offer_leave): what applying them does to the level.
std::string apply_question(const ModPage &m) {
    const auto changes = m.mods->changes;
    const char *effect = m.mods->effect == live_mods::ApplyEffect::reload_level ? "Applying them now reloads the level you are on, so they show straight away."
                       : m.mods->effect == live_mods::ApplyEffect::leave_level ? "Applying them now takes you to San Van: the map you are on is going."
                       : "Applying them now adds the maps to the map list. Nothing is reloaded.";
    return std::to_string(changes) + (changes == 1 ? " change to your mods is" : " changes to your mods are") + " not in the game yet. " + effect +
           " You can also apply them later, from the Mod Manager.";
}
// Backing out of the pause menu from the Mod Manager with changes not applied: asked over the menu,
// which stays up (its Back is held) until the answer: LATER closes it (hub_frame).
bool guard_leave(const ModPage &m) { return m.leaving || (!m.declined && unapplied(m)); }
void offer_leave(ModPage &m) {
    if (question_asking() || message_asking()) return;
    m.leaving = true;
    ask_question("MOD MANAGER", "APPLY YOUR MOD CHANGES?", apply_question(m), "APPLY NOW", "LATER", [](bool yes) {
        if (yes) (void)ms::apply();
        mod_page().leave_answer.store(yes ? 1 : 2);
    });
}
// Asked once the pause menu has closed some other way after the Mod Manager was open (hub_page_pending).
void offer_apply(ModPage &m) {
    if (m.declined || m.leaving || !unapplied(m) || question_asking() || message_asking()) return;
    const auto changes = m.mods->changes;
    (void)changes;
    ask_question("MOD MANAGER", "APPLY YOUR MOD CHANGES?", apply_question(m), "APPLY NOW", "LATER", [](bool yes) {
                     if (yes) (void)ms::apply();
                 });
}

// ---------------------------------------------------------------- the page

void mod_manager_page(Ui &ui, ImVec2 a, ImVec2 z) {
    auto &m = mod_page();
    auto &h = ui.h;
    auto *draw = ui.draw;
    const float u = ui.u;
    refresh_view(m);
    // A mod's page belongs to BROWSE, and a plugin's page to PLUGINS: changing tab (LB / RB) leaves it.
    if (m.tab != browse_tab) m.details.clear(), m.categories_open = false;
    if (m.tab != plugins_tab) m.plugin_open.clear();
    m.progress = ms::progress();
    const auto &progress = m.progress;
    if (progress.serial != m.progress_serial) {
        // A job ended: say how it went.
        if (m.progress_serial && !progress.result.empty()) mod_say(m, progress.result, progress.failed);
        m.progress_serial = progress.serial;
    }
    if (m.notice_until && GetTickCount64() >= m.notice_until) m.notice.clear(), m.notice_until = 0;
    // (Asked about its changes when the pause menu closes: hub_page_pending. Not once they were asked
    // about as the player backed out, and said later.)
    if (!m.declined && !m.leaving) m.visited = true;
    // On BROWSE's grid X changes the order (where SORT does: sort_applies) and Y opens the picked mod's full page; View (Select)
    // applies the changes, from anywhere here. (B and Escape going back from a mod's page are hub_frame's.)
    const std::uint32_t pressed = ui.input && !h.typing ? ui.pad.buttons & ~h.pad_before : 0;
    const bool on_grid = m.tab == browse_tab && m.details.empty() && !m.categories_open;
    if (on_grid && sort_applies(m) && (pressed & 0x4000)) m.sort = static_cast<ms::Sort>((static_cast<std::size_t>(m.sort) + 1) % ms::sort_count), h.sound = sound_select;
    if (on_grid && (pressed & 0x8000))
        if (const auto *package = package_named(m, m.picked)) open_details(ui, m, *package), h.sound = sound_select;
    const auto changes = m.mods ? m.mods->changes : 0;
    const bool can_apply = changes && !live_mods::busy() && !progress.busy;
    if (can_apply && (pressed & 0x0020)) mod_say(m, ms::apply()), h.sound = sound_select;

    // The foot of the page: what is going on, and Apply.
    const float foot = 84 * u, gap = 24 * u;
    const ImVec2 body_z(z.x, z.y - foot - gap);
    switch (m.tab) {
    case browse_tab: browse_page(ui, m, a, body_z); break;
    case installed_tab: installed_page(ui, m, a, body_z); break;
    case updates_tab: updates_page(ui, m, a, body_z); break;
    case plugins_tab:
        if (m.plugin_open.empty()) plugin_list_tab(ui, m, a, body_z);
        else plugin_page_tab(ui, m, a, body_z);
        break;
    default: m.tab = browse_tab; break;
    }
    feed_page_keys(m, mods_imgui_nav(m) && ui.input && !h.typing);
    const float fy = z.y - foot;
    float right = z.x;
    if (changes && !live_mods::busy()) {
        std::string apply = live_mods::apply_label(m.mods->effect);
        for (auto &c : apply) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const float w = width_of(ui.bold, 34 * u, apply) + 100 * u;
        if (button(ui, hash("mods-apply"), ImVec2(right - w, fy), ImVec2(right, z.y), apply, Look::primary, !progress.busy)) mod_say(m, ms::apply());
        right -= w + 20 * u;
    }
    // A bar with how far something is: `fraction` below 0 is not known, and a block goes back and forth.
    // (On the grey track, the blue of the server browser's bars of how full a session is.)
    const auto bar = [&](ImVec2 b0, ImVec2 b1, float fraction) {
        draw->AddRectFilled(b0, b1, track, 6 * u);
        if (fraction >= 0) {
            draw->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * std::clamp(fraction, 0.f, 1.f), b1.y), accent, 6 * u);
            return;
        }
        const float t = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0)) / 2.f, w = (b1.x - b0.x) * .25f;
        const float x = b0.x + (b1.x - b0.x - w) * (t < .5f ? t * 2 : 2 - t * 2);
        draw->AddRectFilled(ImVec2(x, b0.y), ImVec2(x + w, b1.y), accent, 6 * u);
    };
    if (progress.busy) {
        // What is being installed: its icon and name, the step and which of how many, how much has
        // come, and a bar, with CANCEL at the end.
        const ImVec2 sa(a.x, fy), sz(right - 20 * u, z.y);
        theme::tile_rect(draw, sa, sz, tile_dark, 0x3b7, u * 2);
        const ImVec2 cancel_a(sz.x - 300 * u, fy + 10 * u), cancel_z(sz.x - 10 * u, z.y - 10 * u);
        if (button(ui, hash("mods-cancel"), cancel_a, cancel_z, progress.waiting ? "CANCEL ALL" : "CANCEL", Look::plain)) ms::cancel();
        float x = sa.x + 10 * u;
        const ts::Package *in_hand = package_named(m, progress.package);
        if (!in_hand)
            if (const auto found = m.by_folder.find(progress.package); found != m.by_folder.end()) in_hand = found->second;
        const float icon = foot - 20 * u;
        if (!progress.title.empty()) {
            package_icon(ui, ImVec2(x, fy + 10 * u), ImVec2(x + icon, fy + 10 * u + icon), in_hand, progress.title);
            x += icon + 20 * u;
        }
        using Step = ms::Progress::Step;
        const char *step = progress.step == Step::downloading ? "DOWNLOADING" : progress.step == Step::installing ? "INSTALLING"
                           : progress.step == Step::uninstalling ? "UNINSTALLING" : "STARTING";
        const float bar_w = std::min(900 * u, (cancel_a.x - x) * .42f), bar_x = cancel_a.x - 30 * u - bar_w;
        const std::string name = progress.title + (progress.version.empty() || progress.step == Step::uninstalling ? std::string() : "  v" + progress.version);
        const std::string where = std::string(step) + (progress.of > 1 ? "  " + std::to_string(progress.at + 1) + " OF " + std::to_string(progress.of) : std::string()) +
                                  (progress.waiting ? "   +" + std::to_string(progress.waiting) + " MORE IN THE QUEUE" : std::string());
        draw->AddText(ui.bold, 26 * u, ImVec2(x, fy + 8 * u), sub_ink, where.c_str());
        label(draw, ui.bold, 36 * u, ImVec2(x, fy + 38 * u), bar_x - 20 * u, theme::paper, name);
        std::string amount = progress.step == Step::downloading
            ? (progress.total ? ms::size_text(progress.received) + " of " + ms::size_text(progress.total) : ms::size_text(progress.received))
            : progress.step == Step::installing ? std::string("Putting it in your Mods folder") : std::string();
        if (progress.fraction >= 0 && progress.step != Step::uninstalling) amount += (amount.empty() ? "" : "   ") + std::to_string(static_cast<int>(progress.fraction * 100)) + "%";
        draw->AddText(ui.body, 26 * u, ImVec2(bar_x, fy + 10 * u), sub_ink, amount.c_str());
        bar(ImVec2(bar_x, fy + 46 * u), ImVec2(bar_x + bar_w, fy + 64 * u), progress.fraction);
        return;
    }
    if (live_mods::busy()) {
        // The merge: its step and how far, and what happens at the end of it.
        const auto applying = live_mods::progress();
        const ImVec2 sa(a.x, fy), sz(right - 20 * u, z.y);
        theme::tile_rect(draw, sa, sz, tile_dark, 0x3b8, u * 2);
        const float x = sa.x + 24 * u, bar_w = std::min(900 * u, (sz.x - x) * .42f), bar_x = sz.x - 24 * u - bar_w;
        const auto shown = std::min(applying.done + 1, applying.total);
        const std::string where = applying.total ? "APPLYING MODS  " + std::to_string(shown) + " OF " + std::to_string(applying.total) : std::string("APPLYING MODS");
        draw->AddText(ui.bold, 26 * u, ImVec2(x, fy + 8 * u), sub_ink, where.c_str());
        label(draw, ui.bold, 34 * u, ImVec2(x, fy + 40 * u), bar_x - 20 * u, theme::paper, applying.step.empty() ? std::string("Merging the mods into the game...") : applying.step);
        draw->AddText(ui.body, 26 * u, ImVec2(bar_x, fy + 10 * u), sub_ink,
                      m.mods && m.mods->effect == live_mods::ApplyEffect::reload_level ? "The level reloads when it is done" : "It takes effect at the next level load");
        bar(ImVec2(bar_x, fy + 46 * u), ImVec2(bar_x + bar_w, fy + 64 * u), applying.total ? static_cast<float>(applying.done) / static_cast<float>(applying.total) : -1.f);
        return;
    }
    std::string line = m.notice;
    ImU32 colour = m.notice_bad ? skate_theme::danger : theme::paper;
    // (What is waiting in white beside the blue APPLY, not in amber: the button is the call to act.)
    if (line.empty() && changes) line = std::to_string(changes) + (changes == 1 ? " change is" : " changes are") + " not in the game yet: apply them when you are ready.", colour = theme::paper;
    if (line.empty() && m.mods && m.mods->waiting)
        line = std::to_string(m.mods->waiting) + (m.mods->waiting == 1 ? " change goes" : " changes go") + " in the next time the game starts.", colour = theme::paper;

    if (line.empty() && m.listing) {
        const auto &listing = *m.listing;
        if (listing.fetching && listing.packages.empty()) line = "Getting the mods from Thunderstore...";
        else if (!listing.packages.empty())
            line = std::to_string(listing.packages.size()) + " mods on Thunderstore" + (listing.fetching ? ", checking for new ones..." : "") +
                   (listing.error.empty() || listing.fetching ? std::string() : "  (the last check failed: " + listing.error + ")");
        colour = theme::muted;
    }
    float x = a.x;
    const auto hint = [&](std::uint32_t pad_button, const char *key, const char *what) {
        const auto name = ui.pad.available ? controller_combo_label(pad_button, ui.pad.style) : std::string(key);
        if (!name.empty()) x += theme::prompt(draw, ui.bold, u * 2, ImVec2(x, fy + 20 * u), name.c_str(), what) + 16 * u;
    };
    if (m.tab == browse_tab && m.categories_open) hint(0x1000, "Enter", "Show only / hide / either"), hint(0x2000, "Esc", "Done");
    else if (m.tab == browse_tab && !m.details.empty()) hint(0x2000, "Esc", "Back to mods");
    else if (m.tab == browse_tab) {
        // On a card A is its main action, which the prompt names; elsewhere it presses what is focused.
        if (const auto *picked = m.card_focused ? package_named(m, m.picked) : nullptr) {
            if (const auto does = main_action(m, *picked); does.enabled) hint(0x1000, "Enter", does.prompt);
        } else hint(0x1000, "Enter", "Choose");
        hint(0x8000, "", "Full page");
        if (sort_applies(m)) hint(0x4000, "", "Sort");
    } else if (m.tab == installed_tab) hint(0x1000, "Enter", "Switch on / off");
    else if (m.tab == updates_tab) hint(0x1000, "Enter", "Update");
    else if (!m.plugin_open.empty()) hint(0x1000, "Enter", "Select"), hint(0x2000, "Esc", "Back to plugins");
    else hint(0x1000, "Enter", "Open"), hint(0x4000, "", "On / off");
    if (can_apply) hint(0x0020, "", "Apply");
    label(draw, ui.body, 30 * u, ImVec2(x + 10 * u, fy + 22 * u), right - 20 * u, colour, line);
}
