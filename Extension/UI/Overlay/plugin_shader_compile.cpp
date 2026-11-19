#include "plugin_shader_compile.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <mutex>
#include <vector>

namespace dingosdk::plugin_shader_compile {
namespace {
using Microsoft::WRL::ComPtr;

// The game's own DirectX Shader Compiler, beside Skate.exe: dxil.dll first, so the compiler finds its validator (and
// what it makes is signed), then dxcompiler.dll. Loaded once, kept for the process's life. Null where either is not
// there or will not load.
DxcCreateInstanceProc dxc_create() {
    static std::once_flag once;
    static DxcCreateInstanceProc create{};
    std::call_once(once, [] {
        try {
            std::wstring exe(MAX_PATH, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
            if (!length || length >= exe.size()) return;
            exe.resize(length);
            const auto folder = std::filesystem::path(exe).parent_path();
            const auto validator = folder / L"dxil.dll", compiler = folder / L"dxcompiler.dll";
            HMODULE dxil = LoadLibraryExW(validator.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            HMODULE dxc = dxil ? LoadLibraryExW(compiler.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
            if (dxc) create = reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void*>(GetProcAddress(dxc, "DxcCreateInstance")));
            logging::write(logging::Level::info, logging::Channel::graphics,
                           create ? "Plugin shaders: HLSL is compiled to DXIL with the game's own dxcompiler.dll and dxil.dll."
                                  : "Plugin shaders: the game's dxcompiler.dll and dxil.dll could not be loaded; HLSL is compiled to DXBC.");
        } catch (...) {
            create = nullptr;
        }
    });
    return create;
}

std::string shortened(std::string text) {
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.size() > 600) text = text.substr(0, 600) + "...";
    return text;
}

// With dxcompiler: DXIL, ps_6_0, HLSL 2018 (the language shaders for the older compiler are written in), optimised,
// its debug and reflection parts left out. False (and no error) where the compiler could not be made.
bool compile_dxil(const std::string& source, const std::string& name, Result& out) {
    const auto create = dxc_create();
    if (!create) return false;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler) return false;
    const std::wstring file(name.begin(), name.end());
    std::vector<LPCWSTR> arguments{file.c_str(), L"-E", L"main", L"-T", L"ps_6_0", L"-HV", L"2018", L"-O3", L"-Qstrip_debug", L"-Qstrip_reflect"};
    DxcBuffer buffer{};
    buffer.Ptr = source.data();
    buffer.Size = source.size();
    buffer.Encoding = DXC_CP_UTF8;
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buffer, arguments.data(), static_cast<UINT32>(arguments.size()), nullptr, IID_PPV_ARGS(&result))) || !result) {
        out.error = "the shader compiler failed";
        return true;
    }
    HRESULT status{};
    result->GetStatus(&status);
    ComPtr<IDxcBlobUtf8> errors;
    if (result->HasOutput(DXC_OUT_ERRORS)) result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (FAILED(status)) {
        out.error = errors && errors->GetStringLength() ? shortened(std::string(errors->GetStringPointer(), errors->GetStringLength()))
                                                         : std::string("it did not compile");
        return true;
    }
    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object || !object->GetBufferSize()) {
        out.error = "the shader compiler gave nothing";
        return true;
    }
    out.code.assign(static_cast<const char*>(object->GetBufferPointer()), object->GetBufferSize());
    out.dxil = true;
    return true;
}
} // namespace

Result compile_pixel_shader(const std::string& source, const std::string& name) noexcept {
    Result out;
    try {
        if (compile_dxil(source, name, out)) return out;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3DCompile(source.data(), source.size(), name.c_str(), nullptr, nullptr, "main", "ps_5_0",
                              D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error)) ||
            !blob) {
            out.error = error ? shortened(std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()))
                              : std::string("it did not compile");
            return out;
        }
        out.code.assign(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize());
    } catch (...) {
        out.code.clear();
        out.error = "an exception was raised while compiling it";
    }
    return out;
}
} // namespace dingosdk::plugin_shader_compile
