#pragma once

#include <filesystem>
#include <string>

namespace dingosdk {

// Skate.exe keeps its settings (the EASaveLoad ProfileSettings container),
// device keys and caches under <Local AppData>\Skate. Under ReSkate those
// lookups resolve to <Local AppData>\ReSkate\Game instead (or the Game folder of
// a separate user-data root, Engine/Core/Platform/user_data_paths.h), so ReSkate
// never reads or writes the live game's files. Other modules are left alone.
std::filesystem::path redirected_local_app_data();
bool start_user_data_redirect(std::string& error) noexcept;

} // namespace dingosdk
