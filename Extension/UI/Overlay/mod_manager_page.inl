// The pause menu's Mod Manager page (native_menu.cpp's third page), drawn here as the other two are:
// Thunderstore's mods to browse and install, the installed ones to switch on and off, update and
// uninstall, and the Apply that puts the changes in the game. Included into hub_page.cpp, inside its
// namespace, for its controls (item, button, field, Scroller) and its focus. The work behind it is
// Extension/Assets/mod_store.h; nothing here reads the disk or the network.
//
// BROWSE is a front page like a console store's: a few mods big in a carousel, then rows of them
// (most popular, recently updated, new, the biggest categories). Searching or choosing categories
// (several, shown only or hidden, in a panel of their own) shows a grid instead. Pressing a mod
// opens its page: its picture, what it is, its README, and INSTALL. MY MODS is the Mods folder;
// UPDATES what Thunderstore has newer.

namespace ms = dingosdk::mod_store;
namespace ts = dingosdk::thunderstore;

enum ModTab : int { browse_tab, installed_tab, updates_tab };

struct ModPage {
    int tab{browse_tab};
    std::string search, installed_search;
    // The categories shown only and those hidden (Query), kept in the profile; and whether the
    // panel that picks them is open.
    std::vector<std::string> only, hidden;
    bool categories_loaded{}, categories_open{};
    ms::Sort sort{ms::Sort::popular};
    int installed_filter{};             // 0 all, 1 on, 2 off, 3 waiting to be applied
    std::string details;                // the package whose page is open (Namespace-Name); empty: none
    // Where it was opened from, to go back to: the tab, and the control (a tile, mostly) pressed.
    int return_tab{browse_tab};
    std::uint32_t return_focus{};
    // B and Escape stay the page's this long after leaving a mod's page with one, so the press
    // that left it does not then close the pause menu (hold_native_menu_back).
    ULONGLONG hold_back_until{};
    int carousel_turn{};                // left or right on the D-pad with the carousel's card focused (hub_frame)
    std::size_t featured{};             // the carousel's card in the middle
    ULONGLONG featured_at{};            // when it last moved by itself
    std::map<std::uint32_t, float> rows; // each shelf's scroll across
    std::string selected;               // MY MODS: the mod the panel shows (its folder)
    std::string confirm;                // a mod whose UNINSTALL was pressed once
    ULONGLONG confirm_until{};
    // What the page shows, worked out again only when the listing, the mods or the query change.
    std::shared_ptr<const ms::Listing> listing;
    std::shared_ptr<const ms::Mods> mods;
    unsigned listing_serial{~0U}, mods_serial{~0U};
    std::string query_key;
    ms::Front front;
    std::vector<const ts::Package*> results, updates;
    std::vector<std::pair<std::string, int>> categories;
    std::map<std::string, const ts::Package*, std::less<>> by_folder;
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
};
ModPage &mod_page() {
    static ModPage value;
    return value;
}
const std::uint32_t featured_card = hash("mods-featured-card"), categories_button = hash("mods-categories-open");

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
// What the CATEGORIES button says: "Maps, Cosmetics, not Audio", or ALL.
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
// A package's icon, or its name's first letter on a tile while the icon is on its way.
void package_icon(Ui &ui, ImVec2 a, ImVec2 b, const ts::Package *package, std::string_view title, bool dim = false) {
    if (package)
        if (const auto id = store_icon(package->latest().icon)) {
            ui.draw->AddImage(id, a, b, ImVec2(0, 0), ImVec2(1, 1), dim ? IM_COL32(150, 150, 150, 255) : IM_COL32_WHITE);
            return;
        }
    skate_theme::rough_rect(ui.draw, a, b, skate_theme::tile_light, static_cast<unsigned>(hash(title)) & 0xfff, ui.u * 2);
    const float size = (b.y - a.y) * .5f;
    const std::string letter = title.empty() ? std::string("?") : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(title.front()))));
    ui.draw->AddText(ui.heading, size, ImVec2(a.x + (b.x - a.x - width_of(ui.heading, size, letter)) * .5f, a.y + (b.y - a.y - size) * .5f),
                     theme::muted, letter.c_str());
}
std::string action_label(ms::Action action) {
    return action == ms::Action::install ? "INSTALL" : action == ms::Action::update ? "UPDATE" : "INSTALLED";
}
ms::Action action_of(const ModPage &m, const ts::Package &package) {
    return m.mods ? ms::action_for(package, m.mods->versions) : ms::Action::install;
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
void open_details(Ui &ui, ModPage &m, const ts::Package &package) {
    m.return_tab = m.tab;
    m.return_focus = ui.h.focus;
    m.tab = browse_tab;
    m.details = package.full_name;
    m.readme.reset();
    m.readme_lines.clear();
    ui.h.focus_next = hash("mods-detail-action");
}
// Back from a mod's page to where it was opened from, the focus on what opened it.
void close_details(Hub &h, ModPage &m) {
    m.details.clear();
    m.tab = m.return_tab;
    h.focus_next = m.return_focus ? m.return_focus : hash("mods-search");
    m.featured_at = GetTickCount64(); // (the carousel stays where it was left)
}
void close_categories(Hub &h, ModPage &m) {
    m.categories_open = false;
    h.focus_next = categories_button;
    h.scrolls[hash("mods-browse")] = 0;
}
// Whether B and Escape are the page's (a mod's page, or the categories, is open: hub_frame), and
// what they do then.
bool mods_hold_back(const ModPage &m) { return m.tab == browse_tab && (!m.details.empty() || m.categories_open); }
void mods_back(Hub &h, ModPage &m) {
    if (m.categories_open) close_categories(h, m);
    else close_details(h, m);
}

// A package as a tile in a row or the grid: its icon, name and maker, and what it is to this PC.
// Returns whether it was pressed. A tile out of view still takes its place in the focus order, so
// the D-pad can move onto it, but is not drawn: drawing it would ask for its icon, and icons have
// only so many places on the GPU.
bool package_tile(Ui &ui, ModPage &m, std::uint32_t id, ImVec2 p, float w, const ts::Package &package, const Item *clip) {
    const float u = ui.u, h = w + 104 * u;
    const ImVec2 q(p.x + w, p.y + h);
    const Hit hit = item(ui, id, p, q, clip);
    if (clip && (q.x < clip->a.x || p.x > clip->b.x || q.y < clip->a.y || p.y > clip->b.y)) return hit.press;
    const bool lit = hit.focus || hit.hover;
    skate_theme::rough_rect(ui.draw, p, q, lit ? theme::blue : skate_theme::tile, id & 0xfff, u * 2);
    const float inset = 10 * u;
    package_icon(ui, ImVec2(p.x + inset, p.y + inset), ImVec2(q.x - inset, p.y + w - inset), &package, package.title());
    // Being installed, or waiting its turn: a strip over the foot of its icon says so.
    const auto &progress = m.progress;
    const bool queued = progress.busy && std::find(progress.queued.begin(), progress.queued.end(), package.full_name) != progress.queued.end();
    if (progress.busy && (progress.package == package.full_name || queued)) {
        const ImVec2 s0(p.x + inset, p.y + w - inset - 46 * u), s1(q.x - inset, p.y + w - inset);
        ui.draw->AddRectFilled(s0, s1, IM_COL32(0, 0, 0, 200));
        if (!queued && progress.fraction >= 0) ui.draw->AddRectFilled(s0, ImVec2(s0.x + (s1.x - s0.x) * std::clamp(progress.fraction, 0.f, 1.f), s1.y), theme::blue);
        const auto text = queued ? std::string("WAITING") : progress.step == ms::Progress::Step::downloading ? std::string("DOWNLOADING") : std::string("INSTALLING");
        ui.draw->AddText(ui.bold, 26 * u, ImVec2(s0.x + 12 * u, s0.y + 10 * u), theme::paper, text.c_str());
    }
    const auto action = action_of(m, package);
    if (action != ms::Action::install)
        plate(ui, ImVec2(p.x + inset + 8 * u, p.y + inset + 8 * u), 40 * u, action == ms::Action::update ? "UPDATE" : "INSTALLED",
              action == ms::Action::update ? skate_theme::warning : skate_theme::good, skate_theme::black);
    label(ui.draw, ui.bold, 32 * u, ImVec2(p.x + 16 * u, p.y + w + 4 * u), q.x - 12 * u, theme::paper, package.title());
    label(ui.draw, ui.body, 26 * u, ImVec2(p.x + 16 * u, p.y + w + 50 * u), q.x - 12 * u, lit ? theme::paper : theme::muted,
          package.owner + "  -  " + ms::count_text(package.downloads));
    return hit.press;
}

// A round arrow button, for the carousel and the rows.
bool arrow(Ui &ui, std::uint32_t id, ImVec2 centre, float radius, int direction, const Item *clip = nullptr) {
    const Hit hit = item(ui, id, ImVec2(centre.x - radius, centre.y - radius), ImVec2(centre.x + radius, centre.y + radius), clip);
    const bool lit = hit.focus || hit.hover;
    ui.draw->AddCircleFilled(centre, radius, lit ? theme::blue : IM_COL32(245, 245, 245, 235), 32);
    const float s = radius * .38f, d = static_cast<float>(direction);
    ui.draw->AddTriangleFilled(ImVec2(centre.x + s * d, centre.y), ImVec2(centre.x - s * .7f * d, centre.y - s),
                               ImVec2(centre.x - s * .7f * d, centre.y + s), lit ? theme::paper : skate_theme::black);
    return hit.press;
}

// ---------------------------------------------------------------- what the page shows

void refresh_view(ModPage &m) {
    m.listing = ms::listing();
    m.mods = ms::installed();
    const auto mods_serial = m.mods ? m.mods->serial : 0U;
    load_categories(m);
    std::string key = m.search + '\n' + std::to_string(static_cast<int>(m.sort));
    for (const auto &name : m.only) key += "\n+" + name;
    for (const auto &name : m.hidden) key += "\n-" + name;
    if (m.listing->serial == m.listing_serial && mods_serial == m.mods_serial && key == m.query_key) return;
    const bool listing_changed = m.listing->serial != m.listing_serial;
    m.listing_serial = m.listing->serial;
    m.mods_serial = mods_serial;
    m.query_key = key;
    const auto &packages = m.listing->packages;
    const ts::Installed none;
    const auto &have = m.mods ? m.mods->versions : none;
    m.results = ms::search(packages, {m.search, m.only, m.hidden, m.sort}, have);
    if (listing_changed || m.front.shelves.empty()) {
        m.front = ms::front(packages, have, ms::today());
        m.categories = ms::categories(packages);
        m.by_folder.clear();
        for (const auto &package : packages) m.by_folder[ts::folder_for(package.full_name)] = &package;
    }
    m.updates.clear();
    for (const auto &package : packages)
        if (ts::update_available(package, have)) m.updates.push_back(&package);
}
const ts::Package *package_named(const ModPage &m, std::string_view full_name) {
    return m.listing ? ts::find_package(m.listing->packages, full_name) : nullptr;
}

// ---------------------------------------------------------------- BROWSE

// The bar over BROWSE: search, the categories (a panel of their own), the order.
void browse_bar(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z, float bar) {
    const float u = ui.u, gap = 28 * u;
    const float search_width = std::min(900 * u, (z.x - a.x) * .28f);
    field(ui, hash("mods-search"), a, ImVec2(a.x + search_width, a.y + bar), m.search, "Search mods", false, 64);
    const float sort_width = 640 * u;
    const ImVec2 c(a.x + search_width + gap, a.y), d(z.x - sort_width - gap, a.y + bar);
    if (button(ui, categories_button, c, d, "CATEGORIES", m.categories_open ? Look::chosen : Look::tile, true, category_summary(m))) {
        if (m.categories_open) close_categories(ui.h, m);
        else {
            m.categories_open = true;
            ui.h.focus_next = m.categories.empty() ? hash("mods-categories-done") : hash(m.categories.front().first, 71);
        }
    }
    if (button(ui, hash("mods-sort"), ImVec2(z.x - sort_width, a.y), ImVec2(z.x, a.y + bar), "SORT", Look::tile, true, ms::sort_label(m.sort)))
        m.sort = static_cast<ms::Sort>((static_cast<std::size_t>(m.sort) + 1) % ms::sort_count);
}

// The categories, all of them, as tiles: pressing one shows only it (with any others shown only),
// again hides it, again leaves it either way, as the launcher's category menu does. Made for the
// D-pad and A as much as the mouse.
void categories_panel(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, inset = 40 * u;
    skate_theme::rough_rect(draw, a, z, skate_theme::tile, 0x3c1, u * 2);
    draw->AddText(ui.heading, 56 * u, ImVec2(a.x + inset, a.y + inset), theme::paper, "CATEGORIES");
    const float bh = 92 * u, bw = 400 * u;
    const ImVec2 done(z.x - inset - bw, a.y + inset), clear(done.x - 20 * u - bw, a.y + inset);
    label(draw, ui.body, 30 * u, ImVec2(a.x + inset, a.y + inset + 78 * u), clear.x - 30 * u, theme::muted,
          "Press a category to show only it, again to hide it, and again for either. Mods in any category shown are listed.");
    if (button(ui, hash("mods-categories-clear"), clear, ImVec2(clear.x + bw, clear.y + bh), "CLEAR", Look::plain, !m.only.empty() || !m.hidden.empty())) {
        m.only.clear();
        m.hidden.clear();
        save_categories(m);
    }
    if (button(ui, hash("mods-categories-done"), done, ImVec2(done.x + bw, done.y + bh), "DONE", Look::sticky)) close_categories(ui.h, m);
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
            const bool lit = hit.focus || hit.hover;
            skate_theme::rough_rect(draw, p, q, lit ? theme::blue : state == 1 ? skate_theme::friends_tile : state == 2 ? IM_COL32(70, 24, 24, 240) : skate_theme::tile_grey,
                                    id & 0xfff, u * 2);
            // Its state on the left, as a mark: a tick shown only, a cross hidden, nothing either way.
            const ImVec2 mark(p.x + 30 * u, p.y + tile_h * .5f);
            if (state == 1) {
                draw->AddLine(ImVec2(mark.x - 14 * u, mark.y), ImVec2(mark.x - 3 * u, mark.y + 12 * u), skate_theme::good, 7 * u);
                draw->AddLine(ImVec2(mark.x - 3 * u, mark.y + 12 * u), ImVec2(mark.x + 18 * u, mark.y - 14 * u), skate_theme::good, 7 * u);
            } else if (state == 2) {
                draw->AddLine(ImVec2(mark.x - 14 * u, mark.y - 14 * u), ImVec2(mark.x + 14 * u, mark.y + 14 * u), skate_theme::danger, 7 * u);
                draw->AddLine(ImVec2(mark.x - 14 * u, mark.y + 14 * u), ImVec2(mark.x + 14 * u, mark.y - 14 * u), skate_theme::danger, 7 * u);
            } else draw->AddCircle(mark, 14 * u, theme::muted, 20, 3 * u);
            std::string upper = name;
            for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            const auto counted = std::to_string(count);
            const float count_w = width_of(ui.body, 30 * u, counted);
            label(draw, ui.bold, 36 * u, ImVec2(p.x + 64 * u, p.y + (tile_h - 36 * u) * .5f), q.x - count_w - 40 * u, state == 2 && !lit ? theme::muted : theme::paper, upper);
            draw->AddText(ui.body, 30 * u, ImVec2(q.x - count_w - 24 * u, p.y + (tile_h - 30 * u) * .5f), lit ? theme::paper : theme::muted, counted.c_str());
        }
    }
    list.end();
}

// Pointed at and clicked with the mouse only: not a stop for the D-pad. The carousel's extras (its
// neighbours, arrows and the card's own buttons) are these, so the D-pad has one place to land on
// it, the card, and turns it with left and right (hub_frame).
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
    ImU32 fill = skate_theme::tile, ink = enabled ? theme::paper : theme::muted;
    if (look == Look::sticky) fill = !enabled ? sticky_off : hit.hover ? sticky_hover : sticky, ink = skate_theme::black;
    else if (hit.hover) fill = theme::blue;
    else if (look == Look::plain) fill = skate_theme::tile_light;
    skate_theme::rough_rect(ui.draw, a, b, fill, hash(text) & 0xfff, u * 2);
    ui.draw->AddText(ui.bold, size, ImVec2(a.x + (b.x - a.x - width_of(ui.bold, size, text)) * .5f, a.y + (b.y - a.y - size) * .5f), ink, text.c_str());
    return hit.press;
}
bool pointer_arrow(Ui &ui, ImVec2 centre, float radius, int direction, const Item *clip) {
    const auto hit = pointed(ui, ImVec2(centre.x - radius, centre.y - radius), ImVec2(centre.x + radius, centre.y + radius), clip);
    ui.draw->AddCircleFilled(centre, radius, hit.hover ? theme::blue : IM_COL32(245, 245, 245, 235), 32);
    const float s = radius * .38f, d = static_cast<float>(direction);
    ui.draw->AddTriangleFilled(ImVec2(centre.x + s * d, centre.y), ImVec2(centre.x - s * .7f * d, centre.y - s),
                               ImVec2(centre.x - s * .7f * d, centre.y + s), hit.hover ? theme::paper : skate_theme::black);
    return hit.press;
}

// The carousel: the card in the middle big, its neighbours to each side, arrows over them. On a
// controller the card is the one stop: A opens the mod, X installs it, left and right turn it.
float featured(Ui &ui, ModPage &m, ImVec2 a, float right, float top, const Item &clip) {
    auto *draw = ui.draw;
    const float u = ui.u, width = right - a.x;
    const auto &list = m.front.featured;
    if (list.empty()) return 0;
    const float card_w = std::min(1900 * u, width * .58f), card_h = 520 * u;
    const float side_w = 1150 * u, side_h = 420 * u, spacing = 48 * u;
    const float centre = a.x + width * .5f;
    const auto count = list.size();
    const auto now = GetTickCount64();
    const auto index = [&](int offset) { return (m.featured + count * 4 + static_cast<std::size_t>(offset + static_cast<int>(count) * 2)) % count; };
    // Turned by the D-pad (hub_frame), or by the mouse below.
    const auto turn = [&](int by) {
        m.featured = index(by);
        m.featured_at = now;
    };
    m.featured %= count;
    if (const int by = std::exchange(m.carousel_turn, 0)) turn(std::clamp(by, -1, 1));
    bool held{};
    // The neighbours, drawn first and dimmed: clicking one brings it to the middle.
    for (const int side : {-1, 1}) {
        if (count < 2) break;
        const auto *package = list[index(side)];
        const float x = side < 0 ? centre - card_w * .5f - spacing - side_w : centre + card_w * .5f + spacing;
        const ImVec2 p(x, top + (card_h - side_h) * .5f), q(x + side_w, p.y + side_h);
        const auto hit = pointed(ui, p, q, &clip);
        held |= hit.hover;
        skate_theme::rough_rect(draw, p, q, hit.hover ? skate_theme::tile_light : IM_COL32(20, 20, 20, 220), hash(package->full_name) & 0xfff, u * 2);
        const float icon = side_h - 60 * u;
        package_icon(ui, ImVec2(p.x + 30 * u, p.y + 30 * u), ImVec2(p.x + 30 * u + icon, p.y + 30 * u + icon), package, package->title(), true);
        label(draw, ui.heading, 48 * u, ImVec2(p.x + icon + 70 * u, p.y + 40 * u), q.x - 30 * u, theme::muted, package->title());
        label(draw, ui.body, 30 * u, ImVec2(p.x + icon + 70 * u, p.y + 104 * u), q.x - 30 * u, theme::muted, "by " + package->owner);
        if (hit.press) turn(side);
    }
    // The card in the middle: the mod's picture big, as a sticker on a backdrop of itself (darkened,
    // and darker still where the text is), what it is beside it, and its buttons under that.
    const auto *package = list[index(0)];
    const ImVec2 p(centre - card_w * .5f, top), q(centre + card_w * .5f, top + card_h);
    const float inset = 36 * u;
    const auto action = action_of(m, *package);
    // To the D-pad the card is as wide as the page, so up from any tile of the row under it lands
    // here, not on the bar above it; to the mouse it is the card itself.
    const Hit card = item(ui, featured_card, ImVec2(clip.a.x, p.y), ImVec2(clip.b.x, q.y), &clip);
    const bool ringed = card.focus && ui.h.ring;
    held |= card.focus;
    skate_theme::rough_rect(draw, p, q, skate_theme::tile, hash(package->full_name) & 0xfff, u * 2);
    const ImVec2 bp(p.x + 6 * u, p.y + 6 * u), bq(q.x - 6 * u, q.y - 6 * u);
    if (const auto backdrop = store_icon(package->latest().icon)) {
        // A band across the middle of the square picture, as wide as the card.
        const float band = (bq.y - bp.y) / (bq.x - bp.x), from = .5f - band * .5f;
        draw->AddImage(backdrop, bp, bq, ImVec2(0, from), ImVec2(1, from + band), IM_COL32(150, 150, 150, 255));
    }
    draw->AddRectFilledMultiColor(bp, bq, IM_COL32(16, 16, 18, 250), IM_COL32(16, 16, 18, 175), IM_COL32(16, 16, 18, 175), IM_COL32(16, 16, 18, 250));
    draw->AddRect(p, q, ringed ? theme::blue : IM_COL32(245, 245, 245, 60), 0, 0, (ringed ? 8.f : 3.f) * u);

    // The picture, tipped a little, in a white border with a shadow under it.
    const float picture = card_h - inset * 2 - 16 * u;
    const ImVec2 middle(p.x + inset + 8 * u + picture * .5f, p.y + card_h * .5f);
    const auto corners = [&](float half, ImVec2 shift, std::array<ImVec2, 4> &out) {
        constexpr float turn = -0.035f; // about 2 degrees
        const float c = std::cos(turn), s = std::sin(turn);
        const std::array<ImVec2, 4> square{ImVec2(-half, -half), ImVec2(half, -half), ImVec2(half, half), ImVec2(-half, half)};
        for (std::size_t i = 0; i < 4; ++i)
            out[i] = ImVec2(middle.x + shift.x + square[i].x * c - square[i].y * s, middle.y + shift.y + square[i].x * s + square[i].y * c);
    };
    std::array<ImVec2, 4> shadow{}, border{}, image{};
    corners(picture * .5f + 10 * u, ImVec2(12 * u, 14 * u), shadow);
    corners(picture * .5f + 10 * u, ImVec2(0, 0), border);
    corners(picture * .5f, ImVec2(0, 0), image);
    draw->AddQuadFilled(shadow[0], shadow[1], shadow[2], shadow[3], IM_COL32(0, 0, 0, 130));
    draw->AddQuadFilled(border[0], border[1], border[2], border[3], theme::paper);
    if (const auto texture = store_icon(package->latest().icon))
        draw->AddImageQuad(texture, image[0], image[1], image[2], image[3]);
    else
        draw->AddQuadFilled(image[0], image[1], image[2], image[3], skate_theme::tile_light);

    // What it is.
    const float tx = middle.x + picture * .5f + 64 * u, edge = q.x - inset;
    float y = p.y + inset;
    float x = tx;
    x += plate(ui, ImVec2(x, y), 42 * u, package->pinned ? "PINNED" : "FEATURED", theme::blue, theme::paper) + 12 * u;
    if (action == ms::Action::installed) plate(ui, ImVec2(x, y), 42 * u, "INSTALLED", skate_theme::good, skate_theme::black);
    else if (action == ms::Action::update) plate(ui, ImVec2(x, y), 42 * u, "UPDATE AVAILABLE", skate_theme::warning, skate_theme::black);
    y += 58 * u;
    label(draw, ui.heading, 76 * u, ImVec2(tx, y), edge, theme::paper, package->title());
    y += 92 * u;
    x = tx;
    const auto fact = [&](const std::string &text, ImU32 fill, ImU32 ink) {
        const float w = width_of(ui.bold, 38 * u * .62f, text) + 24 * u;
        if (x + w > edge) return;
        x += plate(ui, ImVec2(x, y), 38 * u, text, fill, ink) + 10 * u;
    };
    fact("BY " + package->owner, IM_COL32(245, 245, 245, 235), skate_theme::black);
    fact(ms::count_text(package->downloads) + " DOWNLOADS", skate_theme::tile_light, theme::paper);
    fact("V" + package->latest().number, skate_theme::tile_light, theme::paper);
    if (package->latest().file_size) fact(ms::size_text(package->latest().file_size), skate_theme::tile_light, theme::paper);
    for (std::size_t i = 0; i < package->categories.size() && i < 3; ++i) {
        std::string upper = package->categories[i];
        for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        fact(upper, IM_COL32(1, 131, 255, 70), theme::paper);
    }
    y += 54 * u;
    label(draw, ui.body, 28 * u, ImVec2(tx, y), edge, theme::muted, "Updated " + ms::date_text(package->date_updated));
    y += 48 * u;

    // Its buttons, at the foot of the text, and as much of its description as fits above them.
    const float bh = 84 * u, bw = 340 * u, between = 20 * u;
    const ImVec2 b1(tx, q.y - inset - bh), b2(tx + bw + between, q.y - inset - bh);
    const float line = 30 * u * 1.3f;
    const auto lines = static_cast<std::size_t>(std::clamp((b1.y - 22 * u - y) / line, 0.f, 4.f));
    if (lines) paragraph(draw, ui.body, 30 * u, ImVec2(tx, y), edge - tx, soft, package->latest().description, lines);
    // Its buttons are the mouse's; the D-pad has the card, with its ring and its prompts beside them.
    const bool on_button = ui.input && (inside(ui.mouse, b1, ImVec2(b1.x + bw, b1.y + bh)) || inside(ui.mouse, b2, ImVec2(b2.x + bw, b2.y + bh)));
    const auto does = queue_button(m, *package, action, action_label(action));
    const bool can_install = does.enabled;
    bool install = pointer_button(ui, b1, ImVec2(b1.x + bw, b1.y + bh), does.text, can_install ? Look::sticky : Look::plain, can_install, &clip);
    bool details = pointer_button(ui, b2, ImVec2(b2.x + bw, b2.y + bh), "DETAILS", Look::plain, true, &clip);
    if (ringed && ui.pad.available) {
        float px = b2.x + bw + 36 * u;
        const auto prompt = [&](std::uint32_t pad_button, const char *what) {
            if (px > edge - 120 * u) return;
            px += theme::keycap(draw, ui.bold, u * 2, ImVec2(px, b1.y + (bh - 40 * u) * .5f), controller_combo_label(pad_button, ui.pad.style).c_str(), what) + 30 * u;
        };
        if (can_install) prompt(0x4000, action == ms::Action::update ? "Update" : "Install");
        prompt(0x1000, "Details");
    }
    const std::uint32_t pressed = ui.input && !ui.h.typing ? ui.pad.buttons & ~ui.h.pad_before : 0;
    install |= card.focus && can_install && (pressed & 0x4000);
    details |= card.press && !on_button && (!ui.clicked || inside(ui.mouse, p, q));
    if (install) start_install(m, {package->full_name});
    else if (details) open_details(ui, m, *package);
    // The arrows, over the neighbours.
    if (count > 1) {
        if (pointer_arrow(ui, ImVec2(p.x - spacing * .5f - 10 * u, top + card_h * .5f), 46 * u, -1, &clip)) turn(-1);
        if (pointer_arrow(ui, ImVec2(q.x + spacing * .5f + 10 * u, top + card_h * .5f), 46 * u, 1, &clip)) turn(1);
    }
    // The dots under it: which card of how many.
    const float dot = 9 * u, dots_w = static_cast<float>(count) * dot * 3;
    for (std::size_t i = 0; i < count; ++i)
        draw->AddCircleFilled(ImVec2(centre - dots_w * .5f + (static_cast<float>(i) + .5f) * dot * 3, top + card_h + 30 * u), dot,
                              i == m.featured ? theme::blue : skate_theme::tile_light, 16);
    // Moves on by itself every 8 s, unless the player is on it.
    held |= ui.input && inside(ui.mouse, p, q);
    if (held || !m.featured_at) m.featured_at = now;
    else if (now - m.featured_at > 8000) turn(1);
    return card_h + 60 * u;
}

// One row of tiles that scrolls across, with arrows at its ends. Returns how tall it was.
float shelf(Ui &ui, ModPage &m, const ms::Shelf &row, Scroller &list, ImVec2 a, float right) {
    auto *draw = ui.draw;
    const float u = ui.u, tile_w = 270 * u, tile_h = tile_w + 104 * u, gap = 24 * u, title_h = 66 * u;
    bool drawn{};
    const float top = list.next(title_h + tile_h, 40 * u, drawn);
    const auto id = hash(row.title, 31);
    float &offset = m.rows[id];
    const float row_left = a.x + 70 * u, row_right = right - 70 * u, view = row_right - row_left;
    const float content = static_cast<float>(row.packages.size()) * (tile_w + gap) - gap;
    offset = std::clamp(offset, 0.f, std::max(0.f, content - view));
    draw->AddText(ui.heading, 46 * u, ImVec2(a.x, top), theme::paper, row.title.c_str());
    // A row well out of view takes no part at all; one just out of view only its place in the focus
    // order, so the D-pad can move down onto it.
    if (top + title_h + tile_h < list.box.a.y - tile_h || top > list.box.b.y + tile_h) return title_h + tile_h;
    // The row's own box, inside the page's: where its tiles can be pointed at. (It is the page's list
    // to the D-pad, so up and down go from row to row: move_focus.)
    const Item box{list.box.id, ImVec2(row_left, std::max(top + title_h, list.box.a.y)), ImVec2(row_right, std::min(top + title_h + tile_h, list.box.b.y))};
    draw->PushClipRect(ImVec2(row_left - 4 * u, list.box.a.y), ImVec2(row_right + 4 * u, list.box.b.y), true);
    for (std::size_t i = 0; i < row.packages.size(); ++i) {
        const float x = row_left - offset + static_cast<float>(i) * (tile_w + gap);
        // The tiles in view, and one to each side so the D-pad can move onto it.
        if (x + tile_w < row_left - tile_w - gap || x > row_right + tile_w + gap) continue;
        const auto tile_id = hash(row.packages[i]->full_name, id);
        if (package_tile(ui, m, tile_id, ImVec2(x, top + title_h), tile_w, *row.packages[i], &box)) open_details(ui, m, *row.packages[i]);
        // Moving onto a tile out of view brings it in.
        if (ui.h.focus == tile_id && ui.h.focus_moved) {
            if (x < row_left) offset -= row_left - x;
            else if (x + tile_w > row_right) offset += x + tile_w - row_right;
        }
        list.reveal(tile_id, top, title_h + tile_h);
    }
    draw->PopClipRect();
    if (offset > 0 && arrow(ui, hash("mods-row-left", id), ImVec2(a.x + 30 * u, top + title_h + tile_w * .5f), 32 * u, -1, &list.box))
        offset = std::max(0.f, offset - view * .8f);
    if (offset < content - view - 1 && arrow(ui, hash("mods-row-right", id), ImVec2(right - 30 * u, top + title_h + tile_w * .5f), 32 * u, 1, &list.box))
        offset = std::min(content - view, offset + view * .8f);
    return title_h + tile_h;
}

void package_details(Ui &ui, ModPage &m, const ts::Package &package, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, inset = 40 * u;
    const auto &version = package.latest();
    const auto action = action_of(m, package);
    const auto folder = ts::folder_for(package.full_name);
    const ms::Mod *mine{};
    if (m.mods)
        for (const auto &mod : m.mods->mods)
            if (_stricmp(mod.folder.c_str(), folder.c_str()) == 0) mine = &mod;
    // ---- the left column: the picture and what to do, fitted to the window. The picture is as big
    // as the buttons under it leave room for; in a window too short for that, the buttons go two to a row.
    const float left_w = std::min(760 * u, (z.x - a.x) * .3f);
    const ImVec2 la(a.x, a.y), lz(a.x + left_w, z.y);
    skate_theme::rough_rect(draw, la, lz, skate_theme::tile, 0x3a1, u * 2);
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
        if (button(ui, hash("mods-detail-action"), p, q, does.text, does.enabled && action != ms::Action::installed ? Look::sticky : Look::plain, does.enabled))
            start_install(m, {package.full_name});
    }
    if (changes) {
        const auto [p, q] = place();
        if (button(ui, hash("mods-detail-apply"), p, q, "APPLY CHANGES", Look::sticky, !m.progress.busy)) mod_say(m, ms::apply());
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
    skate_theme::rough_rect(draw, ra, rz, skate_theme::tile, 0x3a2, u * 2);
    const float left = ra.x + inset, edge = rz.x - inset;
    y = ra.y + inset;
    label(draw, ui.heading, 72 * u, ImVec2(left, y), edge, theme::paper, package.title());
    y += 92 * u;
    std::string facts = "by " + package.owner + "   v" + version.number + "   " + ms::count_text(package.downloads) + " downloads";
    if (version.file_size) facts += "   " + ms::size_text(version.file_size);
    facts += "   updated " + ms::date_text(package.date_updated);
    if (package.rating) facts += "   " + std::to_string(package.rating) + " likes";
    label(draw, ui.body, 32 * u, ImVec2(left, y), edge, theme::muted, facts);
    y += 54 * u;
    float x = left;
    if (mine) {
        const bool good = mine->state == ms::State::loaded;
        x += plate(ui, ImVec2(x, y), 42 * u, ms::state_label(mine->state), good ? skate_theme::good : skate_theme::warning, skate_theme::black) + 12 * u;
        if (mine->version != version.number && !mine->version.empty())
            x += plate(ui, ImVec2(x, y), 42 * u, "YOU HAVE v" + mine->version, skate_theme::tile_light, theme::paper) + 12 * u;
    }
    for (std::size_t i = 0; i < package.categories.size() && i < 5; ++i) {
        std::string upper = package.categories[i];
        for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        x += plate(ui, ImVec2(x, y), 42 * u, upper, skate_theme::tile_light, theme::paper) + 12 * u;
    }
    y += 66 * u;
    if (package.deprecated) {
        label(draw, ui.bold, 30 * u, ImVec2(left, y), edge, skate_theme::warning, "Its author marked it deprecated: it is no longer looked after.");
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
            y += paragraph(draw, ui.body, 30 * u, ImVec2(left, y), edge - left, theme::muted, "Brings what it needs with it: " + needs, 2) + 14 * u;
        }
    }
    y += 6 * u;
    draw->AddRectFilled(ImVec2(left, y), ImVec2(edge, y + 3 * u), skate_theme::tile_light);
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
            if (hit.focus && ui.h.ring) draw->AddRectFilled(ImVec2(ba.x, top), ImVec2(ba.x + 6 * u, top + height), theme::blue);
        }
        if (!shown) continue;
        const float text_top = top + (line.kind == Kind::heading && line.first ? 16 * u : 0);
        switch (line.kind) {
        case Kind::heading: draw->AddText(ui.bold, size, ImVec2(ba.x + 16 * u, text_top), theme::paper, line.text.c_str()); break;
        case Kind::bullet:
            if (line.first) draw->AddCircleFilled(ImVec2(ba.x + 28 * u, text_top + size * .55f), 6 * u, theme::blue, 12);
            draw->AddText(ui.body, size, ImVec2(ba.x + 56 * u, text_top), soft, line.text.c_str());
            break;
        case Kind::code:
            draw->AddRectFilled(ImVec2(ba.x + 16 * u, top), ImVec2(bz.x - 24 * u, top + height), IM_COL32(10, 10, 10, 200));
            draw->AddText(ui.body, size, ImVec2(ba.x + 44 * u, text_top), IM_COL32(190, 220, 255, 255), line.text.c_str());
            break;
        case Kind::rule: draw->AddRectFilled(ImVec2(ba.x + 16 * u, top + 13 * u), ImVec2(bz.x - 24 * u, top + 16 * u), skate_theme::tile_light); break;
        case Kind::gap: break;
        default: draw->AddText(ui.body, size, ImVec2(ba.x + 16 * u, text_top), soft, line.text.c_str()); break;
        }
    }
    list.end();
}

void browse_page(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, gap = 28 * u, bar = 96 * u;
    if (!m.details.empty()) {
        if (const auto *package = package_named(m, m.details)) return package_details(ui, m, *package, a, z);
        m.details.clear();
    }
    browse_bar(ui, m, a, z, bar);
    const ImVec2 ca(a.x, a.y + bar + gap), cz(z.x, z.y);
    if (m.categories_open) return categories_panel(ui, m, ca, cz);
    const auto &listing = *m.listing;
    if (listing.packages.empty()) {
        const auto line = !listing.error.empty() && !listing.fetching ? "Thunderstore could not be reached: " + listing.error
                          : std::string("Getting the mods from Thunderstore...");
        draw->AddText(ui.body, 40 * u, ImVec2(ca.x, ca.y + 30 * u), theme::muted, line.c_str());
        if (!listing.fetching && button(ui, hash("mods-retry"), ImVec2(ca.x, ca.y + 110 * u), ImVec2(ca.x + 500 * u, ca.y + 206 * u), "TRY AGAIN", Look::sticky))
            ms::refresh();
        return;
    }
    Scroller list(ui, hash("mods-browse"), ca, cz);
    const bool front = m.search.empty() && m.only.empty() && m.hidden.empty();
    if (front) {
        bool shown{};
        const float top = list.next(560 * u + 60 * u, 30 * u, shown);
        const Item clip{hash("mods-browse"), ca, cz};
        featured(ui, m, ImVec2(ca.x, ca.y), cz.x, top, clip);
        list.reveal(featured_card, top, 580 * u);
        for (const auto &row : m.front.shelves) shelf(ui, m, row, list, ca, cz.x - 12 * u);
    } else {
        // Searching, or categories chosen: every match in a grid.
        bool shown{};
        const float head = list.next(56 * u, 10 * u, shown);
        const auto count = std::to_string(m.results.size()) + (m.results.size() == 1 ? " mod" : " mods") +
                           (m.only.empty() && m.hidden.empty() ? std::string() : " in " + category_summary(m)) +
                           (m.search.empty() ? std::string() : " matching \"" + m.search + "\"");
        if (shown) draw->AddText(ui.body, 34 * u, ImVec2(ca.x, head), theme::muted, count.c_str());
        const float tile_w = 270 * u, tile_h = tile_w + 104 * u, spacing = 24 * u;
        const auto columns = std::max<std::size_t>(1, static_cast<std::size_t>((cz.x - ca.x - 20 * u + spacing) / (tile_w + spacing)));
        for (std::size_t i = 0; i < m.results.size(); i += columns) {
            const float top = list.next(tile_h, spacing, shown);
            for (std::size_t j = i; j < std::min(m.results.size(), i + columns); ++j) {
                const auto id = hash(m.results[j]->full_name, 41);
                // (Rows out of view still take their place in the focus order, near it, for the D-pad.)
                if (!shown && (top + tile_h < ca.y - tile_h * 2 || top > cz.y + tile_h * 2)) continue;
                if (package_tile(ui, m, id, ImVec2(ca.x + static_cast<float>(j - i) * (tile_w + spacing), top), tile_w, *m.results[j], &list.box))
                    open_details(ui, m, *m.results[j]);
                list.reveal(id, top, tile_h);
            }
        }
        if (m.results.empty()) {
            const float top = list.next(60 * u, 0, shown);
            draw->AddText(ui.body, 38 * u, ImVec2(ca.x, top), theme::muted, "Nothing matches. Try other words, or ALL.");
        }
    }
    list.end();
}

// ---------------------------------------------------------------- MY MODS

ImU32 state_colour(ms::State state) {
    switch (state) {
    case ms::State::loaded: return skate_theme::good;
    case ms::State::disabled: return skate_theme::tile_light;
    case ms::State::not_loaded: return skate_theme::danger;
    default: return skate_theme::warning;
    }
}

void installed_page(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, gap = 28 * u, bar = 96 * u, list_width = (z.x - a.x) * .62f;
    if (!m.mods) {
        draw->AddText(ui.body, 40 * u, ImVec2(a.x, a.y + 30 * u), theme::muted, "Reading the Mods folder...");
        return;
    }
    const auto &mods = *m.mods;
    // ---- the bar: search and which to show
    {
        const float search_width = std::min(900 * u, list_width * .45f);
        field(ui, hash("mods-installed-search"), a, ImVec2(a.x + search_width, a.y + bar), m.installed_search, "Search your mods", false, 64);
        float x = a.x + search_width + gap;
        const auto chip = [&](const char *text, int filter) {
            const float size = 34 * u, w = width_of(ui.bold, size, text) + 44 * u;
            const ImVec2 c(x, a.y + 10 * u), d(x + w, a.y + bar - 10 * u);
            const Hit hit = item(ui, hash(text, 23), c, d);
            const bool lit = hit.focus || hit.hover, on = m.installed_filter == filter;
            skate_theme::rough_rect(draw, c, d, lit ? theme::blue : on ? theme::paper : skate_theme::tile, hash(text) & 0xfff, u * 2);
            draw->AddText(ui.bold, size, ImVec2(c.x + 22 * u, c.y + (d.y - c.y - size) * .5f), on && !lit ? skate_theme::black : theme::paper, text);
            x = d.x + 12 * u;
            if (hit.press) m.installed_filter = filter, ui.h.scrolls[hash("mods-installed")] = 0;
        };
        chip("ALL", 0);
        chip("ON", 1);
        chip("OFF", 2);
        chip("NOT APPLIED", 3);
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
    const float row_height = 112 * u, row_gap = 8 * u, pad = 20 * u;
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
            const bool lit = hit.focus || hit.hover, chosen = m.selected == mod->folder;
            skate_theme::rough_rect(draw, p, q, lit ? theme::blue : skate_theme::tile, 0xa00 + (id & 0xff), u * 2);
            if (chosen && !lit) draw->AddRectFilled(ImVec2(p.x, p.y + 6 * u), ImVec2(p.x + 10 * u, q.y - 6 * u), theme::blue);
            const float icon = row_height - 20 * u;
            package_icon(ui, ImVec2(p.x + pad, p.y + 10 * u), ImVec2(p.x + pad + icon, p.y + 10 * u + icon), package_of(mod->folder), mod->title, !mod->enabled);
            const float tx = p.x + pad * 2 + icon;
            const auto tag = ms::state_label(mod->state);
            const float tag_w = width_of(ui.bold, 26 * u, tag) + 24 * u;
            const float tag_x = q.x - pad - tag_w;
            plate(ui, ImVec2(tag_x, p.y + (row_height - 44 * u) * .5f), 44 * u, tag, lit ? IM_COL32(0, 0, 0, 90) : state_colour(mod->state),
                  lit || mod->state == ms::State::disabled ? theme::paper : skate_theme::black);
            // The switch: on or off at a glance.
            const float sw = 84 * u, sx = tag_x - sw - 24 * u, sy = p.y + (row_height - 44 * u) * .5f;
            if (!mod->gone) {
                draw->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + 44 * u), mod->enabled ? skate_theme::good : skate_theme::tile_light, 22 * u);
                draw->AddCircleFilled(ImVec2(mod->enabled ? sx + sw - 22 * u : sx + 22 * u, sy + 22 * u), 17 * u, theme::paper, 20);
            }
            label(draw, ui.bold, 38 * u, ImVec2(tx, p.y + 14 * u), sx - 20 * u, mod->enabled || lit ? theme::paper : theme::muted, mod->title);
            std::string byline = mod->author.empty() ? std::string("Unknown author") : "by " + mod->author;
            if (!mod->version.empty()) byline += "   v" + mod->version;
            for (std::size_t i = 0; i < mod->contents.size() && i < 3; ++i) byline += (i ? ", " : "   ") + mod->contents[i];
            label(draw, ui.body, 28 * u, ImVec2(tx, p.y + 64 * u), sx - 20 * u, lit ? theme::paper : theme::muted, byline);
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
    skate_theme::rough_rect(draw, da, dz, skate_theme::tile, 0x7a, u * 2);
    const float inset = 40 * u, left = da.x + inset, edge = dz.x - inset;
    const ms::Mod *mod{};
    for (const auto *row : rows)
        if (row->folder == m.selected) mod = row;
    if (!mod) {
        draw->AddText(ui.body, 36 * u, ImVec2(left, da.y + inset), theme::muted, "Pick a mod to see it here.");
        return;
    }
    const auto *package = package_of(mod->folder);
    float y = da.y + inset;
    const float icon = 220 * u;
    package_icon(ui, ImVec2(left, y), ImVec2(left + icon, y + icon), package, mod->title, !mod->enabled);
    label(draw, ui.heading, 52 * u, ImVec2(left + icon + 30 * u, y + 4 * u), edge, theme::paper, mod->title);
    label(draw, ui.body, 30 * u, ImVec2(left + icon + 30 * u, y + 76 * u), edge, theme::muted,
          (mod->author.empty() ? std::string("Unknown author") : "by " + mod->author) + (mod->version.empty() ? std::string() : "   v" + mod->version));
    plate(ui, ImVec2(left + icon + 30 * u, y + 130 * u), 44 * u, ms::state_label(mod->state), state_colour(mod->state),
          mod->state == ms::State::disabled ? theme::paper : skate_theme::black);
    y += icon + 30 * u;
    if (!mod->why.empty()) y += paragraph(draw, ui.body, 30 * u, ImVec2(left, y), edge - left, skate_theme::warning, mod->why, 3) + 16 * u;
    if (!mod->description.empty()) y += paragraph(draw, ui.body, 30 * u, ImVec2(left, y), edge - left, soft, mod->description, 4) + 16 * u;
    const float bh = 88 * u;
    float by = dz.y - inset - bh;
    const auto progress = ms::progress();
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
            if (button(ui, hash("mods-installed-update"), ImVec2(left, by), ImVec2(edge, by + bh), does.text, does.enabled ? Look::sticky : Look::plain, does.enabled))
                start_install(m, {package->full_name});
        }
        by -= bh + 16 * u;
    }
    if (!mod->gone && button(ui, hash("mods-installed-toggle"), ImVec2(left, by), ImVec2(edge, by + bh), mod->enabled ? "SWITCH OFF" : "SWITCH ON",
                             mod->enabled ? Look::plain : Look::sticky))
        ms::set_enabled(mod->folder, !mod->enabled);
}

// ---------------------------------------------------------------- UPDATES

void updates_page(Ui &ui, ModPage &m, ImVec2 a, ImVec2 z) {
    auto *draw = ui.draw;
    const float u = ui.u, gap = 28 * u, bar = 96 * u, pad = 24 * u;
    const auto progress = ms::progress();
    const auto count = m.updates.size();
    std::string title = count == 0 ? "EVERYTHING IS UP TO DATE" : count == 1 ? "1 MOD HAS AN UPDATE" : std::to_string(count) + " MODS HAVE UPDATES";
    draw->AddText(ui.heading, 56 * u, ImVec2(a.x, a.y + (bar - 56 * u) * .5f), theme::paper, title.c_str());
    if (count) {
        std::vector<std::string> all;
        std::uint64_t bytes{};
        for (const auto *package : m.updates) all.push_back(package->full_name), bytes += package->latest().file_size;
        std::erase_if(all, [&](const std::string &name) { return std::find(progress.queued.begin(), progress.queued.end(), name) != progress.queued.end() ||
                                                                (progress.busy && progress.package == name); });
        if (button(ui, hash("mods-update-all"), ImVec2(z.x - 640 * u, a.y), ImVec2(z.x, a.y + bar),
                   all.empty() ? std::string("ALL QUEUED") : (progress.busy ? "QUEUE ALL  (" : "UPDATE ALL  (") + ms::size_text(bytes) + ")",
                   all.empty() ? Look::plain : Look::sticky, !all.empty()))
            start_install(m, all);
    }
    const ImVec2 la(a.x, a.y + bar + gap), lz(z.x, z.y);
    const float row_height = 120 * u;
    Scroller list(ui, hash("mods-updates"), la, lz);
    const auto installed_version = [&](const ts::Package &package) -> std::string {
        if (!m.mods) return {};
        const auto found = m.mods->versions.find(ts::folder_for(package.full_name));
        return found == m.mods->versions.end() ? std::string() : found->second;
    };
    for (const auto *package : m.updates) {
        const auto id = hash(package->full_name, 61);
        bool drawn{};
        const float top = list.next(row_height, 8 * u, drawn);
        const ImVec2 p(la.x, top), q(lz.x - 20 * u, top + row_height);
        const Hit hit = item(ui, id, p, q, &list.box);
        list.reveal(id, top, row_height);
        const auto does_now = queue_button(m, *package, ms::Action::update, "UPDATE");
        if (hit.press && does_now.enabled) start_install(m, {package->full_name});
        if (!drawn) continue;
        const bool lit = hit.focus || hit.hover;
        skate_theme::rough_rect(draw, p, q, lit ? theme::blue : skate_theme::tile, 0xb00 + (id & 0xff), u * 2);
        const float icon = row_height - 20 * u;
        package_icon(ui, ImVec2(p.x + pad, p.y + 10 * u), ImVec2(p.x + pad + icon, p.y + 10 * u + icon), package, package->title());
        const float tx = p.x + pad * 2 + icon;
        const auto &does = does_now.text;
        const float dw = width_of(ui.bold, 32 * u, does) + 60 * u;
        const ImVec2 c(q.x - pad - dw, p.y + 24 * u), d(q.x - pad, q.y - 24 * u);
        skate_theme::rough_rect(draw, c, d, does_now.enabled ? sticky : sticky_off, 0x52, u * 2);
        draw->AddText(ui.bold, 32 * u, ImVec2(c.x + 30 * u, c.y + (d.y - c.y - 32 * u) * .5f), skate_theme::black, does.c_str());
        label(draw, ui.bold, 40 * u, ImVec2(tx, p.y + 16 * u), c.x - 30 * u, theme::paper, package->title());
        const auto have = installed_version(*package);
        label(draw, ui.body, 30 * u, ImVec2(tx, p.y + 70 * u), c.x - 30 * u, lit ? theme::paper : theme::muted,
              "v" + (have.empty() ? std::string("?") : have) + "  >  v" + package->latest().number + "   " + ms::date_text(package->date_updated) +
                  (package->latest().file_size ? "   " + ms::size_text(package->latest().file_size) : std::string()));
    }
    if (!count) {
        bool shown{};
        const float top = list.next(60 * u, 0, shown);
        draw->AddText(ui.body, 38 * u, ImVec2(la.x, top), theme::muted,
                      m.listing->packages.empty() ? "Getting the mods from Thunderstore..." : "Every mod you have from Thunderstore is its newest version.");
    }
    list.end();
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
    // A mod's page belongs to BROWSE: changing tab (LB / RB) leaves it.
    if (m.tab != browse_tab) m.details.clear(), m.categories_open = false;
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
    // Y changes the order on BROWSE; View (Select) applies the changes, from anywhere here. (B and
    // Escape going back from a mod's page are hub_frame's.)
    const std::uint32_t pressed = ui.input && !h.typing ? ui.pad.buttons & ~h.pad_before : 0;
    if (m.tab == browse_tab && m.details.empty() && !m.categories_open && (pressed & 0x8000))
        m.sort = static_cast<ms::Sort>((static_cast<std::size_t>(m.sort) + 1) % ms::sort_count);
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
    default: m.tab = browse_tab; break;
    }
    const float fy = z.y - foot;
    float right = z.x;
    if (changes && !live_mods::busy()) {
        std::string apply = live_mods::apply_label(m.mods->effect);
        for (auto &c : apply) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const float w = width_of(ui.bold, 34 * u, apply) + 100 * u;
        if (button(ui, hash("mods-apply"), ImVec2(right - w, fy), ImVec2(right, z.y), apply, Look::sticky, !progress.busy)) mod_say(m, ms::apply());
        right -= w + 20 * u;
    }
    // A bar with how far something is: `fraction` below 0 is not known, and a block goes back and forth.
    const auto bar = [&](ImVec2 b0, ImVec2 b1, float fraction) {
        draw->AddRectFilled(b0, b1, skate_theme::tile_light, 6 * u);
        if (fraction >= 0) {
            draw->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * std::clamp(fraction, 0.f, 1.f), b1.y), theme::blue, 6 * u);
            return;
        }
        const float t = static_cast<float>(std::fmod(ImGui::GetTime(), 2.0)) / 2.f, w = (b1.x - b0.x) * .25f;
        const float x = b0.x + (b1.x - b0.x - w) * (t < .5f ? t * 2 : 2 - t * 2);
        draw->AddRectFilled(ImVec2(x, b0.y), ImVec2(x + w, b1.y), theme::blue, 6 * u);
    };
    if (progress.busy) {
        // What is being installed: its icon and name, the step and which of how many, how much has
        // come, and a bar, with CANCEL at the end.
        const ImVec2 sa(a.x, fy), sz(right - 20 * u, z.y);
        skate_theme::rough_rect(draw, sa, sz, skate_theme::tile, 0x3b7, u * 2);
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
        draw->AddText(ui.bold, 26 * u, ImVec2(x, fy + 8 * u), theme::blue, where.c_str());
        label(draw, ui.bold, 36 * u, ImVec2(x, fy + 38 * u), bar_x - 20 * u, theme::paper, name);
        std::string amount = progress.step == Step::downloading
            ? (progress.total ? ms::size_text(progress.received) + " of " + ms::size_text(progress.total) : ms::size_text(progress.received))
            : progress.step == Step::installing ? std::string("Putting it in your Mods folder") : std::string();
        if (progress.fraction >= 0 && progress.step != Step::uninstalling) amount += (amount.empty() ? "" : "   ") + std::to_string(static_cast<int>(progress.fraction * 100)) + "%";
        draw->AddText(ui.body, 26 * u, ImVec2(bar_x, fy + 10 * u), theme::muted, amount.c_str());
        bar(ImVec2(bar_x, fy + 46 * u), ImVec2(bar_x + bar_w, fy + 64 * u), progress.fraction);
        return;
    }
    if (live_mods::busy()) {
        // The merge: its step and how far, and what happens at the end of it.
        const auto applying = live_mods::progress();
        const ImVec2 sa(a.x, fy), sz(right - 20 * u, z.y);
        skate_theme::rough_rect(draw, sa, sz, skate_theme::tile, 0x3b8, u * 2);
        const float x = sa.x + 24 * u, bar_w = std::min(900 * u, (sz.x - x) * .42f), bar_x = sz.x - 24 * u - bar_w;
        const auto shown = std::min(applying.done + 1, applying.total);
        const std::string where = applying.total ? "APPLYING MODS  " + std::to_string(shown) + " OF " + std::to_string(applying.total) : std::string("APPLYING MODS");
        draw->AddText(ui.bold, 26 * u, ImVec2(x, fy + 8 * u), theme::blue, where.c_str());
        label(draw, ui.bold, 34 * u, ImVec2(x, fy + 40 * u), bar_x - 20 * u, theme::paper, applying.step.empty() ? std::string("Merging the mods into the game...") : applying.step);
        draw->AddText(ui.body, 26 * u, ImVec2(bar_x, fy + 10 * u), theme::muted,
                      m.mods && m.mods->effect == live_mods::ApplyEffect::reload_level ? "The level reloads when it is done" : "It takes effect at the next level load");
        bar(ImVec2(bar_x, fy + 46 * u), ImVec2(bar_x + bar_w, fy + 64 * u), applying.total ? static_cast<float>(applying.done) / static_cast<float>(applying.total) : -1.f);
        return;
    }
    std::string line = m.notice;
    ImU32 colour = m.notice_bad ? skate_theme::danger : theme::paper;
    if (line.empty() && changes) line = std::to_string(changes) + (changes == 1 ? " change is" : " changes are") + " not in the game yet: apply them when you are ready.", colour = skate_theme::warning;
    if (line.empty() && m.mods && m.mods->waiting)
        line = std::to_string(m.mods->waiting) + (m.mods->waiting == 1 ? " change goes" : " changes go") + " in the next time the game starts.", colour = skate_theme::warning;
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
        if (!name.empty()) x += theme::keycap(draw, ui.bold, u * 2, ImVec2(x, fy + 20 * u), name.c_str(), what) + 36 * u;
    };
    if (m.tab == browse_tab && m.categories_open) hint(0x1000, "Enter", "Show only / hide / either"), hint(0x2000, "Esc", "Done");
    else if (m.tab == browse_tab && !m.details.empty()) hint(0x2000, "Esc", "Back to mods");
    else if (m.tab == browse_tab && h.focus == featured_card && h.ring) hint(0x1000, "Enter", "Details"), hint(0x4000, "", "Install"), hint(0x000C, "Left / Right", "More");
    else if (m.tab == browse_tab) hint(0x1000, "Enter", "Open"), hint(0x8000, "", "Sort");
    else if (m.tab == installed_tab) hint(0x1000, "Enter", "Switch on / off");
    else hint(0x1000, "Enter", "Update");
    if (can_apply) hint(0x0020, "", "Apply");
    label(draw, ui.body, 30 * u, ImVec2(x + 10 * u, fy + 22 * u), right - 20 * u, colour, line);
}
