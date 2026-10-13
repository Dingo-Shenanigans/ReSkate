#pragma once
#include <cstdint>
#include <string>

// Lua in the game is Lua and nothing else, whoever wrote the script: a loose file in scripts/,
// a custom script, or one of the game's own. The game ships stock Lua 5.2 with every library
// open, so a script could otherwise load a DLL or start a program. These are switched off in
// the game's own Lua functions, before any Lua state exists, so no script runs ahead of it and
// none can keep a copy of the working function:
//  * loading a DLL: package.loadlib, and require through the two C searchers;
//  * starting a program: os.execute, io.popen;
//  * writing, deleting or renaming a file on the player's disk: io.open, io.output, os.remove,
//    os.rename (a script that can write a file can leave a program where Windows starts it);
//  * loading precompiled chunks: bytecode is not checked by Lua and is a known way to run
//    native code. Every script the game ships is text.
// The game's scripts use none of them: they read through the engine's own vfs and dofile.
// Each attempt is refused the way Lua reports a failure, and logged.
namespace dingosdk {
bool start_lua_native_block(std::uintptr_t base, std::string& error);
}
