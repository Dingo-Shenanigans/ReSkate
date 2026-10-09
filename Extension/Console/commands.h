#pragma once
#include "Engine/Core/Console/command_registry.h"
#include "Extension/UI/Overlay/overlay.h"

namespace dingosdk::console {
using Model = overlay::Model;
using Commands = Registry<Model>;
using Entry = Commands::Entry;
using Argument = Commands::Argument;
const Commands &game_commands();
Argument argument(std::string name, Type type = Type::text, bool optional = false);
Entry action(std::string name, std::string description, Group group, std::vector<Argument> arguments = {});
Entry variable(std::string name, std::string description, Group group, Argument argument);
State boolean_state(bool available, bool value, std::string reason = {}, std::string detail = {});
void register_movement_commands(Commands &);
void register_ai_commands(Commands &);
void register_settings_commands(Commands &);
void register_world_commands(Commands &);
void register_graphics_commands(Commands &);
void register_progression_commands(Commands &);
void register_object_commands(Commands &);
void register_park_editor_commands(Commands &);
void register_multiplayer_commands(Commands &);
void register_perf_commands(Commands &);
void register_trainer_commands(Commands &);
void register_hall_of_meat_commands(Commands &);
// Runtime adapters. Invoked only by the verified game-thread dispatcher.
void request_debug(overlay::DebugAction, bool enabled = false, float value = 0);
void request_feature(overlay::OfflineFeatureGroup, bool enabled);
void request_engine_variable(std::string_view name, bool restore, bool value);
std::string request_named_setting(std::string_view name, std::string_view value, bool restore);
std::string request_reset_named_settings();
void request_level(const std::string &level, const std::string &start, const std::string &sublevel,
                   const std::string &substart);
} // namespace dingosdk::console
