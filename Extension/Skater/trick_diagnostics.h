#pragma once
#include "Engine/Core/Json/json.h"
#include <atomic>
namespace dingosdk::trick_logger {
using DiagnosticSink=void (*)(const char*,Json);
inline std::atomic<DiagnosticSink> diagnostic_sink{};
inline void emit_diagnostic(const char* kind,Json value) {if(const auto sink=diagnostic_sink.load())sink(kind,std::move(value));}
inline void detector_evidence(Json value){emit_diagnostic("detector_evidence",std::move(value));}
inline void native_eligibility(Json value){emit_diagnostic("native_eligibility",std::move(value));}
inline void comparison(Json value){emit_diagnostic("detector_comparison",std::move(value));}
}
