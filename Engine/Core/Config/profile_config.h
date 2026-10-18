#pragma once
#include "Engine/Core/Json/json.h"
#include <filesystem>
#include <fstream>
#include <optional>
#include <shlobj.h>
namespace dingosdk::profile_config {
// Extracted from Trainer data_directory/load_store/save_store; shared implementation.
inline std::filesystem::path directory() {
    PWSTR raw{};std::filesystem::path result;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw)) && raw)
        result=std::filesystem::path(raw)/L"ReSkate"/L"trainer";
    if(raw)CoTaskMemFree(raw);
    return result;
}
inline std::optional<Json> read(const std::filesystem::path& path) {
    try {
        std::ifstream file(path,std::ios::binary);
        if(!file) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        if(text.empty() || text.size()>1024*1024) return std::nullopt;
        return Json::parse(text);
    } catch(...) {return std::nullopt;}
}
inline bool write(const std::filesystem::path& target,const Json& json) {
    try {
        if(target.parent_path().empty()) return false;
        std::error_code error;std::filesystem::create_directories(target.parent_path(),error);
        auto temporary=target;temporary+=L".tmp";
        {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<json.dump(2);if(!file)return false;}
        std::filesystem::rename(temporary,target,error);
        if(error) {std::filesystem::remove(target,error);std::filesystem::rename(temporary,target,error);}
        return !error;
    } catch(...) {return false;}
}
}
