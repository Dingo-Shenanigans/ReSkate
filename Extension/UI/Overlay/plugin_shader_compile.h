#pragma once
#include <string>

// A plugin's HLSL pixel shader (reskate.screen.shader, reskate.world.shader) compiled, on the caller's thread (each
// compiles on one of its own). With the DirectX Shader Compiler the game installs beside Skate.exe (dxcompiler.dll,
// and dxil.dll, whose validator signs what it makes): DXIL, ps_6_0, as ReSkate Studio compiles a mod's material
// shaders. It is far quicker than the older compiler on a large shader (the Snow Skate mod's snow took 65 s with it,
// 2026-11-15). Where those cannot be loaded, the older compiler in d3dcompiler (DXBC, ps_5_0).
namespace dingosdk::plugin_shader_compile {
struct Result {
    std::string code;  // the compiled shader: DXIL when `dxil`, else DXBC; empty when it failed
    bool dxil{};
    std::string error; // why it did not compile (the compiler's own message, shortened)
};
// `source` the whole HLSL (the prelude in front of the plugin's), `name` its file (in the compiler's messages);
// entry `main`.
Result compile_pixel_shader(const std::string& source, const std::string& name) noexcept;
} // namespace dingosdk::plugin_shader_compile
