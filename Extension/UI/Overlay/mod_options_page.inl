// Mod Options' settings tabs (WORLD to SETTINGS; MAPS is tools_page's own grid), made for a controller:
// one column of big rows under group headings on the left, and on the right a panel about the row the
// focus is on: its group, its name, what it is now, what it does, and what the buttons do to it. Up and
// down choose a row; A turns a switch on or off, does an action, or steps a choice on; left and right
// change a choice or a number (held, they keep going: hub_frame repeats them); Y puts a row back to its
// default where it has one. A row that cannot be changed now still takes the focus, so the panel can
// say why. Included into hub_page.cpp, after its controls (item, Scroller) and before tools_page.
//
// Every row does what the Insert menu's control for it does (skate_menu_*.cpp, multiplayer_menu.cpp,
// atmosphere_menu.cpp): the same console commands, debug requests and multiplayer actions.

// The panel's quieter inks: a row's detail, and the group name over the panel's title (the kit's news blue).
constexpr ImU32 soft_ink = IM_COL32(150, 152, 158, 255), news_blue = IM_COL32(124, 196, 255, 255);

// One row of a tab, or a heading over a group of them.
struct Opt {
    enum class Kind { heading, toggle, choice, number, action, bind, info, text };
    Kind kind{};
    std::string name, value, help;
    std::string why_not;     // shown in the panel while it cannot be changed
    bool enabled = true;
    bool on = false;         // a switch's state
    float fraction = -1;     // a number's place in its range, for its bar; < 0: none
    std::function<void(int)> change; // 0 for A on a switch, an action or a bind; -1 / +1 for a step
    std::function<void()> reset;     // Y; empty: no default
    std::string reset_does;          // what Y does, said in the panel
    // A text row: A gives it the keyboard (Enter or Escape gives it back); `value` is shown while
    // `text` is empty.
    std::string *text{};
    std::size_t limit = 64;
    bool secret{};
};
using Opts = std::vector<Opt>;

void opt_heading(Opts &o, std::string name) {
    Opt row;
    row.kind = Opt::Kind::heading;
    row.name = std::move(name);
    o.push_back(std::move(row));
}
Opt &opt_toggle(Opts &o, std::string name, bool on, bool enabled, std::string help, std::function<void(bool)> set) {
    Opt row;
    row.kind = Opt::Kind::toggle;
    row.name = std::move(name), row.help = std::move(help), row.on = on, row.enabled = enabled, row.value = on ? "ON" : "OFF";
    row.change = [set = std::move(set), on](int) { set(!on); };
    o.push_back(std::move(row));
    return o.back();
}
Opt &opt_choice(Opts &o, std::string name, std::string value, bool enabled, std::string help, std::function<void(int)> step) {
    Opt row;
    row.kind = Opt::Kind::choice;
    row.name = std::move(name), row.value = std::move(value), row.help = std::move(help), row.enabled = enabled, row.change = std::move(step);
    o.push_back(std::move(row));
    return o.back();
}
Opt &opt_number(Opts &o, std::string name, std::string value, float fraction, bool enabled, std::string help, std::function<void(int)> step) {
    auto &row = opt_choice(o, std::move(name), std::move(value), enabled, std::move(help), std::move(step));
    row.kind = Opt::Kind::number;
    row.fraction = std::clamp(fraction, 0.f, 1.f);
    return row;
}
Opt &opt_action(Opts &o, std::string name, std::string does, bool enabled, std::string help, std::function<void()> run) {
    Opt row;
    row.kind = Opt::Kind::action;
    row.name = std::move(name), row.value = std::move(does), row.help = std::move(help), row.enabled = enabled;
    row.change = [run = std::move(run)](int) { run(); };
    o.push_back(std::move(row));
    return o.back();
}
Opt &opt_text(Opts &o, std::string name, std::string &text, std::string empty, bool enabled, std::string help, std::size_t limit = 64,
             bool secret = false) {
    Opt row;
    row.kind = Opt::Kind::text;
    row.name = std::move(name), row.value = std::move(empty), row.help = std::move(help), row.enabled = enabled;
    row.text = &text, row.limit = limit, row.secret = secret;
    o.push_back(std::move(row));
    return o.back();
}
Opt &opt_info(Opts &o, std::string name, std::string value, std::string help = {}) {
    Opt row;
    row.kind = Opt::Kind::info;
    row.name = std::move(name), row.value = std::move(value), row.help = std::move(help);
    o.push_back(std::move(row));
    return o.back();
}

// The rows that take left and right (choices and numbers), drawn last frame: hub_frame asks before
// moving the focus sideways (options_take_step), and gives the step to the row instead.
std::vector<std::uint32_t> option_steppers;
int option_steppers_frame = -1;
std::uint32_t option_step_id{};
int option_step{};
bool options_take_step(std::uint32_t focus, int direction) {
    if (option_steppers_frame + 1 < ImGui::GetFrameCount()) return false;
    if (std::find(option_steppers.begin(), option_steppers.end(), focus) == option_steppers.end()) return false;
    option_step_id = focus, option_step = direction;
    return true;
}

// The next of a list's entries from `at`, going round.
int wrap_step(int at, int count, int direction) { return count <= 0 ? 0 : ((at < 0 ? 0 : at) + direction + count) % count; }

// The row the focus is on, and the group it is in, for the card about it.
struct OptionsFocus {
    const Opt *row{};
    std::string group;
};

// The rows in a..z, scrolling, with what A, a click, a step and Y do to them. `y_pressed`: Y went
// down this frame. (Its rows join the steppers hub_frame asks about; a page draws one list a frame.)
OptionsFocus options_list(Ui &ui, std::uint32_t tab_id, const Opts &rows, ImVec2 a, ImVec2 z, bool y_pressed) {
    auto &h = ui.h;
    const float u = ui.u, list_w = z.x - a.x;
    const float tall = 96 * u, between = 8 * u, head = 74 * u;
    auto *draw = ui.draw;
    option_steppers.clear();
    option_steppers_frame = ImGui::GetFrameCount();

    const Opt *focused{};
    std::string focused_group, group;
    const auto id_of = [&](const Opt &row) { return hash(group + "/" + row.name, tab_id); };
    // The first row the focus can land on, for a focus that is not on any of them.
    std::uint32_t first{};
    {
        Scroller list(ui, hash("options", tab_id), a, ImVec2(a.x + list_w, z.y));
        bool placed{};
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto &row = rows[i];
            bool drawn{};
            if (row.kind == Opt::Kind::heading) {
                group = row.name;
                const float top = list.next(placed ? head : head - 30 * u, 0, drawn);
                if (drawn) {
                    // As skate.'s list headings ("GRIND TRICKS"): white capitals over a white brush line.
                    const float base = top + (placed ? head : head - 30 * u) - 16 * u, right = a.x + list_w - 26 * u;
                    draw->AddText(ui.bold, 34 * u, ImVec2(a.x + 4 * u, base - 46 * u), theme::paper, row.name.c_str());
                    draw->AddQuadFilled(ImVec2(a.x, base - 2 * u), ImVec2(right, base - 5 * u), ImVec2(right - 6 * u, base + 3 * u), ImVec2(a.x + 4 * u, base + 5 * u),
                                        theme::paper);
                }
                placed = true;
                continue;
            }
            placed = true;
            const auto id = id_of(row);
            if (!first) first = id;
            const float top = list.next(tall, between, drawn);
            list.reveal(id, top, tall);
            const ImVec2 p(a.x, top), q(a.x + list_w - 26 * u, top + tall);
            const Hit hit = item(ui, id, p, q, &list.box);
            const bool steps = row.kind == Opt::Kind::choice || row.kind == Opt::Kind::number;
            if (steps) option_steppers.push_back(id);
            if (h.focus == id) focused = &row, focused_group = group;
            // A text row: A or a click gives it the keyboard, and Enter, Escape or a click elsewhere gives
            // it back (as field() does).
            bool typing = row.kind == Opt::Kind::text && h.typing == id;
            if (row.kind == Opt::Kind::text && row.text) {
                if (hit.press && row.enabled && !typing) set_typing(h, id), typing = true;
                else if (typing && ui.clicked && !hit.hover) set_typing(h, 0), typing = false;
                if (typing) {
                    take_typing(*row.text, row.limit);
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
                        set_typing(h, 0), typing = false;
                }
            }
            // What happened to it this frame: A or a click, a step from hub_frame, Y.
            if (row.enabled && row.change) {
                if (hit.press) {
                    // A click on a choice or a number: its left half steps back, its right half on.
                    const bool mouse = ui.clicked && inside(ui.mouse, p, q);
                    row.change(steps && mouse && ui.mouse.x < (p.x + q.x) * .5f ? -1 : 1);
                } else if (steps && option_step_id == id && option_step) {
                    row.change(option_step);
                    h.sound = sound_navigate;
                }
            }
            if (h.focus == id && y_pressed && row.enabled && row.reset) row.reset(), h.sound = sound_select;
            if (!drawn) continue;
            const bool lit = (hit.focus && !hit.hover) || typing;
            const ImU32 fill = lit ? theme::focus_fill : hit.hover ? row_hover : tile_dark;
            const ImU32 ink = lit ? focus_ink : row.enabled ? theme::paper : theme::muted;
            theme::tile_rect(draw, p, q, fill, id & 0xfff, u * 2);
            const float size = 38 * u, text_y = top + (tall - size) * .5f;
            // The value at the right end: ON in blue, OFF grey; a choice or a number in blue, with its
            // arrows while it has the focus; an action's word; a bind's buttons.
            std::string value = row.value;
            if (steps && hit.focus) value = "<  " + value + "  >";
            const bool shows_text = row.kind == Opt::Kind::text && row.text && (!row.text->empty() || typing);
            if (shows_text) value = row.secret ? std::string(row.text->size(), '*') : *row.text;
            if (typing && std::fmod(ImGui::GetTime(), 1.0) < .5) value += '|';
            ImU32 value_ink = lit ? focus_ink : !row.enabled ? theme::muted
                            : row.kind == Opt::Kind::text ? (shows_text ? theme::paper : theme::muted)
                            : row.kind == Opt::Kind::toggle ? (row.on ? theme::blue : theme::muted)
                            : row.kind == Opt::Kind::info ? soft_ink : theme::blue;
            // (A long text keeps its end in view, where the typing is.)
            const float name_w = width_of(ui.body, size, row.name), room = std::max(60 * u, q.x - p.x - name_w - 120 * u);
            const float value_w = std::min(width_of(ui.bold, size, value), room);
            label(draw, ui.body, size, ImVec2(p.x + 30 * u, text_y), q.x - value_w - 60 * u, ink, row.name);
            draw->PushClipRect(ImVec2(q.x - value_w - 30 * u, top), ImVec2(q.x - 30 * u, top + tall), true);
            const float full_w = width_of(ui.bold, size, value);
            draw->AddText(ui.bold, size, ImVec2(q.x - 30 * u - (typing ? full_w : value_w), text_y), value_ink, value.c_str());
            draw->PopClipRect();
            // A number's place in its range, as a thin bar along the row's foot.
            if (row.kind == Opt::Kind::number && row.fraction >= 0) {
                const float bar_y = q.y - 12 * u, left = p.x + 30 * u, right = q.x - 30 * u;
                draw->AddRectFilled(ImVec2(left, bar_y), ImVec2(right, bar_y + 4 * u), lit ? IM_COL32(0, 70, 150, 255) : IM_COL32(48, 48, 52, 255));
                draw->AddRectFilled(ImVec2(left, bar_y), ImVec2(left + (right - left) * row.fraction, bar_y + 4 * u), lit ? skate_theme::black : theme::blue);
            }
        }
        list.end();
    }
    option_step_id = 0, option_step = 0;
    // A focus that is not on any of this tab's rows (just opened, or the row went away) waits on the
    // tab strip until the D-pad comes down to the first row: hub_frame's own rule for every page.
    (void)first;
    return {focused, focused_group};
}

// The card about the focused row in pa..pz: skate.'s description card (Skatepedia's), off-white paper
// and black text. `text_scale` shrinks its title and value, for a small card under a list.
void options_card(Ui &ui, std::uint32_t seed, const OptionsFocus &focus, ImVec2 pa, ImVec2 pz, float text_scale = 1.f) {
    const float u = ui.u;
    auto *draw = ui.draw;
    const Opt *focused = focus.row;
    const std::string &focused_group = focus.group;
    constexpr ImU32 paper_card = IM_COL32(234, 234, 230, 250), card_ink = IM_COL32(16, 16, 16, 255), card_sub = IM_COL32(70, 70, 72, 255);
    theme::tile_rect(draw, pa, pz, paper_card, seed & 0xfff, u * 2);
    const float left = pa.x + 60 * u * text_scale, right = pz.x - 60 * u * text_scale, wrap = right - left;
    float y = pa.y + 56 * u * text_scale;
    if (!focused) {
        draw->AddText(ui.body, 36 * u, ImVec2(left, y), card_sub, "Choose a setting to see what it does.", nullptr, wrap);
        return;
    }
    if (!focused_group.empty()) {
        draw->AddText(ui.bold, 28 * u, ImVec2(left, y), theme::blue, focused_group.c_str());
        y += 28 * u + 22 * u;
    }
    std::string title = focused->name;
    for (auto &c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    {
        const float size = 76 * u * text_scale;
        const auto extent = ui.heading->CalcTextSizeA(size, FLT_MAX, wrap, title.c_str());
        draw->AddText(ui.heading, size, ImVec2(left, y), card_ink, title.c_str(), nullptr, wrap);
        y += extent.y + 30 * u;
    }
    if (!focused->value.empty() && focused->kind != Opt::Kind::action && focused->kind != Opt::Kind::text && text_scale >= 1.f) {
        const float size = 60 * u;
        const ImU32 colour = focused->kind == Opt::Kind::toggle && !focused->on ? card_sub : focused->kind == Opt::Kind::info ? card_ink : theme::blue;
        draw->AddText(ui.bold, size, ImVec2(left, y), colour, focused->value.c_str());
        if (focused->kind == Opt::Kind::number && focused->fraction >= 0) {
            const float bar_x = left + width_of(ui.bold, size, focused->value) + 40 * u, bar_y = y + size * .5f - 6 * u;
            if (bar_x < right - 100 * u) {
                draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(right, bar_y + 12 * u), IM_COL32(196, 196, 192, 255));
                draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + (right - bar_x) * focused->fraction, bar_y + 12 * u), theme::blue);
            }
        }
        y += size + 36 * u;
    }
    const auto paragraph = [&](const std::string &text, ImU32 colour, float size) {
        if (text.empty()) return;
        const auto extent = ui.body->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
        draw->AddText(ui.body, size, ImVec2(left, y), colour, text.c_str(), nullptr, wrap);
        y += extent.y + 26 * u;
    };
    paragraph(focused->help, card_ink, 36 * u);
    if (!focused->enabled && focused->kind != Opt::Kind::info)
        paragraph(focused->why_not.empty() ? std::string("Not available right now.") : focused->why_not, IM_COL32(168, 60, 20, 255), 34 * u);
    // What the buttons do to it, at the panel's foot.
    std::vector<std::pair<std::string, std::string>> does;
    if (focused->enabled) {
        switch (focused->kind) {
        case Opt::Kind::toggle: does.emplace_back(ui.pad.available ? "A" : "Enter", "On / off"); break;
        case Opt::Kind::choice: does.emplace_back(ui.pad.available ? "LEFT RIGHT" : "Left Right", "Change it"); break;
        case Opt::Kind::number: does.emplace_back(ui.pad.available ? "LEFT RIGHT" : "Left Right", "Less / more (hold to keep going)"); break;
        case Opt::Kind::action: does.emplace_back(ui.pad.available ? "A" : "Enter", focused->value.empty() ? std::string("Do it") : focused->value); break;
        case Opt::Kind::bind: does.emplace_back(ui.pad.available ? "A" : "Enter", "Record a new bind"); break;
        case Opt::Kind::text: does.emplace_back(ui.pad.available ? "A" : "Enter", "Type it"); break;
        default: break;
        }
        if (focused->reset) does.emplace_back(ui.pad.available ? "Y" : "", focused->reset_does.empty() ? std::string("Back to its default") : focused->reset_does);
    }
    float foot = pz.y - 56 * u - static_cast<float>(does.size()) * 58 * u;
    for (const auto &[key, what] : does) {
        float x = left;
        if (!key.empty()) {
            // (Black on the paper, where the page's white keycaps would not show.)
            const float scale = u * 2.2f, size = 12 * scale, w = ui.bold->CalcTextSizeA(size, FLT_MAX, 0, key.c_str()).x + 12 * scale;
            draw->AddRectFilled(ImVec2(x, foot), ImVec2(x + w, foot + 20 * scale), card_ink, 3 * scale);
            draw->AddText(ui.bold, size, ImVec2(x + 6 * scale, foot + (20 * scale - size) * .5f), theme::paper, key.c_str());
            x += w + 24 * u;
        }
        draw->AddText(ui.body, 32 * u, ImVec2(x, foot + 4 * u), card_sub, what.c_str());
        foot += 58 * u;
    }
}

// A Mod Options tab: its rows on the left, the card about the focused one on the right.
void options_page(Ui &ui, std::uint32_t tab_id, const Opts &rows, ImVec2 a, ImVec2 z, bool y_pressed) {
    const float u = ui.u, gap = 40 * u, width = z.x - a.x;
    const float list_w = std::min(std::max(width * .58f, 1500 * u), width - 900 * u);
    const auto focus = options_list(ui, tab_id, rows, a, ImVec2(a.x + list_w, z.y), y_pressed);
    options_card(ui, hash("options-panel", tab_id), focus, ImVec2(a.x + list_w + gap - 26 * u, a.y), z);
}

// ---------------------------------------------------------------- what each tab holds

// What a tab's rows act through: the same channels tools_page has always used.
struct OptionsContext {
    Hub &h;
    const Model &m;
    const CallbacksV3 &cb;
    std::function<void(const std::string &)> run;           // a console command
    std::function<void(DebugRequest)> debug;                // a debug request
    std::function<void(const char *, const std::string &)> online; // a multiplayer action
    std::function<void(std::string_view, const std::string &)> act; // a Mod Options action (native_tools_view.h)
    bool console{}, debugging{};
};

std::string one_decimal(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}
template<std::size_t N> std::size_t nearest_step(float current, const std::array<float, N> &steps) {
    std::size_t best{};
    for (std::size_t i = 1; i < N; ++i)
        if (std::abs(steps[i] - current) < std::abs(steps[best] - current)) best = i;
    return best;
}
template<std::size_t N> float fraction_of(float current, const std::array<float, N> &steps) {
    return N > 1 ? static_cast<float>(nearest_step(current, steps)) / static_cast<float>(N - 1) : 0.f;
}

void world_rows(Opts &o, OptionsContext &c) {
    const auto &m = c.m;
    const bool loaded = m.world.map != WorldMap::none;
    const std::string load_first = "Load a map on the MAPS tab first.";

    opt_heading(o, "TIME");
    {
        const bool can = native_tools::time_of_day_available(m, c.cb);
        const int now = static_cast<int>(native_tools::time_of_day(m));
        static int asked = -1;
        static ULONGLONG asked_until{};
        if (asked >= 0 && (asked == now || GetTickCount64() > asked_until)) asked = -1;
        const int shown = asked >= 0 ? asked : now;
        const int count = static_cast<int>(native_tools::time_of_day_labels.size());
        auto &row = opt_choice(o, "Time of day", native_tools::time_of_day_labels[static_cast<std::size_t>(std::clamp(shown, 0, count - 1))], can,
                               "Default follows the level's own time. Your choice saves and comes back when you load this map again.",
                               [&c, shown, count](int way) {
                                   const int next = wrap_step(shown, count, way);
                                   asked = next, asked_until = GetTickCount64() + 4000;
                                   for (const auto &line : native_tools::time_of_day_commands(c.m, static_cast<unsigned>(next))) c.run(line);
                               });
        row.why_not = m.world.controlled_by_host ? (server_can(m.multiplayer, "tod") ? "" : "The lobby host controls world layers, including time of day.")
                    : !loaded ? load_first : "Time of day is not available on this map right now.";
        if (m.world.controlled_by_host && server_can(m.multiplayer, "tod")) row.help += " Your role on this server sets it for everyone.";
        row.reset = [&c] { for (const auto &line : native_tools::time_of_day_commands(c.m, 0)) c.run(line); };
        row.reset_does = "Back to the level's own time";
    }

    opt_heading(o, "STREETS");
    {
        const auto &controls = m.world_controls;
        const bool can = controls.population_available && m.world.ready && c.console;
        const int count = static_cast<int>(population_labels.size());
        for (int which = 0; which < 2; ++which) {
            const int chosen = (which == 0 ? controls.choices.traffic : controls.choices.pedestrians) + 1;
            const std::string nearby = controls.population_ready[static_cast<std::size_t>(which)]
                                           ? std::to_string(controls.population[static_cast<std::size_t>(which)]) + " nearby right now."
                                           : std::string("Waiting for the population count.");
            auto &row = opt_choice(o, which == 0 ? "Traffic" : "Pedestrians", population_labels[static_cast<std::size_t>(std::clamp(chosen, 0, count - 1))], can,
                                   (which == 0 ? "How many vehicles drive the streets. " : "How many people walk the streets. ") + nearby,
                                   [&c, which, chosen, count](int way) {
                                       const int next = wrap_step(chosen, count, way);
                                       c.run("environment " + std::string(which == 0 ? "traffic" : "pedestrians") + " " + std::to_string(next - 1));
                                   });
            row.why_not = loaded ? "Traffic and pedestrians are not ready on this map yet." : load_first;
            row.reset = [&c, which] { c.run("environment " + std::string(which == 0 ? "traffic" : "pedestrians") + " -1"); };
            row.reset_does = "Back to the level's own";
        }
    }

    opt_heading(o, "SCREEN EFFECTS");
    {
        static constexpr std::array<const char *, 3> names{"Film grain", "Vignette", "Chromatic aberration"};
        static constexpr std::array<const char *, 3> helps{"Grain over the picture. On allows the scene's authored grain amount.",
                                                           "Darkening around the edges of the picture.", "Color fringing around the edges of the picture."};
        const bool can = m.graphics.available && c.console;
        for (unsigned i = 0; i < names.size(); ++i) {
            const int choice = m.graphics.choices.effects[i];
            const bool on = choice < 0 ? (m.graphics.ready[i] ? m.graphics.enabled[i] : true) : choice != 0;
            auto &row = opt_toggle(o, names[i], on, can, std::string(helps[i]) + (choice < 0 ? " Following the scene now." : ""),
                                   [&c, i](bool next) { c.run(std::string("graphics ") + graphics_keys[i] + (next ? " 1" : " 0")); });
            row.reset = [&c, i] { c.run(std::string("graphics ") + graphics_keys[i] + " -1"); };
            row.reset_does = "Back to what the scene asks for";
        }
        opt_action(o, "Restore effect defaults", "RESTORE", can, "Every effect back to what the scene asks for.", [&c] { c.run("graphics reset -1"); });
    }

    opt_heading(o, "CHALLENGES");
    {
        const auto &p = m.progression;
        auto &row = opt_toggle(o, "Show challenges", !p.challenges_hidden, p.challenges_enabled && c.console,
                               "Challenges in the world, on the compass and on the map. Hiding them keeps your progress.",
                               [&c](bool on) { c.run(on ? "challenges 1" : "challenges 0"); });
        row.why_not = "Challenges are not ready yet.";
    }

    // (Last: on some maps it is a very long list.)
    opt_heading(o, "WORLD LAYERS");
    {
        static int kind = 0;
        static const std::array<const char *, 8> kinds{"All categories", "Seasonal", "Activities", "World", "Lighting", "Structure", "Debug", "Parks"};
        const bool can = !native_tools::layers_locked(m) && m.world.available && m.world.ready && c.console;
        opt_choice(o, "Show", kinds[static_cast<std::size_t>(kind)], loaded, "Which kind of layer is listed below.",
                   [](int way) { kind = wrap_step(kind, static_cast<int>(kinds.size()), way); });
        std::string why = !loaded ? load_first
                        : m.world.controlled_by_host && !server_can(m.multiplayer, "layers") ? "The host is syncing world layers."
                                                                                             : "World layers are not ready on this map yet.";
        unsigned shown{};
        for (unsigned i = 0; i < world_layers().size(); ++i) {
            const auto &layer = world_layers()[i];
            if (layer.map != m.world.map || !m.world.supported[i] || world_layer_kept_off(layer.key)) continue;
            if (kind && layer.category != kinds[static_cast<std::size_t>(kind)]) continue;
            ++shown;
            const bool follows = m.world.choices[i] == "default";
            const bool enabled = follows ? m.world.enabled[i].value_or(false) : m.world.choices[i] == "on";
            std::string help(layer.detail);
            if (!follows) help += " Saved override.";
            if (!m.world.status[i].empty()) help += " " + m.world.status[i];
            const std::string key(layer.key);
            auto &row = opt_toggle(o, world_layer_title(layer.label), enabled, can, help, [&c, key](bool on) { c.run("world " + key + (on ? " on" : " off")); });
            row.why_not = why;
        }
        if (loaded && !shown) opt_info(o, "No layers of that kind", "", "This map has no world layers of that kind.");
        auto &defaults = opt_action(o, "Restore layer defaults", "RESTORE", can, "Every layer on this map back to the game's control.",
                                    [&c] { c.run("world defaults " + std::string(world_map_key(c.m.world.map))); });
        defaults.why_not = why;
    }
}

void parks_rows(Opts &o, OptionsContext &c) {
    const auto &m = c.m;
    const bool on_bam = m.world.map == WorldMap::bam;
    const bool locked = !m.parks.available || native_tools::parks_locked(m) || !c.console;
    const std::string why = !on_bam ? "Parks are on San Vansterdam. Load it on the MAPS tab."
                          : native_tools::parks_locked(m) ? "The lobby host controls park layouts."
                                                          : "Parks are not ready yet.";

    opt_heading(o, "THE LOTS");
    // A row a lot: left and right pick its layout, A loads it.
    static std::array<std::string, 3> staged, seen;
    for (unsigned lot = 0; lot < park_lots.size() && lot < staged.size(); ++lot) {
        if (staged[lot].empty() || seen[lot] != m.parks.choices[lot] || native_tools::parks_locked(m)) staged[lot] = m.parks.choices[lot];
        seen[lot] = m.parks.choices[lot];
        std::vector<std::string> ids{"empty"};
        for (unsigned family = 0; family < park_families.size(); ++family)
            for (unsigned variant = 1; variant <= park_lots[lot].counts[family]; ++variant) ids.push_back(park_id(family, variant));
        int at = 0;
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == staged[lot]) at = static_cast<int>(i);
        const bool loaded = staged[lot] == m.parks.choices[lot];
        const std::string name = staged[lot] == "empty" || staged[lot].empty() ? std::string("Empty lot") : park_label(staged[lot]);
        auto &row = opt_choice(o, std::string(park_lots[lot].label), name + (loaded ? "" : "  (A LOADS)"), on_bam && !locked,
                               loaded ? "Left and right pick a layout; A loads it. Choices save and are shared when you host."
                                      : "Not loaded yet: A loads this layout.",
                               [&c, lot, ids, at, loaded](int way) {
                                   // A on a layout not loaded yet loads it; else it steps on, as left and right do.
                                   if (way > 0 && !loaded && option_step_id == 0) {
                                       c.run("park " + std::string(park_lots[lot].key) + " " + staged[lot]);
                                       return;
                                   }
                                   staged[lot] = ids[static_cast<std::size_t>(wrap_step(at, static_cast<int>(ids.size()), way))];
                               });
        row.why_not = why;
    }
    opt_info(o, "Construction", "", "Construction is both the construction site and the Hedgemont park.");

    opt_heading(o, "ALL LOTS");
    {
        auto &random = opt_action(o, "Load random parks", "LOAD", on_bam && !locked, "A random layout in every lot.", [&c] {
            c.run("park random");
            staged = {};
        });
        random.why_not = why;
        auto &launch = opt_toggle(o, "Randomize on launch", m.parks.randomize_on_launch, m.parks.available && c.console,
                                  "A random layout in every lot once each time the game starts. Multiplayer follows the host.",
                                  [&c](bool on) { c.run(on ? "park random-on-launch 1" : "park random-on-launch 0"); });
        launch.why_not = "Parks are not ready yet.";
    }

    opt_heading(o, "PARK EDITOR");
    {
        const bool can = m.editor.available && m.debug.camera_available && m.debug.ui_available && c.debugging;
        auto &open = opt_action(o, "Open the park editor", "OPEN", can,
                                "Build a park with the freecam, an object library and handles, then save it as a mod others can install.",
                                [&c] { c.debug({DebugAction::set_park_editor, true}); });
        open.why_not = m.editor.status.empty() ? std::string("The park editor is not ready yet.") : m.editor.status;
        const bool rail = m.park_editor_layout == park_editor_layout_rail;
        opt_choice(o, "Editor layout", rail ? "TOOL RAIL" : "HOTBAR", c.console,
                   rail ? "A rail of tools down the left, a panel beside it and the selection on the right."
                        : "The world fills the screen, the objects in a hotbar along the bottom.",
                   [&c, rail](int) { c.run(rail ? "editor layout hotbar" : "editor layout rail"); });
    }

    opt_heading(o, "PARK MODS");
    {
        const auto &editor = m.editor;
        const auto quoted = [](std::string_view text) {
            std::string out = "\"";
            for (const auto ch : text) {
                if (ch == '"' || ch == '\\') out += '\\';
                out += ch;
            }
            return out + '"';
        };
        const auto command = [&editor, quoted](const char *operation, const std::string &folder) {
            return std::string("editor ") + operation + " " + quoted(editor.map) + " " + std::to_string(editor.generation) + " " +
                   std::to_string(editor.revision) + (folder.empty() ? "" : " " + quoted(folder));
        };
        const bool ready = editor.available && !editor.busy && !editor.failed && c.console;
        const std::string why_mod = !editor.can_load_parks ? "In multiplayer only the host can load park mods."
                                  : editor.status.empty() ? std::string("Park mods are not ready yet.") : editor.status;
        unsigned here{};
        for (const auto &mod : editor.park_mods) {
            if (!mod.enabled || !mod.has_map) continue;
            ++here;
            const std::string title = mod.title.empty() ? mod.folder : mod.title;
            std::string detail = mod.author.empty() ? std::string("Unknown author.") : "By " + mod.author + ".";
            if (!mod.version.empty()) detail += " Version " + mod.version + ".";
            if (!mod.description.empty()) detail += " " + mod.description;
            detail += " Loading it replaces your layout on this map; Undo in the park editor brings yours back.";
            auto &load = opt_action(o, title, "LOAD", ready && editor.can_load_parks, detail,
                                    [&c, command, folder = mod.folder] { c.run(command("mod-load", folder)); });
            load.why_not = why_mod;
            auto &edit = opt_action(o, "Edit " + title, "EDIT", ready && editor.can_load_parks, "Load it into the park editor to change and save it.",
                                    [&c, command, folder = mod.folder] {
                                        c.run(command("mod-open", folder));
                                        c.debug({DebugAction::set_park_editor, true});
                                    });
            edit.why_not = why_mod;
        }
        if (!here) opt_info(o, "No park mods for this map", "", editor.map.empty() ? "Load a level to see its park mods." : "No enabled park mod has a park for this map yet.");
        auto &refresh = opt_action(o, "Refresh park mods", "REFRESH", ready, "Read the installed park mods again. Disabled mods are hidden; enable them in the launcher.",
                                   [&c, command] { c.run(command("mods-refresh", {})); });
        refresh.why_not = why_mod;
    }

    opt_heading(o, "PLACED OBJECTS");
    {
        const auto &state = m.object_persistence;
        auto &keep = opt_toggle(o, "Save and restore placed objects", state.enabled, state.available && c.console,
                                "Off pauses saving and restoring. Saved layouts are kept.", [&c](bool on) { c.run(on ? "objects enabled 1" : "objects enabled 0"); });
        keep.why_not = "Placed objects are not ready yet.";
        if (!state.map.empty()) opt_info(o, "This level", std::to_string(state.count) + " SAVED", "Objects saved on " + std::string(world_map_label(m.world.map)) + ". Each one can be teleported to or deleted on the Insert menu's Build page.");
        auto &clear = opt_action(o, "Delete this level's saved objects", state.clearing ? "CLEARING" : "DELETE",
                                 state.available && c.console && state.can_clear, "Removes every saved object of this level.",
                                 [&c, map = state.map] { c.run("objects clear " + map); });
        clear.why_not = state.map.empty() ? "Load a level first." : "Nothing to delete here.";
    }
}

void skater_rows(Opts &o, OptionsContext &c) {
    const auto &d = c.m.debug;
    const bool debugging = c.debugging;

    opt_heading(o, "CHEATS");
    {
        auto &noclip = opt_toggle(o, "Noclip", d.noclip, (d.noclip_available || d.noclip) && debugging,
                                  "Fly with the normal player camera, at the freecam's flight speed (CAMERA tab). Includes No bail. Close the menu to fly.",
                                  [&c](bool on) { c.debug({DebugAction::set_noclip, on}); });
        noclip.why_not = d.noclip_unavailable;
        const char *bail_help = d.noclip && d.no_bail_active ? "Protection is automatic during noclip. Turn this on to keep it when flight ends."
                              : d.no_bail && !d.no_bail_active ? "On; waiting for an active local skater."
                                                               : "Prevent new wipeouts. Recover from any current bail before turning it on.";
        opt_toggle(o, "No bail", d.no_bail, (d.no_bail_available || d.no_bail) && debugging, bail_help,
                   [&c](bool on) { c.debug({DebugAction::set_no_bail, on}); });
        const auto &wear = c.m.offline.board_wear;
        auto &board = opt_toggle(o, "Board wear", wear.effective, wear.available && c.cb.queue_offline_feature,
                                 "Your board's grip and deck scuff up as you skate, as in the game.", [&c](bool) { c.act("board-wear", {}); });
        board.why_not = "Board wear is not ready yet.";
        auto &reset = opt_action(o, "Reset board wear", "RESET", wear.available && wear.effective && c.console, "Your board back to new.",
                                 [&c] { c.act("board-wear-reset", {}); });
        reset.why_not = wear.effective ? "Board wear is not ready yet." : "Board wear is off.";
    }

    opt_heading(o, "BOOSTS");
    {
        constexpr std::array<float, 10> forward{0.f, 1.f, 5.f, 10.f, 25.f, 50.f, 100.f, 150.f, 200.f, 300.f};
        constexpr std::array<float, 9> up{0.f, 1.f, 2.f, 3.f, 5.f, 8.f, 12.f, 18.f, 25.f};
        const auto boost = [&](const char *name, float value, DebugAction action, const auto &steps, const char *help) {
            auto &row = opt_number(o, name, value <= 0 ? std::string("OFF") : "+" + whole(value), fraction_of(value, steps), debugging, help,
                                   [&c, value, action, steps](int way) { c.debug({action, false, stepped(value, steps, way)}); });
            row.reset = [&c, action] { c.debug({action, false, 0.f}); };
            row.reset_does = "Off";
        };
        boost("Forward boost", d.forward_velocity_speed, DebugAction::set_forward_velocity_speed, forward,
              "Speed added forward each press of its button (SETTINGS > BUTTONS). 0 is off.");
        boost("Up boost", d.up_velocity_speed, DebugAction::set_up_velocity_speed, up, "Speed added upward each press of its button. 0 is off.");
        boost("Off-board up boost", d.offboard_up_velocity_speed, DebugAction::set_offboard_up_velocity_speed, up,
              "Upward speed for each press while off the board, falling or gliding. 0 is off.");
        opt_toggle(o, "Revert boost", d.revert_boost, debugging, "Speed boost when you land an auto revert or a board bend. Clean 180s get nothing.",
                   [&c](bool on) { c.debug({DebugAction::set_revert_boost_enabled, on}); });
        constexpr std::array<float, 10> strengths{.5f, .75f, 1.f, 1.5f, 2.f, 2.5f, 3.f, 3.5f, 4.f, 5.f};
        auto &strength = opt_number(o, "Revert boost strength", "x" + one_decimal(d.revert_boost_speed), fraction_of(d.revert_boost_speed, strengths),
                                    debugging && d.revert_boost, "Auto reverts add 1.5 m/s and board bends 2 to 2.6 m/s, times this.",
                                    [&c, value = d.revert_boost_speed, strengths](int way) {
                                        c.debug({DebugAction::set_revert_boost_speed, false, stepped(value, strengths, way)});
                                    });
        strength.why_not = "Turn Revert boost on first.";
        strength.reset = [&c] { c.debug({DebugAction::set_revert_boost_speed, false, 1.f}); };
        strength.reset_does = "x1.0";
    }

    opt_heading(o, "DARK POP");
    {
        opt_toggle(o, "Dark pop", d.dark_pop, debugging,
                   "The pre-0.32.0 dark pop: catch a flip dark, press D-pad Left, Down or Right in the air, land it still dark. "
                   "Play with Dark Catch Assist off and Legacy Dark Catches on.",
                   [&c](bool on) { c.debug({DebugAction::set_dark_pop_enabled, on}); });
        opt_toggle(o, "Hide the D-pad in the air", d.dark_pop_hide_dpad, debugging,
                   "In the air the game sees D-pad Left, Down and Right let go (Dark Pop still sees them), so Right never opens the Quick Drop radial.",
                   [&c](bool on) { c.debug({DebugAction::set_dark_pop_hide_dpad, on}); });
    }

    opt_heading(o, "SANDBOX");
    {
        const auto &p = c.m.progression;
        if (p.score_available) {
            opt_info(o, "Rip score", std::to_string(p.score) + " / " + std::to_string(p.score_cap), "Your saved RIP score and its cap, at level " + std::to_string(p.score_level) + ".");
            auto &fill = opt_action(o, "Fill rip score to the cap", "FILL", c.console && p.score < p.score_cap, "Your RIP score up to its cap.", [&c] {
                const auto &q = c.m.progression;
                c.run("ripscore " + std::to_string(q.score_cap) + " " + std::to_string(q.score_cap) + " " + std::to_string(q.score_level));
            });
            fill.why_not = "It is at its cap already.";
        }
        auto &maxed = opt_toggle(o, "Keep all districts maxed", p.ranks_maxed, c.console, "Hold every district at its top level.",
                                 [&c](bool on) { c.run(std::string("maxranks ") + (on ? "1" : "0")); });
        maxed.why_not = "Not available right now.";
    }
}

void camera_rows(Opts &o, OptionsContext &c) {
    namespace fp = dingosdk::first_person;
    const auto &d = c.m.debug;
    const auto &arm = d.first_person_arm;
    const bool debugging = c.debugging, can_edit = d.available && debugging;
    static constexpr std::array<float, 7> speeds{.6f, 3.f, 5.f, 15.f, 60.f, 300.f, 1500.f};
    static constexpr std::array<const char *, 7> speed_names{"PRECISE", "SLOW", "CRUISE", "DEFAULT", "FAST", "TRAVEL", "MAXIMUM"};

    opt_heading(o, "FREECAM");
    {
        auto &freecam = opt_toggle(o, "Freecam", d.free_camera, d.available && d.camera_available && debugging,
                                   "Detach the camera and explore. Close the menu to fly: the left stick moves, the right stick looks.",
                                   [&c](bool on) { c.debug({DebugAction::set_free_camera, on}); });
        freecam.why_not = d.camera_unavailable;
        opt_toggle(o, "Controller flies the freecam", c.m.bindings.freecam_controller, c.m.bindings.available && c.console,
                   "The controller flies the freecam, and the skater does not move meanwhile.",
                   [&c](bool on) { c.run(on ? "freecam_controller true" : "freecam_controller false"); });
        const std::size_t at = nearest_step(d.camera_speed, speeds);
        auto &speed = opt_number(o, "Flight speed", speed_names[at], static_cast<float>(at) / 6.f, debugging,
                                 "How fast the freecam and noclip fly. Set it here even with both off: it is kept for next time.",
                                 [&c, at](int way) {
                                     const auto next = static_cast<std::size_t>(std::clamp(static_cast<int>(at) + way, 0, 6));
                                     c.debug({DebugAction::set_camera_speed, false, speeds[next]});
                                 });
        speed.reset = [&c] { c.debug({DebugAction::set_camera_speed, false, 15.f}); };
        speed.reset_does = "Default speed";
        const bool custom = d.free_camera_fov > 0;
        const float fov = custom ? d.free_camera_fov : d.camera_fov;
        auto &view = opt_number(o, "Freecam field of view", whole(fov) + (custom ? " DEG" : " DEG (GAME)"), (fov - 40.f) / 80.f, debugging,
                                "How wide the freecam sees, from 40 to 120 degrees.",
                                [&c, fov](int way) { c.debug({DebugAction::set_free_camera_fov, false, std::clamp(std::round(fov) + 5.f * way, 40.f, 120.f)}); });
        view.reset = [&c] { c.debug({DebugAction::set_free_camera_fov, false, 0.f}); };
        view.reset_does = "The game's own";
        auto &go = opt_action(o, "Teleport to the freecam", "GO", d.free_camera && d.camera_position_valid && c.console,
                              "Move the skater to where the freecam is.", [&c] { c.run("tp_to_freecam"); });
        go.why_not = "Turn the freecam on and fly somewhere first.";
    }

    opt_heading(o, "FIRST PERSON");
    {
        auto &first = opt_toggle(o, "First person", d.first_person, d.available && d.camera_available && debugging,
                                 "See the world through the skater's eyes.", [&c](bool on) { c.debug({DebugAction::set_first_person, on}); });
        first.why_not = d.camera_unavailable;
        const float fov = d.first_person_fov > 0 ? d.first_person_fov : d.camera_fov;
        auto &view = opt_number(o, "First person field of view", whole(fov) + " DEG", (fov - 40.f) / 80.f, d.first_person && debugging,
                                "How wide first person sees, from 40 to 120 degrees.",
                                [&c, fov](int way) { c.debug({DebugAction::set_first_person_fov, false, std::clamp(std::round(fov) + 5.f * way, 40.f, 120.f)}); });
        view.why_not = "Turn first person on first.";
        opt_toggle(o, "Third person on foot", arm.board_only, can_edit, "The game's camera while walking; first person comes back on the board.",
                   [&c](bool on) { c.debug({DebugAction::set_first_person_board_only, on}); });
        opt_toggle(o, "In the camera cycle", arm.camera_cycle, can_edit,
                   "R3 on the board goes high camera, low camera, first person, then high again. Needs Camera height control on in the game's settings.",
                   [&c](bool on) { c.debug({DebugAction::set_first_person_camera_cycle, on}); });
    }

    opt_heading(o, "TRUE FIRST PERSON");
    {
        opt_toggle(o, "True first person", arm.stabilize, can_edit, "Keep the horizon level and the view steady; the head's heading is still followed.",
                   [&c](bool on) { c.debug({DebugAction::set_first_person_stabilize, on}); });
        const auto percent = [&](const char *name, float value, DebugAction action, const char *help) {
            auto &row = opt_number(o, name, whole(value) + "%", value / fp::strength_limit, can_edit && arm.stabilize, help,
                                   [&c, value, action](int way) { c.debug({action, false, std::clamp(std::round(value / 5.f) * 5.f + 5.f * way, 0.f, fp::strength_limit)}); });
            row.why_not = "Turn True first person on first.";
        };
        percent("Smoothing", arm.smoothing, DebugAction::set_first_person_smoothing, "How slowly the view follows the head. 0% follows directly.");
        percent("Head nod", arm.head_pitch, DebugAction::set_first_person_head_pitch, "The share of the head's up and down movement kept.");
        percent("Head tilt", arm.head_roll, DebugAction::set_first_person_head_roll, "The share of the head's horizon roll kept.");
        percent("Head bob", arm.bob, DebugAction::set_first_person_bob, "The share of the head's bob kept.");
        auto &flips = opt_toggle(o, "Follow flips", arm.follow_flips, can_edit && arm.stabilize, "Turn the view with the skater during flips and bails.",
                                 [&c](bool on) { c.debug({DebugAction::set_first_person_follow_flips, on}); });
        flips.why_not = "Turn True first person on first.";
    }

    opt_heading(o, "SPRING ARM");
    {
        opt_toggle(o, "Spring", arm.enabled, can_edit, "Let the camera lag behind and soften animated head movement.",
                   [&c](bool on) { c.debug({DebugAction::set_first_person_spring, on}); });
        const auto slider = [&](const char *name, float value, float limit, float step, DebugAction action, const char *unit, const char *help) {
            char text[32];
            std::snprintf(text, sizeof(text), step < .1f ? "%.2f%s" : "%.0f%s", value, unit);
            opt_number(o, name, text, (value + limit) / (2 * limit), can_edit, help,
                       [&c, value, limit, step, action](int way) { c.debug({action, false, std::clamp(value + step * way, -limit, limit)}); });
        };
        slider("Offset right", arm.offset[0], fp::offset_limit, .02f, DebugAction::set_first_person_offset_x, " m", "The camera moved right of the head, in metres.");
        slider("Offset up", arm.offset[1], fp::offset_limit, .02f, DebugAction::set_first_person_offset_y, " m", "The camera moved up from the head, in metres.");
        slider("Offset forward", arm.offset[2], fp::offset_limit, .02f, DebugAction::set_first_person_offset_z, " m", "The camera moved forward of the head, in metres.");
        slider("Pitch", arm.rotation[0], fp::rotation_limit, 1.f, DebugAction::set_first_person_pitch, " DEG", "Turns the view down (or up) from the head. More looks further down at the board.");
        slider("Yaw", arm.rotation[1], fp::rotation_limit, 1.f, DebugAction::set_first_person_yaw, " DEG", "Turns the view left or right from the head.");
        slider("Roll", arm.rotation[2], fp::rotation_limit, 1.f, DebugAction::set_first_person_roll, " DEG", "Rolls the view from the head.");
        opt_action(o, "Reset the spring arm", "RESET", can_edit, "Every spring arm setting back to its default.",
                   [&c] { c.debug({DebugAction::reset_first_person_arm}); });
    }
}

void style_rows(Opts &o, OptionsContext &c) {
    const auto &mp = c.m.multiplayer;
    const bool listed = !mp.identity_animation.empty();
    if (!mp.identity_tag.empty()) {
        opt_heading(o, "YOUR TAG");
        opt_info(o, "Your tag", mp.identity_tag, "As chat and your nametag show it, in your list's color.");
        opt_toggle(o, "Show my tag", mp.identity_tag_shown, true,
                   "Your tag and the color of your name, in chat and on your nametag. Off: your name looks like any other player's.",
                   [&c](bool on) { c.online("mark-tag", on ? "on" : "off"); });
        if (mp.identity_tag_lists.size() > 1) {
            int at = 0;
            for (std::size_t i = 0; i < mp.identity_tag_lists.size(); ++i)
                if (mp.identity_tag_lists[i].list == mp.identity_tag_list) at = static_cast<int>(i);
            auto &which = opt_choice(o, "Tag", mp.identity_tag_lists[static_cast<std::size_t>(at)].tag, mp.identity_tag_shown,
                                     "Which of your tags shows. Your items keep the colors they have.", [&c, at](int way) {
                                         const auto &lists = c.m.multiplayer.identity_tag_lists;
                                         c.online("mark-tag-list", lists[static_cast<std::size_t>(wrap_step(at, static_cast<int>(lists.size()), way))].list);
                                     });
            which.why_not = "Turn Show my tag on first.";
        }
    }

    opt_heading(o, "ITEM COLORS");
    const bool shown = mp.identity_items_shown;
    opt_toggle(o, "Color my items", shown, true,
               listed ? "Your colors on whatever you wear, each item on its own. Everyone you skate with sees them."
                      : "A solid color of your own on anything you wear. Everyone you skate with on this version of ReSkate or newer sees it.",
               [&c](bool on) { c.online("mark-items", on ? "on" : "off"); });
    // The modes in the order shown: 0 what the list gives, 1 off, 2 a gradient, 3 one colour, 4 the rainbow.
    std::vector<int> modes{1, 3};
    std::vector<std::string> names{"OFF", "SOLID"};
    if (listed) {
        modes = {0, 2, 3, 1}, names = {mp.identity_animation, "GRADIENT", "SOLID", "OFF"};
        if (mp.identity_rainbow) modes.insert(modes.begin() + 1, 4), names.insert(names.begin() + 1, "RAINBOW");
    }
    struct Swatch {
        const char *name;
        unsigned rgb;
    };
    static constexpr std::array<Swatch, 18> swatches{{{"White", 0xffffff}, {"Light grey", 0xc0c0c0}, {"Grey", 0x808080}, {"Charcoal", 0x404040},
                                                      {"Black", 0x101010}, {"Red", 0xe02020}, {"Orange", 0xff8010}, {"Yellow", 0xffe030},
                                                      {"Lime", 0x9ce030}, {"Green", 0x20a040}, {"Teal", 0x20b0a0}, {"Sky blue", 0x50b8f0},
                                                      {"Blue", 0x2050e0}, {"Navy", 0x182460}, {"Purple", 0x8040e0}, {"Pink", 0xff60b0},
                                                      {"Brown", 0x805020}, {"Tan", 0xd2b48c}}};
    const auto hex_of = [](const std::array<float, 3> &colour) {
        unsigned value{};
        for (const float part : colour) value = value << 8 | static_cast<unsigned>(std::clamp(part, 0.f, 1.f) * 255.f + .5f);
        return value;
    };
    const auto swatch_at = [&](unsigned rgb) {
        for (std::size_t i = 0; i < swatches.size(); ++i)
            if (swatches[i].rgb == rgb) return static_cast<int>(i);
        return -1;
    };
    for (std::size_t item = 0; item < mp.identity_styles.size(); ++item) {
        const auto &style = mp.identity_styles[item];
        const unsigned from = hex_of(style.from), to = hex_of(style.to);
        const auto set = [&c, item](int mode, unsigned a, unsigned b, int speed) {
            char text[64];
            std::snprintf(text, sizeof(text), "%zu %d %06x %06x %d", item, mode, a, b, speed);
            c.online("mark-style", text);
        };
        std::string title = style.name;
        for (auto &ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        int at = static_cast<int>(std::find(modes.begin(), modes.end(), style.mode) - modes.begin());
        if (at >= static_cast<int>(modes.size())) at = 0;
        auto &mode = opt_choice(o, title, names[static_cast<std::size_t>(at)], shown,
                                listed ? "What colors whatever you have on in this slot: your tag's colors, colors you pick, or none."
                                       : "A color you pick for whatever you have on in this slot, or its own.",
                                [set, modes, at, from, to, speed = style.speed](int way) {
                                    set(modes[static_cast<std::size_t>(wrap_step(at, static_cast<int>(modes.size()), way))], from, to, speed);
                                });
        mode.why_not = "Turn Color my items on first.";
        if (style.mode == 2 || style.mode == 3) {
            const bool gradient = style.mode == 2;
            for (int end = 0; end < (gradient ? 2 : 1); ++end) {
                const unsigned rgb = end ? to : from;
                const int found = swatch_at(rgb);
                auto &colour = opt_choice(o, title + (gradient ? (end ? " second color" : " first color") : " color"), found >= 0 ? swatches[static_cast<std::size_t>(found)].name : "Custom",
                                          shown, "Left and right step through the colors. Any color: the Insert menu's Skater > Style picker.",
                                          [set, found, end, from, to, mode_now = style.mode, speed = style.speed](int way) {
                                              const auto next = swatches[static_cast<std::size_t>(wrap_step(found, static_cast<int>(swatches.size()), way))].rgb;
                                              set(mode_now, end ? from : next, end ? next : to, speed);
                                          });
                colour.why_not = "Turn Color my items on first.";
            }
        }
        if (style.mode == 0 || style.mode == 2 || style.mode == 4) {
            static constexpr std::array<const char *, 3> speeds{"NORMAL", "SLOW", "FAST"};
            const int speed = std::clamp(style.speed, 0, 2);
            auto &row = opt_choice(o, title + " speed", speeds[static_cast<std::size_t>(speed)], shown, "How fast it moves through its colors.",
                                   [set, speed, mode_now = style.mode, from, to](int way) { set(mode_now, from, to, wrap_step(speed, 3, way)); });
            row.why_not = "Turn Color my items on first.";
        }
    }
}

void online_rows(Opts &o, OptionsContext &c) {
    const auto &mp = c.m.multiplayer;
    if (c.m.steam_offline) {
        opt_heading(o, "OTHER SKATERS");
        opt_info(o, "Multiplayer", "OFF", "Multiplayer is off in offline mode.");
        return;
    }
    const auto onoff = [&c](const char *action) { return [&c, action](bool on) { c.online(action, on ? "on" : "off"); }; };
    opt_heading(o, "OTHER SKATERS");
    {
        constexpr std::array<float, 8> shown{50.f, 75.f, 100.f, 150.f, 200.f, 300.f, 500.f, 1000.f};
        auto &distance = opt_number(o, "Player distance", mp.player_distance >= player_distance_unlimited ? std::string("EVERYONE") : whole(mp.player_distance, " M"),
                                    fraction_of(mp.player_distance, shown), true,
                                    "How far away other players are still shown as skaters. Past it they keep their nametag or dot. "
                                    "Lower it on a busy server for more frames and less memory.",
                                    [&c, value = mp.player_distance, shown](int way) { c.online("player-distance", whole(stepped(value, shown, way))); });
        distance.reset = [&c] { c.online("player-distance", whole(player_distance_unlimited)); };
        distance.reset_does = "Everyone";
        opt_toggle(o, "Direct connections", mp.prefer_direct, true,
                   "Connect straight to dedicated servers that offer it, for the lowest ping. The server can then see your IP address; other players never can. "
                   "Applies from the next server you join.",
                   onoff("direct-connections"));
        if (dingosdk::discord_presence::available())
            opt_toggle(o, "Discord status", dingosdk::discord_presence::enabled(), true,
                       "Show on your Discord profile where you are skating: the map, the server or lobby and how many are in it.",
                       [](bool on) { dingosdk::discord_presence::set_enabled(on); });
    }
    opt_heading(o, "NAMETAGS");
    {
        constexpr std::array<float, 8> tags{10.f, 25.f, 50.f, 75.f, 120.f, 200.f, 300.f, 500.f};
        opt_toggle(o, "Player nametags", mp.nametags, true,
                   "The name above each skater, with their distance: purple for ReSkate developers, red for content creators, gold for homies, "
                   "pink for server admins, blue for the host, green for your Steam friends.",
                   onoff("nametags"));
        auto &distance = opt_number(o, "Nametag distance", whole(mp.nametag_distance, " M"), fraction_of(mp.nametag_distance, tags), mp.nametags,
                                    "How far away a player's name still shows. Past it they are a dot.",
                                    [&c, value = mp.nametag_distance, tags](int way) { c.online("nametag-distance", whole(stepped(value, tags, way))); });
        distance.why_not = "Turn nametags on first.";
        auto &dots = opt_toggle(o, "Nametag dots", mp.nametag_dots, mp.nametags, "Players past the nametag distance, and off screen, as dots.", onoff("nametag-dots"));
        dots.why_not = "Turn nametags on first.";
        auto &friends = opt_toggle(o, "Friends' nametags only", mp.nametags_friends, mp.nametags,
                                   "Only your Steam friends have a name or dot. Chat bubbles still show for everyone.", onoff("nametags-friends"));
        friends.why_not = "Turn nametags on first.";
    }
    opt_heading(o, "CHAT");
    {
        opt_toggle(o, "Text chat", mp.chat_visible, true, "Session chat in the bottom-right corner; T opens it. Off: nothing shows and T does nothing.", onoff("chat-visible"));
        auto &filter = opt_toggle(o, "Chat filter", mp.chat_filter, mp.chat_visible, "Show bad words in chat messages and names as ****.", onoff("chat-filter"));
        filter.why_not = "Turn text chat on first.";
        opt_toggle(o, "Chat bubbles", mp.chat_bubbles, true, "Each player's newest chat line in a bubble above their skater.", onoff("chat-bubbles"));
        const std::string bubbles_off = "Turn chat bubbles on first.";
        auto &own = opt_toggle(o, "Own chat bubbles", mp.chat_bubbles_own, mp.chat_bubbles, "Your own messages above your skater too.", onoff("chat-bubbles-own"));
        own.why_not = bubbles_off;
        constexpr std::array<float, 7> reach{5.f, 10.f, 20.f, 40.f, 75.f, 120.f, 200.f};
        constexpr std::array<float, 6> seconds{1.f, 3.f, 5.f, 8.f, 15.f, 30.f};
        constexpr std::array<float, 8> lines{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
        auto &reach_row = opt_number(o, "Bubble distance", whole(mp.chat_bubbles_distance, " M"), fraction_of(mp.chat_bubbles_distance, reach), mp.chat_bubbles,
                                     "How far away a player can be and still show a chat bubble.",
                                     [&c, value = mp.chat_bubbles_distance, reach](int way) { c.online("chat-bubbles-distance", whole(stepped(value, reach, way))); });
        reach_row.why_not = bubbles_off;
        auto &time_row = opt_number(o, "Bubble time", whole(mp.chat_bubbles_duration, " S"), fraction_of(mp.chat_bubbles_duration, seconds), mp.chat_bubbles,
                                    "How many seconds a chat bubble stays before it fades.",
                                    [&c, value = mp.chat_bubbles_duration, seconds](int way) { c.online("chat-bubbles-duration", whole(stepped(value, seconds, way))); });
        time_row.why_not = bubbles_off;
        const float history = static_cast<float>(mp.chat_bubbles_history);
        auto &lines_row = opt_number(o, "Bubble lines", whole(history), fraction_of(history, lines), mp.chat_bubbles,
                                     "How many recent messages stack above each skater.",
                                     [&c, history, lines](int way) { c.online("chat-bubbles-history", whole(stepped(history, lines, way))); });
        lines_row.why_not = bubbles_off;
    }
}

// SETTINGS > BUTTONS: a row a bind, A records the next button, combination or key (for that long the
// game does not see them), Y clears it; plugins' binds under their plugin's name.
void settings_rows(Opts &o, OptionsContext &c) {
    auto &h = c.h;
    const auto &m = c.m;
    const auto &binds = m.bindings;
    const bool available = binds.available && c.console;
    constexpr int plugin_bind_row = 1000;
    const auto plugin_binds = plugin_ui::latest_binds();
    const auto save = [&c, plugin_binds](int action, std::uint32_t combo) {
        if (action >= plugin_bind_row) {
            const auto index = static_cast<std::size_t>(action - plugin_bind_row);
            if (index < plugin_binds->size()) plugin_ui::rebind({(*plugin_binds)[index].plugin, (*plugin_binds)[index].action, combo});
            say(c.h, combo ? "Saved." : "Cleared. The plugin's action has no bind now.");
            return;
        }
        const char *name = action >= 10 ? action_binds[static_cast<std::size_t>(action - 10)].name.data()
                         : action == 1 ? "freecamcontroller" : action == 2 ? "freecam" : action == 3 ? "noclip" : action == 4 ? "forwardvelocity"
                         : action == 5 ? "upvelocity" : action == 6 ? "tptofreecam" : action == 8 ? "voteyes" : action == 9 ? "voteno" : "offboardupvelocity";
        c.run(std::string("bind ") + name + " " + std::to_string(combo));
    };
    if (h.recording_bind) {
        ControllerInput controller;
        DingoSDKOverlayReadControllerInput(&controller, true);
        if (!available || GetTickCount64() >= h.recording_until || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            h.recording_bind = 0, set_typing(h, 0);
            say(h, "Recording cancelled. Your binding is unchanged.");
        } else if (const auto combo = h.bind_capture.update(controller, true)) {
            const int action = std::exchange(h.recording_bind, 0);
            set_typing(h, 0);
            save(action, *combo);
        }
    }
    ControllerInput pad;
    DingoSDKOverlayReadControllerInput(&pad, true);
    const auto style = pad.style;
    const auto bind_row = [&](int action, std::string name, std::uint32_t combo) {
        const bool mine = h.recording_bind == action;
        Opt row;
        row.kind = Opt::Kind::bind;
        row.name = std::move(name);
        row.value = mine ? (h.bind_capture.ready ? std::string("PRESS IT NOW...") : std::string("LET GO FIRST...")) : combo ? controller_combo_label(combo, style) : std::string("NONE");
        row.enabled = available && (!h.recording_bind || mine);
        row.help = mine ? "Hold up to five buttons or keys together, then let go. Nothing for 30 seconds, or Escape, cancels."
                        : "A records a new bind for it: a button, a combination of buttons, or a key.";
        row.why_not = h.recording_bind ? "Another bind is being recorded." : "Waiting for the local profile.";
        row.change = [&h, action](int) {
            if (h.recording_bind) return;
            h.recording_bind = action;
            h.bind_capture = {};
            h.recording_until = GetTickCount64() + 30000;
            h.notice.clear();
            set_typing(h, hash("bind-recording"));
        };
        if (combo) row.reset = [save, action] { save(action, 0); }, row.reset_does = "Clear it";
        o.push_back(std::move(row));
    };
    const auto action_bind = [](std::string_view key) {
        for (std::size_t i = 0; i < action_binds.size(); ++i)
            if (action_binds[i].key == key) return 10 + static_cast<int>(i);
        return 0;
    };
    const auto combo_of = [&binds](int action) -> std::uint32_t {
        switch (action) {
        case 1: return binds.freecam_controller_combo;
        case 2: return binds.freecam_combo;
        case 3: return binds.noclip_combo;
        case 4: return binds.forward_velocity_combo;
        case 5: return binds.up_velocity_combo;
        case 6: return binds.tp_to_freecam_combo;
        case 7: return binds.offboard_up_velocity_combo;
        case 8: return binds.vote_yes_combo;
        case 9: return binds.vote_no_combo;
        default: return action >= 10 ? binds.action_combos[static_cast<std::size_t>(action - 10)] : 0;
        }
    };
    const auto name_of = [](int action) -> std::string {
        switch (action) {
        case 1: return "Freecam controller";
        case 2: return "Freecam";
        case 3: return "Noclip";
        case 4: return "Forward boost";
        case 5: return "Up boost";
        case 6: return "Teleport to the freecam";
        case 7: return "Off-board up boost";
        case 8: return "Vote yes";
        case 9: return "Vote no";
        default: return action >= 10 ? std::string(action_binds[static_cast<std::size_t>(action - 10)].label) : std::string();
        }
    };
    struct Group {
        const char *title;
        std::vector<int> actions;
    };
    const std::vector<Group> groups{
        {"BUTTONS: GENERAL", {action_bind("reskate_menu"), action_bind("hide_hud"), action_bind("challenges"), action_bind("time_of_day")}},
        {"BUTTONS: CAMERA", {2, 1, 6, action_bind("first_person")}},
        {"BUTTONS: CHEATS AND BOOSTS", {3, action_bind("no_bail"), 4, 5, 7}},
        {"BUTTONS: ONLINE", {action_bind("voice_chat"), action_bind("nametags"), action_bind("nametag_dots"), 8, 9}},
    };
    for (const auto &group : groups) {
        if (std::string_view(group.title) == "BUTTONS: ONLINE" && m.steam_offline) continue;
        opt_heading(o, group.title);
        for (const int action : group.actions)
            if (action) bind_row(action, name_of(action), combo_of(action));
    }
    if (!plugin_binds->empty()) {
        std::map<std::string, std::string> titles;
        for (const auto &plugin : plugins::installed())
            if (!plugin.title.empty()) titles[plugin.id] = plugin.title;
        std::string current;
        for (std::size_t i = 0; i < plugin_binds->size(); ++i) {
            const auto &bind = (*plugin_binds)[i];
            if (bind.plugin != current) {
                current = bind.plugin;
                const auto title = titles.find(bind.plugin);
                std::string heading = "BUTTONS: " + (title != titles.end() ? title->second : bind.plugin);
                for (auto &ch : heading) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                opt_heading(o, heading);
            }
            bind_row(plugin_bind_row + static_cast<int>(i), bind.action, bind.combo);
        }
        const bool changed = std::any_of(plugin_binds->begin(), plugin_binds->end(), [](const auto &b) { return b.combo != b.own; });
        opt_action(o, "Plugins' own binds", "RESTORE", changed && !h.recording_bind, "Every plugin's action back on the bind the plugin gave it.", [&h, plugin_binds] {
            for (const auto &bind : *plugin_binds) plugin_ui::rebind({bind.plugin, bind.action, std::nullopt});
            say(h, "Every plugin's action is on the plugin's own bind again.");
        });
    }

    opt_heading(o, "SCREEN");
    {
        const auto &d = m.debug;
        auto &hide = opt_toggle(o, "Hide game UI", d.game_ui_hidden, d.available && d.ui_available && c.debugging,
                                "Keep the view clear for riding and captures. The ReSkate menu's key always brings it back.",
                                [&c](bool on) { c.debug({DebugAction::set_game_ui_hidden, on}); });
        hide.why_not = "Not available right now.";
        const bool hub_layout = m.menu_layout == menu_layout_hub;
        opt_choice(o, "Insert menu layout", hub_layout ? "WINDOW" : "SIDE SHEET", c.console,
                   hub_layout ? "The Insert menu as a window in the middle of the screen, its pages down its left side."
                              : "The Insert menu docked to the right edge, the game in view beside it.",
                   [&c, hub_layout](int) { c.run(hub_layout ? "ui layout sheet" : "ui layout hub"); });
        constexpr std::array<float, 9> sizes{.75f, .85f, 1.f, 1.1f, 1.25f, 1.4f, 1.6f, 1.8f, 2.f};
        auto &size = opt_number(o, "Insert menu size", one_decimal(m.menu_scale) + "x", fraction_of(m.menu_scale, sizes), c.console,
                                "Scales the Insert menu and its text in both layouts.",
                                [&c, value = m.menu_scale, sizes](int way) {
                                    const float next = std::clamp(stepped(value, sizes, way), min_menu_scale, max_menu_scale);
                                    c.run("ui scale " + std::to_string(next));
                                });
        size.reset = [&c] { c.run("ui scale " + std::to_string(default_menu_scale)); };
        size.reset_does = "Its default size";
    }

    opt_heading(o, "REPLAYS AND MUSIC");
    {
        const auto &k = m.keyframe_smoothing;
        opt_toggle(o, "Keyframe smoothing", k.enabled, k.available && c.console,
                   "In the replay editor the free camera glides through its keys instead of changing speed at each one. Every key is still met, at its time.",
                   [&c](bool on) { c.run(on ? "keysmooth on" : "keysmooth off"); });
        const int percent = keyframe_smoothness_percent(k);
        auto &smooth = opt_number(o, "Keyframe smoothness", std::to_string(percent) + "%", static_cast<float>(percent) / 100.f, k.available && k.enabled && c.console,
                                  "100% is the smoothest that still meets every key; 0% the game's own curve.",
                                  [&c, percent](int way) { c.run("keysmooth amount " + std::to_string(std::clamp(percent + 10 * way, 0, 100) / 100.0)); });
        smooth.why_not = "Turn Keyframe smoothing on first.";
        static constexpr std::array<const char *, 5> qualities{"GAME'S OWN", "LOW", "MEDIUM", "HIGH", "VERY HIGH"};
        const int quality = std::clamp(m.replay_export_quality, 0, static_cast<int>(qualities.size()) - 1);
        opt_choice(o, "Replay export quality", qualities[static_cast<std::size_t>(quality)], c.console,
                   "The MP4 export's bitrate. At 1080p60: Low about 6 Mbit/s, Medium 12, High 25, Very high 45. Higher is sharper in fast motion, and a bigger file.",
                   [&c, quality](int way) {
                       const int next = wrap_step(quality, static_cast<int>(replay_export::quality_count), way);
                       c.run("replay quality " + std::string(replay_export::quality_names[static_cast<std::size_t>(next)]));
                   });
        const bool shuffle = dingosdk::profile_runtime::music_shuffle_enabled();
        opt_toggle(o, "Shuffle playlists", shuffle, dingosdk::profile_runtime::music_playback_available(), "Off plays songs in playlist order. On shuffles.",
                   [](bool on) {
                       dingosdk::profile_runtime::set_music_shuffle_enabled(on);
                       dingosdk::profile_runtime::set_local_preference("MusicShuffle", on);
                   });
    }

    opt_heading(o, "PERFORMANCE");
    {
        opt_toggle(o, "Performance HUD", dingosdk::profiler::hud(), c.console, "Client update and frame timing, ReSkate's cost and CPU per thread.",
                   [&c](bool on) { c.run(on ? "perf hud on" : "perf hud off"); });
        opt_toggle(o, "Profiler window", dingosdk::profiler::window(), true, "Zones, threads and a stack sampler. Shows while the Insert menu or the console is open.",
                   [](bool on) { dingosdk::profiler::set_window(on); });
    }
}
