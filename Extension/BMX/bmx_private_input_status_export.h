#pragma once
// Explicit local diagnostic command only. Never call this from an input hook.
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace dingosdk::bmx::private_input_status_export {
inline constexpr std::size_t byte_limit=256*1024;
inline constexpr unsigned attempt_limit=8;
// Bound complete and failed temporary exports to at most 2 MiB per process.
inline unsigned reserve(std::atomic<unsigned>& used) noexcept {
    auto count=used.load(std::memory_order_relaxed);
    while(count<attempt_limit)if(used.compare_exchange_weak(count,count+1,std::memory_order_relaxed))return count+1;
    return 0;
}
struct Result {bool saved{};const char* issue="not_attempted";std::filesystem::path path;std::size_t bytes{};};
// Caller must freshly qualify its private profile/root immediately before use.
// Fixed basename, no supplied filename, no directory creation, no replacement.
inline Result write(const std::filesystem::path& root,std::string_view text,
    DWORD process,std::uint64_t creation,unsigned sequence) {
    Result result;
    if(!root.is_absolute()||!process||!creation||!sequence||sequence>attempt_limit){result.issue="invalid_export_identity";return result;}
    if(text.empty()||text.size()>byte_limit){result.issue="status_byte_limit";return result;}
    result.path=root/(L"bmx-native-input-"+std::to_wstring(process)+L"-"+std::to_wstring(creation)+L"-"+std::to_wstring(sequence)+L".json");
    const auto temporary=std::filesystem::path(result.path.native()+L".tmp");
    const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){result.issue="exclusive_temporary_create_failed";return result;}
    DWORD written{};
    const bool complete=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)&&
        written==text.size()&&FlushFileBuffers(file);
    const bool closed=CloseHandle(file)!=FALSE;
    if(!complete||!closed){result.issue="temporary_write_or_flush_failed";return result;}
    // Same-directory rename publishes only the complete file. Omit
    // MOVEFILE_REPLACE_EXISTING: a previous receipt is never overwritten.
    if(!MoveFileExW(temporary.c_str(),result.path.c_str(),MOVEFILE_WRITE_THROUGH)){
        result.issue="atomic_no_replace_publish_failed";return result;
    }
    result.saved=true;result.issue="saved";result.bytes=text.size();return result;
}
}
