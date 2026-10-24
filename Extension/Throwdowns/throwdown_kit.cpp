#include "throwdown_kit.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <mutex>

// The kit's store (throwdown_kit.h): every plugin's documents, parsed from what it publishes with
// tolerant readers (a missing or mistyped field is its default, never an error, so a plugin's
// typo blanks a label instead of its whole HUD), bounded so no document grows past what the
// native widgets show; and the events the native UI queues for each plugin. kit::flag lives with
// the native placement (native_flag_session.cpp).
namespace dingosdk::throwdowns::kit {
namespace {
constexpr std::size_t most_text = 256, most_long_text = 1024, most_rows = 32, most_events = 64, most_queues = 64, most_args = 16;
constexpr std::size_t most_arg = 70u << 10;       // a dialog answer carries up to 64 KB of a file
constexpr std::size_t most_document = 512u << 10; // a publication, as JSON text

// ---- JSON, read tolerantly -----------------------------------------------------------------------
const Json* field(const Json& j, std::string_view key) {
    if (!j.is_object() || !j.contains(key)) return nullptr;
    const auto& value = j.at(key);
    return value.is_null() ? nullptr : &value;
}
template<class T> T number(const Json& j, std::string_view key, T fallback = {}) {
    const auto* v = field(j, key);
    if (!v) return fallback;
    if (v->is_number()) {
        const double d = v->get<double>();
        if (!std::isfinite(d)) return fallback;
        if constexpr (std::is_integral_v<T>) {
            if (d < static_cast<double>(std::numeric_limits<T>::lowest()) || d > static_cast<double>(std::numeric_limits<T>::max())) return fallback;
        }
        return static_cast<T>(d);
    }
    if (v->is_string() && std::is_integral_v<T>) {
        T out{};
        const auto& s = v->string();
        if (std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc{}) return out;
    }
    return fallback;
}
// Steam IDs come as strings (a Lua number cannot hold one exactly), or numbers.
std::uint64_t id_of(const Json& v) {
    if (v.is_string()) {
        std::uint64_t out{};
        const auto& s = v.string();
        return std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc{} ? out : 0;
    }
    if (!v.is_number()) return 0;
    const double d = v.get<double>();
    return std::isfinite(d) && d > 0 && d < 1.8e19 ? static_cast<std::uint64_t>(d) : 0;
}
std::uint64_t id(const Json& j, std::string_view key) {
    const auto* v = field(j, key);
    return v ? id_of(*v) : 0;
}
bool flag(const Json& j, std::string_view key, bool fallback = false) {
    const auto* v = field(j, key);
    return v && v->is_boolean() ? v->get<bool>() : fallback;
}
// A label: a string, or a number as the plugin would print it (ranks and scores come as either).
std::string label_of(const Json& v, std::size_t most = most_text) {
    std::string s;
    if (v.is_string()) s = v.string();
    else if (v.is_number_integer()) s = v.is_number_unsigned() ? std::to_string(v.get<std::uint64_t>()) : std::to_string(v.get<std::int64_t>());
    else if (v.is_number()) {
        const double d = v.get<double>();
        if (std::isfinite(d)) s = std::format("{}", d);
    } else if (v.is_boolean()) s = v.get<bool>() ? "true" : "false";
    if (s.size() > most) s.resize(most);
    return s;
}
std::string text(const Json& j, std::string_view key, std::size_t most = most_text) {
    const auto* v = field(j, key);
    return v ? label_of(*v, most) : std::string{};
}
Vec3 vec(const Json& v) {
    Vec3 out{};
    if (v.is_array() && v.size() >= 3)
        for (std::size_t i = 0; i < 3; ++i) out[i] = v.at(i).is_number() ? static_cast<float>(v.at(i).get<double>()) : 0.f;
    else if (v.is_object()) out = {number<float>(v, "x"), number<float>(v, "y"), number<float>(v, "z")};
    for (auto& n : out)
        if (!std::isfinite(n) || std::fabs(n) > 1e6f) n = 0;
    return out;
}
Vec3 vec(const Json& j, std::string_view key) {
    const auto* v = field(j, key);
    return v ? vec(*v) : Vec3{};
}
template<class T, class F> std::vector<T> list(const Json& j, std::string_view key, std::size_t most, F&& each) {
    std::vector<T> out;
    const auto* v = field(j, key);
    if (!v || !v->is_array()) return out;
    for (std::size_t i = 0; i < v->size() && out.size() < most; ++i) out.push_back(each(v->at(i)));
    return out;
}
std::vector<std::string> texts(const Json& j, std::string_view key, std::size_t most) {
    return list<std::string>(j, key, most, [](const Json& v) { return label_of(v); });
}
std::uint32_t colour(const Json& j, std::string_view key, std::uint32_t fallback) {
    const auto* v = field(j, key);
    if (!v || !v->is_number()) return fallback;
    const double d = v->get<double>();
    return std::isfinite(d) && d >= 0 && d <= 4294967295.0 ? static_cast<std::uint32_t>(d) : fallback;
}
std::uint32_t ms(const Json& j, std::string_view key) {
    const double d = number<double>(j, key);
    return d > 0 ? static_cast<std::uint32_t>(std::min(d, 4294967295.0)) : 0;
}

// ---- documents -----------------------------------------------------------------------------------
// A document's `mode` is the plugin's own id; the store keeps the SDK's ("<plugin>/<id>").
std::string mode_key(std::string_view plugin, const Json& j) {
    auto own = text(j, "mode", 64);
    if (own.empty()) return {};
    const std::string prefix = std::string(plugin) + "/";
    return own.starts_with(prefix) ? own : prefix + own;
}
SetupSlider slider_of(const Json* j) {
    SetupSlider s;
    if (!j || !j->is_object()) return s;
    s.present = true;
    s.caption = text(*j, "caption", 64);
    s.min = number<float>(*j, "min");
    s.max = number<float>(*j, "max", s.min);
    s.step = std::max(number<float>(*j, "step", 1), 0.0f);
    s.value = std::clamp(number<float>(*j, "value", s.min), std::min(s.min, s.max), std::max(s.min, s.max));
    return s;
}
SetupRow setup_row_of(const Json& j) {
    SetupRow r;
    r.id = text(j, "id", 64);
    r.kind = text(j, "kind", 16);
    if (r.kind.empty()) r.kind = "slider";
    r.caption = text(j, "caption", 64);
    r.label = text(j, "label", 64);
    r.min = number<float>(j, "min");
    r.max = number<float>(j, "max", r.min);
    r.step = std::max(number<float>(j, "step", 1), 0.0f);
    r.options = texts(j, "options", 16);
    if (r.kind == "choice") {
        // From 1 as Lua counts.
        const int count = static_cast<int>(r.options.size());
        r.selected = count ? std::clamp(number<int>(j, "value", 1) - 1, 0, count - 1) : 0;
        r.value = static_cast<float>(r.selected);
    } else r.value = number<float>(j, "value", r.min);
    return r;
}
Setup setup_of(std::string_view plugin, const Json& j) {
    Setup s;
    s.mode = mode_key(plugin, j);
    s.title = text(j, "title", 64);
    s.art = text(j, "art");
    s.description = text(j, "description", most_long_text);
    s.back = text(j, "back", 32);
    s.players = slider_of(field(j, "players"));
    s.timer = slider_of(field(j, "timer"));
    s.rows = list<SetupRow>(j, "rows", 12, setup_row_of);
    if (field(j, "hide_native")) s.hide_native = texts(j, "hide_native", 8);
    return s;
}
Waiting waiting_of(std::string_view plugin, const Json& j) {
    Waiting w;
    w.mode = mode_key(plugin, j);
    w.title = text(j, "title", 64);
    w.art = text(j, "art");
    w.status = text(j, "status");
    w.enrolled = number<unsigned>(j, "enrolled");
    w.capacity = number<unsigned>(j, "capacity");
    w.can_start = flag(j, "can_start");
    if (const auto* c = field(j, "countdown"); c && c->is_object()) {
        w.countdown = true;
        w.remaining_ms = ms(*c, "remaining_ms");
        w.total_ms = ms(*c, "total_ms");
    }
    return w;
}
// {remaining_ms, total_ms, sampled_ms, ticking, up}, or a stopwatch {base_ms, sampled_ms, running,
// cap_ms} (an elapsed time counting up).
Clock clock_of(const Json* j) {
    Clock c;
    if (!j || !j->is_object()) return c;
    c.present = true;
    c.sampled_ms = number<std::uint64_t>(*j, "sampled_ms");
    if (field(*j, "base_ms")) {
        c.up = true;
        c.remaining_ms = ms(*j, "base_ms");
        c.total_ms = ms(*j, "cap_ms");
        c.ticking = flag(*j, "running");
        return c;
    }
    c.remaining_ms = ms(*j, "remaining_ms");
    c.total_ms = ms(*j, "total_ms");
    c.ticking = flag(*j, "ticking", true);
    c.up = flag(*j, "up");
    return c;
}
BoardRow board_row_of(const Json& j) {
    BoardRow r;
    r.rank = text(j, "rank", 16);
    r.name = text(j, "name", 64);
    r.value = text(j, "value", 64);
    r.color = colour(j, "color", 0xffffffffU);
    r.self = flag(j, "self");
    r.out = flag(j, "out");
    r.marks = list<Mark>(j, "marks", 8, [](const Json& m) { return Mark{text(m, "text", 16), flag(m, "on")}; });
    r.clock = clock_of(field(j, "clock"));
    return r;
}
Prompt prompt_of(const Json& j) {
    Prompt p;
    p.inputs = texts(j, "inputs", 4);
    if (const auto input = text(j, "input", 64); !input.empty() && p.inputs.size() < 4) p.inputs.push_back(input);
    p.caption = text(j, "caption", 64);
    p.glyph = text(j, "glyph", 64);
    p.hold = std::clamp(number<float>(j, "hold"), 0.0f, 1.0f);
    return p;
}
Hud::Text banner_of(const Json* j) {
    Hud::Text t;
    if (!j || !j->is_object()) return t;
    t.text = text(*j, "title");
    if (t.text.empty()) t.text = text(*j, "text");
    t.style = text(*j, "style", 32);
    t.subtitle = text(*j, "subtitle");
    t.art = text(*j, "art");
    t.serial = number<std::uint64_t>(*j, "serial");
    t.ms = ms(*j, "ms");
    return t;
}
Hud hud_of(const Json& j) {
    Hud h;
    h.active = flag(j, "active", true);
    h.theme = text(j, "theme", 32);
    if (const auto* header = field(j, "header")) {
        h.header.title = text(*header, "title", 64);
        h.header.art = text(*header, "art");
        h.header.status = text(*header, "status");
        const auto offset = vec(*header, "offset"); // {x, y} or {x, y, 0}
        if (const auto* o = field(*header, "offset"); o && o->is_array() && o->size() == 2)
            h.header.offset = {o->at(0).is_number() ? static_cast<float>(o->at(0).get<double>()) : 0.f,
                               o->at(1).is_number() ? static_cast<float>(o->at(1).get<double>()) : 0.f};
        else h.header.offset = {offset[0], offset[1]};
        for (auto& n : h.header.offset)
            if (!std::isfinite(n) || std::fabs(n) > 1e4f) n = 0;
        h.header.icon_size = std::min(number<unsigned>(*header, "icon_size"), 512u);
        h.header.frame_width = std::min(number<unsigned>(*header, "frame_width"), 2048u);
    }
    h.layout = text(j, "layout", 16);
    h.counters = list<std::string>(j, "counters", 8, [](const Json& c) { return c.is_object() ? text(c, "text") : label_of(c); });
    h.score_width = std::clamp(number<float>(j, "score_width", 480), 0.0f, 4096.0f);
    h.score_art = text(j, "score_art", 64);
    h.clock = clock_of(field(j, "clock"));
    if (const auto* b = field(j, "board"); b && b->is_object()) {
        h.board.present = true;
        h.board.title = text(*b, "title", 64);
        h.board.variant = text(*b, "variant", 32);
        h.board.style = text(*b, "style", 32);
        h.board.name_art = text(*b, "name_art", 64);
        h.board.out_art = text(*b, "out_art", 64);
        h.board.rank_art = text(*b, "rank_art", 64);
        h.board.value_art = text(*b, "value_art", 64);
        h.board.reserve = text(*b, "reserve", 16);
        h.board.value_width = std::clamp(number<float>(*b, "value_width", 0), 0.0f, 2048.0f);
        h.board.heading = text(*b, "heading", 16);
        h.board.columns = texts(*b, "columns", 8);
        h.board.rows = list<BoardRow>(*b, "rows", most_rows, board_row_of);
    }
    if (const auto* r = field(j, "results"); r && r->is_object()) {
        h.results.present = true;
        h.results.title = text(*r, "title", 64);
        h.results.heading_art = text(*r, "heading_art", 64);
        h.results.title_width = std::clamp(number<float>(*r, "title_width", 0), 0.0f, 2048.0f);
        h.results.notice = text(*r, "notice");
        h.results.caption = text(*r, "caption");
        h.results.style = text(*r, "style", 32);
        h.results.rows = list<BoardRow>(*r, "rows", most_rows, board_row_of);
    }
    if (const auto* i = field(j, "intro"); i && i->is_object()) {
        h.intro.present = true;
        h.intro.title = text(*i, "title", 64);
        h.intro.art_key = text(*i, "art_key");
        h.intro.rules = text(*i, "rules", most_long_text);
        h.intro.until_ms = number<std::uint64_t>(*i, "until_ms");
    }
    h.countdown = clock_of(field(j, "countdown"));
    h.banner = banner_of(field(j, "banner"));
    h.callout = banner_of(field(j, "callout"));
    h.warning = banner_of(field(j, "warning"));
    if (const auto* c = field(j, "card"); c && c->is_object()) {
        h.card.present = true;
        h.card.caption = text(*c, "caption", 64);
        h.card.title = text(*c, "title");
        h.card.image = text(*c, "image");
    }
    if (const auto* l = field(j, "list"); l && l->is_object()) {
        h.list.present = true;
        h.list.caption = text(*l, "caption", 64);
        h.list.items = texts(*l, "items", 16);
    }
    if (const auto* p = field(j, "position"); p && p->is_object()) {
        h.position.present = true;
        h.position.place = number<unsigned>(*p, "place");
        h.position.medal = text(*p, "medal", 16);
        h.position.art = text(*p, "art", 64);
    }
    h.timer_art = text(j, "timer_art", 64);
    h.timers = list<TimerText>(j, "timers", 6, [](const Json& t) {
        TimerText out;
        out.caption = text(t, "caption", 64);
        if (const auto* value = field(t, "text"); value && value->is_object()) out.clock = clock_of(value);
        else out.text = text(t, "text", 64);
        if (const auto* c = field(t, "clock"); c && c->is_object()) out.clock = clock_of(c);
        return out;
    });
    if (const auto* c = field(j, "compass"); c && c->is_object()) {
        h.compass.present = true;
        h.compass.at = vec(*c, "at");
        h.compass.art = text(*c, "art");
    }
    h.markers = list<Marker>(j, "markers", most_rows, [](const Json& m) { return Marker{vec(m, "at"), text(m, "text", 64), flag(m, "distance")}; });
    if (const auto* b = field(j, "blackout"); b && b->is_object()) {
        h.blackout.present = true;
        h.blackout.text = text(*b, "text");
    }
    if (const auto* e = field(j, "editor"); e && e->is_object()) {
        h.editor.present = true;
        h.editor.title = text(*e, "title", 64);
        h.editor.variant = text(*e, "variant", 16);
        h.editor.prompts = list<Prompt>(*e, "prompts", 12, prompt_of);
        if (const auto* info = field(*e, "info")) {
            h.editor.info_title = text(*info, "title", 64);
            h.editor.info = list<std::pair<std::string, std::string>>(*info, "rows", 12,
                [](const Json& r) { return std::pair{text(r, "caption", 64), text(r, "value", 64)}; });
        }
        if (const auto* toast = field(*e, "toast")) {
            h.editor.toast = text(*toast, "text");
            h.editor.toast_alpha = std::clamp(number<float>(*toast, "alpha", 1), 0.0f, 1.0f);
        }
        if (const auto* modal = field(*e, "modal")) {
            h.editor.modal = text(*modal, "text", most_long_text);
            h.editor.modal_actions = list<Prompt>(*modal, "actions", 4, prompt_of);
        }
    }
    return h;
}
Panel panel_of(const Json& j) {
    Panel p;
    p.present = true;
    p.title = text(j, "title", 64);
    p.description = text(j, "description", most_long_text);
    p.art = text(j, "art");
    p.score_caption = text(j, "score_caption", 32);
    p.quit = text(j, "quit", 32);
    p.settings = list<PanelSetting>(j, "settings", 3, [](const Json& s) { return PanelSetting{text(s, "icon"), text(s, "caption", 64), text(s, "value", 64)}; });
    // A prompt's command rides in its glyph (throwdown_kit.h).
    p.prompts = list<Prompt>(j, "prompts", 8, [](const Json& s) {
        auto prompt = prompt_of(s);
        prompt.glyph = text(s, "command", 64);
        return prompt;
    });
    p.players = list<PanelPlayer>(j, "players", most_rows, [](const Json& s) {
        return PanelPlayer{id(s, "id"), text(s, "name", 64), text(s, "value", 64), text(s, "rank", 16), text(s, "outcome", 64), text(s, "portrait"),
                           flag(s, "self")};
    });
    return p;
}
Flag flag_of(std::string_view plugin, const Json& j) {
    Flag f;
    f.id = text(j, "id", 64);
    f.mode = mode_key(plugin, j);
    f.leader = id(j, "leader");
    f.game = number<std::uint32_t>(j, "game");
    f.at = vec(j, "at");
    f.yaw = number<float>(j, "yaw");
    f.players = number<unsigned>(j, "players", 1);
    f.capacity = number<unsigned>(j, "capacity", 6);
    f.open = flag(j, "open", true);
    f.stats = list<std::pair<std::string, std::string>>(j, "stats", 3, [](const Json& s) { return std::pair{text(s, "title", 32), text(s, "value", 32)}; });
    return f;
}
Match match_of(const Json& j) {
    Match m;
    m.markers = flag(j, "markers");
    m.input_lock = flag(j, "input_lock");
    m.offboard = flag(j, "offboard");
    m.teleports = flag(j, "teleports");
    m.in_activity = flag(j, "in_activity");
    m.hidden = list<std::uint64_t>(j, "hidden", 16, id_of);
    std::erase(m.hidden, 0);
    return m;
}
bool asks(const Match& m) { return m.markers || m.input_lock || m.offboard || m.teleports || m.in_activity || !m.hidden.empty(); }

// ---- the store -----------------------------------------------------------------------------------
struct Entry {
    Documents documents;
    std::string text; // as last published: an unchanged publication is not parsed again
};
struct Store {
    std::mutex mutex;
    std::map<std::string, Entry, std::less<>> plugins;
    std::map<std::string, std::vector<Event>, std::less<>> events;
    std::string current;
    std::uint64_t revision{};
};
Store& store() {
    static auto* s = new Store; // never destroyed: the UI may read it during shutdown
    return *s;
}
std::string_view plugin_of(std::string_view mode) {
    const auto at = mode.find('/');
    return at == std::string_view::npos ? std::string_view{} : mode.substr(0, at);
}
void queue(Store& s, std::string_view plugin, Event event) {
    if (plugin.empty()) return;
    auto found = s.events.find(plugin);
    if (found == s.events.end()) {
        if (s.events.size() >= most_queues) return;
        found = s.events.emplace(std::string(plugin), std::vector<Event>{}).first;
    }
    if (event.args.size() > most_args) event.args.resize(most_args);
    for (auto& a : event.args)
        if (a.size() > most_arg) a.resize(most_arg);
    event.plugin = std::string(plugin);
    auto& list = found->second;
    if (list.size() >= most_events) list.erase(list.begin()); // the newest matter more
    list.push_back(std::move(event));
}
} // namespace

std::string publish(std::string_view plugin, std::string_view json_text) {
    if (json_text.size() > most_document) return "a throwdown document is at most 512 KB as JSON";
    try {
        return publish(plugin, Json::parse(json_text));
    } catch (const std::exception& e) {
        return std::string("not a throwdown document: ") + e.what();
    }
}
std::string publish(std::string_view plugin, const Json& j) {
    if (plugin.empty()) return "a document belongs to a plugin";
    if (!j.is_object()) return "a throwdown document is a table";
    auto dumped = j.dump();
    if (dumped.size() > most_document) return "a throwdown document is at most 512 KB as JSON";
    {
        auto& s = store();
        std::lock_guard lock(s.mutex);
        if (const auto found = s.plugins.find(plugin); found != s.plugins.end() && found->second.text == dumped) return {};
    }
    // Parsed outside the lock: the UI thread reads while the plugin thread publishes.
    Documents d;
    d.plugin = std::string(plugin);
    if (const auto* v = field(j, "setup"); v && v->is_object()) d.setup = setup_of(plugin, *v);
    if (const auto* v = field(j, "waiting"); v && v->is_object()) d.waiting = waiting_of(plugin, *v);
    if (const auto* v = field(j, "hud"); v && v->is_object()) d.hud = hud_of(*v);
    if (const auto* v = field(j, "panel"); v && v->is_object()) d.panel = panel_of(*v);
    d.flags = list<Flag>(j, "flags", most_rows, [&](const Json& f) { return flag_of(plugin, f); });
    if (const auto* v = field(j, "match"); v && v->is_object()) d.match = match_of(*v);
    auto& s = store();
    std::lock_guard lock(s.mutex);
    d.revision = ++s.revision;
    auto& entry = s.plugins[std::string(plugin)];
    entry.documents = std::move(d);
    entry.text = std::move(dumped);
    return {};
}
void release(std::string_view plugin) {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    if (const auto found = s.plugins.find(plugin); found != s.plugins.end()) s.plugins.erase(found);
    if (const auto found = s.events.find(plugin); found != s.events.end()) s.events.erase(found);
    if (plugin_of(s.current) == plugin) s.current.clear();
    ++s.revision;
}
std::vector<Documents> documents() {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    std::vector<Documents> out;
    out.reserve(s.plugins.size());
    for (const auto& [plugin, entry] : s.plugins) out.push_back(entry.documents);
    return out;
}
std::optional<Documents> documents_for(std::string_view mode) {
    const auto plugin = plugin_of(mode);
    if (plugin.empty()) return std::nullopt;
    auto& s = store();
    std::lock_guard lock(s.mutex);
    const auto found = s.plugins.find(plugin);
    if (found == s.plugins.end()) return std::nullopt;
    return found->second.documents;
}
std::optional<Documents> active() {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    for (const auto& [plugin, entry] : s.plugins)
        if (asks(entry.documents.match) || entry.documents.hud.active) return entry.documents;
    return std::nullopt;
}
Match match() noexcept {
    Match out;
    auto& s = store();
    std::lock_guard lock(s.mutex);
    for (const auto& [plugin, entry] : s.plugins) {
        const auto& m = entry.documents.match;
        out.markers |= m.markers;
        out.input_lock |= m.input_lock;
        out.offboard |= m.offboard;
        out.teleports |= m.teleports;
        out.in_activity |= m.in_activity;
        for (const auto who : m.hidden)
            if (std::find(out.hidden.begin(), out.hidden.end(), who) == out.hidden.end()) out.hidden.push_back(who);
    }
    return out;
}
bool allows_teleports(std::string_view plugin, bool flag_mine) noexcept {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    const auto entry = s.plugins.find(plugin);
    return entry != s.plugins.end() && entry->second.documents.match.teleports && (entry->second.documents.hud.active || flag_mine);
}
bool hidden(std::uint64_t player) noexcept {
    if (!player) return false;
    auto& s = store();
    std::lock_guard lock(s.mutex);
    for (const auto& [plugin, entry] : s.plugins) {
        const auto& h = entry.documents.match.hidden;
        if (std::find(h.begin(), h.end(), player) != h.end()) return true;
    }
    return false;
}

void post(std::string_view mode, std::string action, std::vector<std::string> args) {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    queue(s, plugin_of(mode), Event{{}, std::move(action), std::string(mode), std::move(args)});
}
void post_to(std::string_view plugin, std::string action, std::string mode, std::vector<std::string> args) {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    queue(s, plugin, Event{{}, std::move(action), std::move(mode), std::move(args)});
}
std::vector<Event> take(std::string_view plugin) {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    const auto found = s.events.find(plugin);
    if (found == s.events.end()) return {};
    auto out = std::move(found->second);
    s.events.erase(found);
    return out;
}

void set_current(std::string_view mode) {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    s.current = std::string(mode.substr(0, std::min<std::size_t>(mode.size(), 160)));
}
std::string current() {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    return s.current;
}
} // namespace dingosdk::throwdowns::kit
