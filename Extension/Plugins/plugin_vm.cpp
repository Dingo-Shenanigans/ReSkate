#include "plugin_vm.h"
#include "plugin_api.h"
#include "plugin_wire.h"
#include "plugin_bodies.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include <cstdlib>
#include <format>
#include <fstream>

namespace dingosdk::plugins {
namespace {
double elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}
// One log line: an error message can be as long as a script makes it.
std::string shortened(std::string_view text) {
    constexpr std::size_t most = 2000;
    return text.size() <= most ? std::string(text) : std::string(text.substr(0, most)) + " ...";
}
// The traceback is added where the error was raised, while its stack is still there.
int traceback(lua_State* L) {
    std::string text = lua_isstring(L, 1) ? lua_tostring(L, 1) : "an error that is not a string";
    if (const char* trace = lua_debugtrace(L); trace && *trace) {
        text += "\n";
        text += trace;
    }
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
}

std::string read_package_file(const std::filesystem::path& file, std::size_t most, std::string& error) {
    error.clear();
    std::error_code code;
    if (!std::filesystem::is_regular_file(file, code)) { error = "not found"; return {}; }
    const auto size = std::filesystem::file_size(file, code);
    if (code) { error = "cannot be read"; return {}; }
    if (size > most) { error = std::format("is larger than {} MB", most >> 20); return {}; }
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in || !in.read(text.data(), static_cast<std::streamsize>(size))) { error = "cannot be read"; return {}; }
    return text;
}

Plugin::Plugin(std::string id, std::filesystem::path directory, Manifest manifest, Services& services, Limits limits)
    : id_(std::move(id)), directory_(std::move(directory)), manifest_(std::move(manifest)), services_(services),
      limits_(limits) {
    const auto entry = std::filesystem::path(manifest_.entry);
    code_root_ = entry.has_parent_path() ? entry.parent_path().generic_string() : std::string{};
}

Plugin::~Plugin() { close(); }

void* Plugin::allocate(void* ud, void* ptr, std::size_t old_size, std::size_t new_size) {
    auto& self = *static_cast<Plugin*>(ud);
    if (!ptr) old_size = 0;
    if (new_size == 0) {
        std::free(ptr);
        self.memory_ -= old_size;
        return nullptr;
    }
    if (new_size > old_size && self.memory_ - old_size + new_size > self.limits_.memory) {
        self.memory_exceeded_ = true;
        return nullptr;
    }
    void* block = std::realloc(ptr, new_size);
    if (!block) return nullptr;
    self.memory_ = self.memory_ - old_size + new_size;
    return block;
}

// Called at Luau's safe points: loop back edges, calls and returns. Once a plugin is over a limit
// every safe point raises again, so a pcall in the script cannot catch it and carry on.
void Plugin::interrupt(lua_State* L, int gc) {
    if (gc >= 0) return; // inside the collector, where raising is not allowed
    auto& self = *static_cast<Plugin*>(lua_callbacks(L)->userdata);
    if (self.halt_ == Halt::none) {
        if (self.memory_exceeded_) self.halt_ = Halt::memory;
        else if (self.timed_ && (++self.interrupts_ & 63) == 0 && Clock::now() > self.deadline_) self.halt_ = Halt::time;
    }
    if (self.halt_ != Halt::none) luaL_error(L, "the plugin was stopped");
}

bool Plugin::load(std::string entry) {
    if (L_) return state_ == State::running;
    state_ = State::loading;
    reason_.clear();
    halt_ = Halt::none;
    memory_exceeded_ = false;
    if (!storage_loaded_) {
        storage_loaded_ = true;
        const auto file = services_.storage_file(id_);
        std::string error;
        const auto text = read_package_file(file, limits_.storage_bytes * 2, error);
        if (!error.empty() && error != "not found") {
            // Kept aside, not overwritten by the next save.
            std::error_code code;
            std::filesystem::rename(file, std::filesystem::path(file) += ".bad", code);
            log(LogLevel::warn, "its storage could not be read and was set aside: " + error);
        }
        if (error.empty()) {
            try {
                auto stored = Json::parse(text, {limits_.storage_bytes * 2, 33, 1u << 20});
                if (!stored.is_object()) throw std::runtime_error("it does not hold an object");
                storage_ = std::move(stored);
            } catch (const std::exception& problem) {
                // Kept aside, not overwritten by the next save.
                std::error_code code;
                std::filesystem::rename(file, std::filesystem::path(file) += ".bad", code);
                log(LogLevel::warn, std::string("its storage could not be read and was set aside: ") + problem.what());
            }
        }
    }
    L_ = lua_newstate(&Plugin::allocate, this);
    if (!L_) {
        stop("its VM could not be created");
        return false;
    }
    auto* callbacks = lua_callbacks(L_);
    callbacks->userdata = this;
    callbacks->interrupt = &Plugin::interrupt;
    try {
        luaL_openlibs(L_);
        Api::install(*this, L_);
        luaL_sandbox(L_);
        lua_State* thread = lua_newthread(L_);
        luaL_sandboxthread(thread);
        script_thread_ = lua_ref(L_, -1);
        lua_pop(L_, 1);
        lua_newtable(L_);
        modules_ = lua_ref(L_, -1);
        lua_pop(L_, 1);
    } catch (const std::exception& error) {
        stop(std::string("its VM could not be set up: ") + error.what());
        close();
        return false;
    }
    state_ = State::running;
    const auto relative = entry.empty() ? manifest_.entry : entry;
    if (!load_chunk(L_, relative, directory_ / std::filesystem::path(relative))) {
        const std::string message = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "it could not be loaded";
        lua_pop(L_, 1);
        stop(relative + " " + message);
        close();
        return false;
    }
    if (!call(0, relative, limits_.call_ms) && state_ == State::running) stop(relative + " raised an error as it ran");
    if (state_ == State::running) fire("load", {});
    if (state_ != State::running) {
        close();
        return false;
    }
    return true;
}

bool Plugin::load_chunk(lua_State* L, const std::string& relative, const std::filesystem::path& file) {
    std::string error;
    const auto source = read_package_file(file, limits_.source_bytes, error);
    if (!error.empty()) {
        lua_pushstring(L, error.c_str());
        return false;
    }
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    std::size_t size = 0;
    // Only text is ever loaded: the bytecode handed to the VM is the compiler's own.
    char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
    if (!bytecode) {
        lua_pushstring(L, "could not be compiled");
        return false;
    }
    lua_getref(L, script_thread_);
    lua_State* thread = lua_tothread(L, -1);
    lua_pop(L, 1);
    const std::string chunk = "@" + relative;
    const int status = luau_load(thread, chunk.c_str(), bytecode, size, 0);
    std::free(bytecode);
    lua_xmove(thread, L, 1);
    return status == 0;
}

bool Plugin::call(int count, std::string_view what, double limit_ms, int results) {
    const int function = lua_gettop(L_) - count;
    if (state_ != State::running) {
        lua_settop(L_, function - 1);
        return false;
    }
    lua_pushcfunction(L_, traceback, "traceback");
    lua_insert(L_, function);
    const auto start = Clock::now();
    deadline_ = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(limit_ms));
    timed_ = true;
    interrupts_ = 0;
    ++calls_;
    const int status = lua_pcall(L_, count, results, function);
    --calls_;
    timed_ = calls_ > 0; // a call this one was made from goes on, timed
    spent_ms_ += elapsed_ms(start);
    // Caught in the script or not, going over its memory stops it.
    if (memory_exceeded_ && halt_ == Halt::none) halt_ = Halt::memory;
    if (status == LUA_OK && halt_ == Halt::none) {
        if (results) lua_remove(L_, function);
        else lua_settop(L_, function - 1);
        return true;
    }
    if (halt_ == Halt::none && (memory_exceeded_ || status == LUA_ERRMEM)) halt_ = Halt::memory;
    std::string message;
    if (halt_ == Halt::none) message = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "an error that is not a string";
    lua_settop(L_, function - 1);
    if (halt_ == Halt::time) stop(std::format("{} ran longer than {:g} ms", what, limit_ms));
    else if (halt_ == Halt::memory) stop(std::format("it used more than {} MB of memory", limits_.memory >> 20));
    else if (halt_ == Halt::none) report(what, message);
    return false;
}

void Plugin::report(std::string_view what, std::string_view message) {
    services_.log(id_, LogLevel::error, shortened(std::string(what) + ": " + std::string(message)));
    const double now = services_.now_ms();
    errors_.push_back(now);
    std::erase_if(errors_, [&](double at) { return now - at > limits_.error_window_ms; });
    if (static_cast<int>(errors_.size()) > limits_.errors)
        stop(std::format("more than {} errors in {} s", limits_.errors, limits_.error_window_ms / 1000));
}

bool Plugin::count_line() {
    const double now = services_.now_ms();
    if (now - log_second_ >= 1000) {
        log_second_ = now;
        log_lines_ = 0;
    }
    if (++log_lines_ <= limits_.log_lines_per_second) return true;
    if (log_lines_ == limits_.log_lines_per_second + 1)
        services_.log(id_, LogLevel::warn, std::format("more than {} lines in a second: the rest of this second's are left out",
            limits_.log_lines_per_second));
    return false;
}

void Plugin::log(LogLevel level, std::string_view text) {
    if (count_line()) services_.log(id_, level, shortened(text));
}

void Plugin::print(std::string_view text) {
    if (count_line()) services_.console(shortened(text));
}

void Plugin::stop(std::string reason) {
    if (state_ == State::stopped) return;
    state_ = State::stopped;
    reason_ = std::move(reason);
    if (halt_ == Halt::none) halt_ = Halt::stopped;
    services_.log(id_, LogLevel::error, "stopped: " + reason_);
}

void Plugin::close_if_stopped() {
    if (state_ == State::stopped && L_) close();
}

void Plugin::close() {
    save_storage();
    shapes_.clear();
    panels_.clear();
    page_.reset();
    pending_.clear();
    building_ = nullptr;
    drawing_ = false;
    draw_table_ = ui_table_ = 0;
    exposed_.clear();
    listeners_.clear();
    http_pending_.clear();
    screens_.clear();
    open_screen_.clear();
    binds_.clear();
    handlers_.clear();
    timers_.clear();
    commands_.clear();
    loading_modules_.clear();
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
    script_thread_ = modules_ = 0;
}

void Plugin::unload() {
    if (!L_) return;
    if (state_ == State::running) {
        const auto start = Clock::now();
        const auto found = handlers_.find("unload");
        const auto handlers = found == handlers_.end() ? std::vector<int>{} : found->second;
        for (const int handler : handlers) {
            const double left = limits_.unload_ms - elapsed_ms(start);
            if (left <= 0 || state_ != State::running) break;
            lua_getref(L_, handler);
            call(0, "the unload handler", left);
        }
        if (state_ == State::running) {
            state_ = State::stopped;
            reason_ = "unloaded";
        }
    }
    close();
}

void Plugin::fire(std::string_view event, const std::vector<std::string>& arguments) {
    fire_with(event, [&](lua_State* L) {
        for (const auto& argument : arguments) lua_pushlstring(L, argument.data(), argument.size());
        return static_cast<int>(arguments.size());
    });
}

void Plugin::fire_with(std::string_view event, const std::function<int(lua_State*)>& push) {
    if (state_ != State::running) return;
    const auto found = handlers_.find(event);
    if (found == handlers_.end()) return;
    const auto handlers = found->second; // a handler may add others
    const auto what = std::format("the {} handler", event);
    for (const int handler : handlers) {
        if (state_ != State::running) return;
        lua_getref(L_, handler);
        int count = 0;
        try {
            count = push(L_);
        } catch (const std::exception& error) {
            // Out of memory building the event: the VM is closed next, so its stack is left as it is.
            stop(std::string("its event could not be handed over: ") + error.what());
            return;
        }
        call(count, what, limits_.call_ms);
    }
}

void Plugin::trick(const TrickNews& news) {
    if (state_ != State::running || !has("events.tricks")) return;
    const auto vector = [](lua_State* L, const Vec3& v) { lua_pushvector(L, v[0], v[1], v[2]); };
    switch (news.kind) {
    case TrickNews::Kind::started:
        fire_with("trick_started", [&](lua_State* L) {
            lua_createtable(L, 0, 1);
            vector(L, news.started_at);
            lua_setfield(L, -2, "at");
            return 1;
        });
        break;
    case TrickNews::Kind::landed:
        fire_with("trick_landed", [&](lua_State* L) {
            lua_createtable(L, 0, 5);
            lua_pushnumber(L, news.points);
            lua_setfield(L, -2, "points");
            lua_pushnumber(L, news.multiplier);
            lua_setfield(L, -2, "multiplier");
            vector(L, news.started_at);
            lua_setfield(L, -2, "started_at");
            vector(L, news.landed_at);
            lua_setfield(L, -2, "landed_at");
            lua_pushnumber(L, news.ms);
            lua_setfield(L, -2, "ms");
            lua_pushlstring(L, news.name.data(), news.name.size());
            lua_setfield(L, -2, "name");
            return 1;
        });
        break;
    case TrickNews::Kind::broken:
        fire_with("line_broken", [&](lua_State* L) {
            lua_createtable(L, 0, 1);
            lua_pushlstring(L, news.reason.data(), news.reason.size());
            lua_setfield(L, -2, "reason");
            return 1;
        });
        break;
    }
}

void Plugin::tick(double dt_ms) {
    if (state_ != State::running) return;
    ++tick_number_;
    raycasts_ = 0;
    const double now = services_.now_ms();
    std::vector<int> due;
    for (const auto& [id, timer] : timers_)
        if (timer.due <= now) due.push_back(id);
    for (const int id : due) {
        if (state_ != State::running) break;
        const auto found = timers_.find(id);
        if (found == timers_.end()) continue; // cancelled by an earlier one
        const int function = found->second.function;
        const bool repeat = found->second.every > 0;
        if (repeat) found->second.due = now + found->second.every;
        else timers_.erase(found);
        lua_getref(L_, function);
        if (!repeat) lua_unref(L_, function);
        call(0, "a timer", limits_.call_ms);
    }
    if (const auto found = handlers_.find("tick"); found != handlers_.end() && state_ == State::running) {
        const auto handlers = found->second;
        for (const int handler : handlers) {
            if (state_ != State::running) break;
            lua_getref(L_, handler);
            lua_pushnumber(L_, dt_ms);
            call(1, "the tick handler", limits_.call_ms);
        }
    }
    // The time budget: everything it ran since the last tick counts towards this one.
    window_spent_ += spent_ms_;
    spent_ms_ = 0;
    ++window_ticks_;
    if (!window_started_) {
        window_started_ = true;
        window_start_ = now;
    } else if (now - window_start_ >= 1000) {
        last_window_ms_ = window_spent_ / window_ticks_;
        over_windows_ = last_window_ms_ > limits_.tick_ms ? over_windows_ + 1 : 0;
        window_start_ = now;
        window_spent_ = 0;
        window_ticks_ = 0;
        if (over_windows_ >= limits_.over_windows && state_ == State::running)
            stop(std::format("it took {:.2f} ms a tick for {} s, over its {} ms", last_window_ms_, limits_.over_windows,
                limits_.tick_ms));
    }
}

void Plugin::level_changed(const std::string& previous, const std::string& current) {
    if (!previous.empty()) fire("level_unloading", {previous});
    if (!current.empty()) fire("level_loaded", {current});
}

void Plugin::run_command(const std::string& name, const std::vector<std::string>& arguments) {
    if (state_ != State::running) return;
    const auto found = commands_.find(name);
    if (found == commands_.end()) return;
    lua_getref(L_, found->second.function);
    lua_createtable(L_, static_cast<int>(arguments.size()), 0);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        lua_pushlstring(L_, arguments[i].data(), arguments[i].size());
        lua_rawseti(L_, -2, static_cast<int>(i + 1));
    }
    call(1, "the command " + name, limits_.call_ms);
}

void Plugin::draw() {
    shapes_.clear();
    shape_text_ = 0;
    if (state_ != State::running || !has("overlay") || !handlers_.contains("draw")) return;
    drawing_ = true;
    fire_with("draw", [&](lua_State* L) {
        lua_getref(L, draw_table_);
        return 1;
    });
    drawing_ = false;
}

std::vector<plugin_ui::Panel> Plugin::build_panels(const std::vector<plugin_ui::Action>& actions) {
    std::vector<plugin_ui::Panel> result;
    if (state_ != State::running || !has("menu") || (panels_.empty() && !page_)) return result;
    pending_ = actions;
    std::vector<std::pair<PanelEntry, bool>> entries;
    if (page_) entries.emplace_back(*page_, true);
    for (const auto& entry : panels_) entries.emplace_back(entry, false);
    for (const auto& [entry, page] : entries) {
        if (state_ != State::running) break;
        plugin_ui::Panel panel{id_, entry.title, {}};
        panel.page = page;
        building_ = &panel;
        lua_getref(L_, entry.function);
        lua_getref(L_, ui_table_);
        call(1, (page ? "the page " : "the panel ") + entry.title, limits_.call_ms);
        building_ = nullptr;
        result.push_back(std::move(panel));
    }
    pending_.clear();
    return result;
}
std::string Plugin::page_name() const {
    if (state_ != State::running || !has("menu")) return {};
    if (page_) return page_->title;
    return panels_.empty() ? std::string() : id_;
}

void Plugin::session_changed(const std::vector<SessionPlayer>& joined, const std::vector<SessionPlayer>& left) {
    if (state_ != State::running || !has("session.read")) return;
    for (const auto& player : joined)
        fire_with("player_joined", [&](lua_State* L) {
            push_player(L, player);
            return 1;
        });
    for (const auto& player : left)
        fire_with("player_left", [&](lua_State* L) {
            push_player(L, player);
            return 1;
        });
}

void Plugin::receive(const SessionPlayer& from, const std::string& payload) {
    if (state_ != State::running || !has("net.session")) return;
    fire_with("message", [&](lua_State* L) {
        push_player(L, from);
        lua_pushlstring(L, payload.data(), payload.size());
        return 2;
    });
}

std::string Plugin::call_exposed(const std::string& function, const Json& arguments, Json& result) {
    if (state_ != State::running) return "it is not running";
    if (busy()) return "it is in a call already (a plugin cannot be called back into)";
    const auto found = exposed_.find(function);
    if (found == exposed_.end()) return "it exposes no function \"" + function + "\"";
    const int top = lua_gettop(L_);
    try {
        lua_getref(L_, found->second);
        int count = 0;
        for (const auto& argument : arguments) {
            push_json(L_, argument);
            ++count;
        }
        if (!call(count, "its exposed function " + function, limits_.call_ms, 1))
            return state_ == State::running ? "it raised an error (its log says which)" : "it stopped: " + reason_;
        result = lua_to_json(L_, -1);
        lua_settop(L_, top);
        return {};
    } catch (const std::exception& error) {
        if (L_ && state_ == State::running) lua_settop(L_, top);
        return std::string("its answer could not be handed over: ") + error.what();
    }
}

std::vector<std::string> Plugin::exposed_names() const {
    std::vector<std::string> names;
    for (const auto& item : exposed_) names.push_back(item.first);
    return names;
}

void Plugin::plugin_event(const std::string& source, const std::string& source_channel, const std::string& event, const Json& data) {
    if (state_ != State::running) return;
    const auto lower = [](std::string text) {
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        return text;
    };
    const auto listeners = listeners_;
    for (const auto& listener : listeners) {
        if (state_ != State::running) return;
        if (listener.event != event) continue;
        const auto from = lower(listener.source);
        if (from != "*" && from != lower(source) && from != lower(source_channel)) continue;
        lua_getref(L_, listener.function);
        int count = 2;
        try {
            lua_pushlstring(L_, source.data(), source.size());
            push_json(L_, data);
        } catch (const std::exception& error) {
            stop(std::string("an event could not be handed over: ") + error.what());
            return;
        }
        call(count, "its on_plugin handler for " + event, limits_.call_ms);
    }
}

void Plugin::http_done(const HttpResponse& response) {
    if (state_ != State::running) return;
    const auto found = http_pending_.find(response.id);
    if (found == http_pending_.end()) return;
    const int function = found->second;
    http_pending_.erase(found);
    lua_getref(L_, function);
    lua_unref(L_, function);
    lua_pushinteger(L_, response.status);
    lua_pushlstring(L_, response.body.data(), response.body.size());
    if (response.error.empty()) lua_pushnil(L_);
    else lua_pushlstring(L_, response.error.data(), response.error.size());
    call(3, "its web request's callback", limits_.call_ms);
}

void Plugin::skater_changed(const char* state, const char* previous) {
    if (state_ != State::running || !has("events.skater")) return;
    fire_with("skater_state", [&](lua_State* L) {
        lua_pushstring(L, state);
        lua_pushstring(L, previous);
        return 2;
    });
}

// A launch into the air (events.skater), with its velocities as vectors.
void Plugin::launched(const LaunchNews& launch) {
    if (state_ != State::running || !has("events.skater")) return;
    fire_with("launch", [&](lua_State* L) {
        lua_createtable(L, 0, 8);
        lua_pushlstring(L, launch.from.data(), launch.from.size());
        lua_setfield(L, -2, "from");
        push_position(L, launch.velocity);
        lua_setfield(L, -2, "velocity");
        push_position(L, launch.game);
        lua_setfield(L, -2, "game");
        if (launch.travel) {
            push_position(L, Vec3{(*launch.travel)[0], 0, (*launch.travel)[1]});
            lua_setfield(L, -2, "travel");
        }
        lua_pushnumber(L, launch.travel_speed);
        lua_setfield(L, -2, "travel_speed");
        lua_pushboolean(L, launch.reason == "applied");
        lua_setfield(L, -2, "shaped");
        lua_pushlstring(L, launch.reason.data(), launch.reason.size());
        lua_setfield(L, -2, "reason");
        if (!launch.by.empty()) {
            lua_pushlstring(L, launch.by.data(), launch.by.size());
            lua_setfield(L, -2, "by");
        }
        return 1;
    });
}

void Plugin::game_event(std::string_view event, std::string_view permission, const Json& data) {
    if (state_ != State::running || !has(permission)) return;
    fire_with(event, [&](lua_State* L) {
        push_json(L, data);
        return 1;
    });
}
void Plugin::bailed(std::uint64_t wipeouts, std::optional<SlamNews> slam) {
    if (state_ != State::running || !has("events.skater")) return;
    fire_with("bail", [&](lua_State* L) {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, static_cast<double>(wipeouts));
        lua_setfield(L, -2, "count");
        // The slam that did it, when one was recorded in the last second: its force and cause.
        if (slam && slam->ago_ms < 1000) {
            lua_pushnumber(L, slam->force);
            lua_setfield(L, -2, "force");
            lua_pushinteger(L, slam->cause);
            lua_setfield(L, -2, "cause");
        }
        if (const auto s = services_.skater()) {
            lua_pushvector(L, s->position[0], s->position[1], s->position[2]);
            lua_setfield(L, -2, "at");
        }
        return 1;
    });
}

void Plugin::touch_tick(const std::vector<Body>& list) {
    if (state_ != State::running || !has("session.skaters") || !touch_reach_) return;
    const auto tell = [&](std::string_view event, std::uint64_t player, const Contact& c) {
        fire_with(event, [&](lua_State* L) {
            lua_createtable(L, 0, 5);
            const auto id = std::to_string(player);
            lua_pushlstring(L, id.data(), id.size());
            lua_setfield(L, -2, "player");
            lua_pushstring(L, body_part_names[c.part]);
            lua_setfield(L, -2, "part");
            lua_pushstring(L, body_part_names[c.their_part]);
            lua_setfield(L, -2, "their_part");
            lua_pushnumber(L, std::max(c.gap, 0.f));
            lua_setfield(L, -2, "distance");
            lua_pushvector(L, c.at[0], c.at[1], c.at[2]);
            lua_setfield(L, -2, "position");
            return 1;
        });
    };
    const Body* mine = nullptr;
    for (const auto& body : list)
        if (!body.player) mine = &body;
    std::set<std::uint64_t> seen;
    for (const auto& body : list) {
        if (!body.player || !mine) continue;
        const auto near = bodies::nearest(*mine, body);
        if (!near) continue;
        seen.insert(body.player);
        const Contact now{near->mine, near->theirs, near->gap, near->at};
        const auto was = contacts_.find(body.player);
        if (was == contacts_.end()) {
            if (near->gap > *touch_reach_ || contacts_.size() >= 64) continue;
            contacts_[body.player] = now;
            tell("touch", body.player, now);
        } else if (near->gap <= *touch_reach_ + 0.1f) {
            was->second = now;
        } else {
            contacts_.erase(was);
            tell("touch_end", body.player, now);
        }
        if (state_ != State::running || !touch_reach_) return; // a handler stopped it, or touching
    }
    for (auto it = contacts_.begin(); it != contacts_.end();) {
        if (seen.contains(it->first)) {
            ++it;
            continue;
        }
        const auto [player, last] = *it;
        it = contacts_.erase(it);
        tell("touch_end", player, last);
        if (state_ != State::running || !touch_reach_) return;
    }
}

void Plugin::chat_line(const ChatLine& line) {
    if (state_ != State::running || !has("chat.read")) return;
    fire_with("chat", [&](lua_State* L) {
        lua_createtable(L, 0, 5);
        const auto id = std::to_string(line.sender);
        lua_pushlstring(L, id.data(), id.size());
        lua_setfield(L, -2, "sender");
        lua_pushlstring(L, line.name.data(), line.name.size());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, line.text.data(), line.text.size());
        lua_setfield(L, -2, "text");
        lua_pushboolean(L, line.local);
        lua_setfield(L, -2, "local");
        lua_pushboolean(L, line.server);
        lua_setfield(L, -2, "server");
        return 1;
    });
}

void Plugin::track_changed(const Track& track) {
    if (state_ != State::running || !has("music")) return;
    fire_with("track_changed", [&](lua_State* L) {
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, track.app.data(), track.app.size());
        lua_setfield(L, -2, "app");
        lua_pushlstring(L, track.title.data(), track.title.size());
        lua_setfield(L, -2, "title");
        lua_pushlstring(L, track.artist.data(), track.artist.size());
        lua_setfield(L, -2, "artist");
        lua_pushboolean(L, track.playing);
        lua_setfield(L, -2, "playing");
        return 1;
    });
}

void Plugin::throwdown_phase(const char* phase) {
    if (state_ != State::running || !has("throwdowns")) return;
    fire_with("throwdown", [&](lua_State* L) {
        lua_createtable(L, 0, 1);
        lua_pushstring(L, phase);
        lua_setfield(L, -2, "phase");
        return 1;
    });
}

void Plugin::action(const std::string& name) {
    if (state_ != State::running || !has("input")) return;
    fire({"action"}, {name});
}

std::optional<plugin_ui::Panel> Plugin::build_screen(const std::vector<plugin_ui::Action>& actions) {
    if (state_ != State::running || !has("ui.screen") || open_screen_.empty()) return std::nullopt;
    const auto found = std::find_if(screens_.begin(), screens_.end(), [&](const auto& e) { return e.title == open_screen_; });
    if (found == screens_.end()) return std::nullopt;
    const auto entry = *found;
    plugin_ui::Panel panel{id_, entry.title, {}, true};
    panel.dock = entry.dock;
    panel.width = entry.width;
    pending_ = actions;
    building_ = &panel;
    lua_getref(L_, entry.function);
    lua_getref(L_, ui_table_);
    call(1, "the screen " + entry.title, limits_.call_ms);
    building_ = nullptr;
    pending_.clear();
    if (state_ != State::running || open_screen_ != entry.title) return std::nullopt;
    return panel;
}

void Plugin::close_screen() {
    if (open_screen_.empty()) return;
    const auto title = std::exchange(open_screen_, {});
    fire("screen_closed", {title});
}

void Plugin::set_channel(std::string channel) {
    channel_ = std::move(channel);
    channel_hash_ = wire::channel_of(channel_);
}

std::vector<CommandInfo> Plugin::commands() const {
    std::vector<CommandInfo> result;
    for (const auto& [name, command] : commands_) {
        result.push_back({name, {}, command.help});
        for (const auto& sub : command.subcommands) result.push_back({name + " " + sub.name, sub.args, sub.help});
    }
    return result;
}

void Plugin::save_storage() {
    if (!storage_dirty_) return;
    storage_dirty_ = false;
    const auto file = services_.storage_file(id_);
    try {
        std::filesystem::create_directories(file.parent_path());
        auto temporary = std::filesystem::path(file) += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            const auto text = storage_.dump(); // as compact as the quota measures it
            if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("cannot write it");
        }
        std::filesystem::rename(temporary, file);
    } catch (const std::exception& error) {
        services_.log(id_, LogLevel::warn, std::string("its storage could not be saved: ") + error.what());
    }
}
}
