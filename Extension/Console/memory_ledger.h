#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Who owns the memory the process holds, and how much of it is used (`perf memory owners`).
// While it is on, every reservation of memory from Windows is noted with the code that asked
// for it, and forgotten when it is given back; a report then joins what the process holds now
// with who made it: what is committed, what of that is in RAM, and what is in RAM but all zero.
// A minute at a time it also looks for an owner whose memory only grows, and says so in the log.
// It is a research aid and off until asked for: every allocation in the process passes it.
namespace dingosdk::memory_ledger {
using Line = std::function<void(const std::string&)>;

// Starts noting reservations. What was made before is reported as not seen being made.
bool start(std::string& error);
bool on() noexcept;
// `perf memory trace`: also counts what is asked for (commits too) of sizes in [least, most]
// by who asked, from now on. Starts the ledger if it is off.
bool start_asking(std::uint64_t least, std::uint64_t most, std::string& error);
void stop_asking() noexcept;
bool asking() noexcept;
void asked_report(const Line& out);

// What the process holds by owner and by caller (the ledger is started if it is off).
// `most` callers are listed, each with its stack.
void owners_report(const Line& out, std::size_t most = 12);
// The process's heaps: what each holds and has free inside it, and its blocks by size; with
// `mark`, the sizes are remembered and the next report says what was added since.
void heap_report(const Line& out, bool mark, std::size_t most = 20);
// Video memory and the system memory the GPU uses for this process, one line per adapter.
void gpu_report(const Line& out);
// Everything above at length, into memory-<time>.txt beside ReSkate.log.
std::filesystem::path write_report();

// For the periodic "Memory:" log line: the largest owners, as the leak watcher last saw them
// (empty while the ledger is off), and the GPU's use.
std::string owners_line();
std::string gpu_line();
}
