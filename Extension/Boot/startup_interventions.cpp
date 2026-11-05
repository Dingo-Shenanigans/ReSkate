#include "startup_interventions.h"
#include "Engine/Core/Hooks/patches.h"
#include "Engine/Core/Log/logging.h"

#include <Windows.h>

#include <array>
#include <limits>
#include <stdexcept>

namespace dingosdk {
namespace {
namespace pins = addr::startup_interventions;
constexpr std::uint8_t breakpoint_opcode = 0xcc;
StartupInterventionMachine machine;
std::atomic<std::uintptr_t> installed_base{};
PVOID handler_handle{};
SRWLOCK handler_lock = SRWLOCK_INIT;
std::array<std::uint8_t, startup_frame_size> saved_frame{};
std::uintptr_t saved_frame_address{};

struct HandlerGuard {
    DWORD last_error{GetLastError()};
    HandlerGuard() noexcept { AcquireSRWLockExclusive(&handler_lock); }
    ~HandlerGuard() { ReleaseSRWLockExclusive(&handler_lock); SetLastError(last_error); }
};

template<class T>
bool read_local(std::uintptr_t address, T& output) noexcept {
    SIZE_T count{};
    return address && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address), &output, sizeof(output), &count) && count == sizeof(output);
}

bool writable_range(std::uintptr_t address, std::size_t size) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !size || !VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    switch (memory.Protect & 0xff) {
    case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
    default: return false;
    }
    const auto region = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return address >= region && address - region < memory.RegionSize && size <= memory.RegionSize - (address - region);
}

bool write_local(std::uintptr_t address, const void* data, std::size_t size) noexcept {
    SIZE_T count{};
    return writable_range(address, size) && WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(address), data, size, &count) && count == size;
}

bool executable_image_range(std::uintptr_t image_base, std::uintptr_t address, std::size_t size) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (!image_base || !address || !size ||
        !VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    switch (memory.Protect & 0xff) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
    default: return false;
    }
    const auto region = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE &&
        reinterpret_cast<std::uintptr_t>(memory.AllocationBase) == image_base &&
        address >= region && address - region < memory.RegionSize && size <= memory.RegionSize - (address - region);
}

// The site's own opcode back over its int3 (patches.h): unchecked, as the handler has already read
// the int3 there, and allocation-free, so the handler can call it.
bool restore_opcode(std::uintptr_t address, std::uint8_t opcode) noexcept {
    return patch_breakpoint_restore(address, opcode) == HookOk;
}

StartupInterventionRegisters capture_registers(const CONTEXT& c, std::uintptr_t address) noexcept {
    return {address, c.Rsp, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rbp, c.Rsi, c.Rdi,
            c.R8, c.R9, c.R10, c.R11, c.R14, c.R15, c.EFlags};
}

bool frame_shape_matches(std::uintptr_t base, const StartupInterventionRegisters& r) noexcept {
    return base == supported_build::preferred_image_base && r.rip == base + startup_frame_rva &&
        r.rbp >= pins::frame_stack_offset && r.rsp == r.rbp - pins::frame_stack_offset &&
        r.rcx == r.rbp - pins::frame_vm_offset && r.rdx == r.rbp - startup_frame_size &&
        r.r15 == r.rbp - pins::frame_saved_registers_offset &&
        r.r11 == base + startup_frame_data_rva && r.r9 == pins::frame_count && r.r10 == r.r9 &&
        (r.rsi & 0xff) == 0;
}

LONG CALLBACK intervention_handler(EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord ||
        pointers->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto base = installed_base.load(std::memory_order_acquire);
    const auto address = reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    if (!base || (address != base + startup_frame_rva && address != base + startup_exit_rva))
        return EXCEPTION_CONTINUE_SEARCH;
    HandlerGuard guard;
    auto& context = *pointers->ContextRecord;
    auto registers = capture_registers(context, address);
    const auto thread = GetCurrentThreadId();

    if (address == base + startup_frame_rva) {
        std::array<std::uint8_t, 3> instruction{};
        if (!read_local(address, instruction) ||
            (instruction[0] != breakpoint_opcode && instruction[0] != startup_frame_instruction[0])) return EXCEPTION_CONTINUE_SEARCH;
        const bool patched = instruction[0] == breakpoint_opcode;
        instruction[0] = startup_frame_instruction[0];
        if (instruction != startup_frame_instruction) { machine.fail(); return EXCEPTION_CONTINUE_SEARCH; }
        // Another thread may have fetched INT3 before capture restored the site.
        if (!patched) { context.Rip = address; return EXCEPTION_CONTINUE_EXECUTION; }
        if (machine.phase() == StartupInterventionPhase::armed && frame_shape_matches(base, registers)) {
            StartupFrameMetadata metadata{};
            if (!read_local(registers.rcx + startup_frame_cursor_offset, metadata.cursor)) {
                machine.fail(); return EXCEPTION_CONTINUE_SEARCH;
            }
            if (startup_frame_clear_matches(base, registers, instruction, metadata)) {
                const auto frame_address = registers.rbp - startup_frame_size;
                if (!read_local(frame_address, saved_frame) ||
                    !machine.capture_frame(base, registers, instruction, metadata, thread) ||
                    !restore_opcode(address, startup_frame_instruction[0])) {
                    machine.fail(); return EXCEPTION_CONTINUE_SEARCH;
                }
                saved_frame_address = frame_address;
                // Let the original clear run; restore this exact frame only if the
                // guarded exit arrives on the same thread and native stack frame.
                context.Rip = address;
                logging::log(logging::Level::info, logging::Channel::runtime,
                    "Startup frame captured: {} bytes, site {:#x}, bytecode {:#x}",
                    saved_frame.size(), startup_frame_rva, startup_frame_data_rva);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        // This VM opcode also performs ordinary byte stores before the target
        // clear. Reproduce MOV [RDX], SIL without changing flags or registers.
        const auto byte = static_cast<std::uint8_t>(registers.rsi);
        if (!write_local(registers.rdx, &byte, sizeof(byte))) {
            machine.fail();
            logging::write(logging::Level::error, logging::Channel::runtime,
                "Startup byte-store intervention could not replay an ordinary store");
            if (!restore_opcode(address, startup_frame_instruction[0])) return EXCEPTION_CONTINUE_SEARCH;
            context.Rip = address;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        context.Rip = address + startup_frame_instruction.size();
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    std::array<std::uint8_t, 6> instruction{};
    std::uint64_t return_address{};
    if (!read_local(address, instruction) || instruction[0] != breakpoint_opcode ||
        !read_local(registers.rsp, return_address)) { machine.fail(); return EXCEPTION_CONTINUE_SEARCH; }
    instruction[0] = startup_exit_instruction[0];
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Startup exit intervention hit: return {:#x}, rcx {:#x}, rdx {:#x}, frame captured {}",
        return_address >= base ? return_address - base : return_address, registers.rcx, registers.rdx,
        machine.phase() == StartupInterventionPhase::frame_captured);
    if (!machine.return_from_exit(base, registers, instruction, return_address, thread) ||
        !write_local(saved_frame_address, saved_frame.data(), saved_frame.size()) ||
        !restore_opcode(address, startup_exit_instruction[0])) {
        machine.fail();
        logging::write(logging::Level::error, logging::Channel::runtime, "Startup exit/frame restoration guard failed");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    context.Rax = registers.rax;
    context.Rip = registers.rip;
    context.Rsp = registers.rsp;
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Startup frame restored: {} bytes; exit intervention completed", saved_frame.size());
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

bool startup_frame_clear_matches(std::uintptr_t base, const StartupInterventionRegisters& registers,
    const std::array<std::uint8_t, 3>& instruction, const StartupFrameMetadata& metadata) noexcept {
    return frame_shape_matches(base, registers) && instruction == startup_frame_instruction &&
        metadata.cursor == base + startup_frame_cursor_rva;
}

bool StartupInterventionMachine::arm() noexcept {
    auto expected = StartupInterventionPhase::uninitialized;
    return phase_.compare_exchange_strong(expected, StartupInterventionPhase::armed, std::memory_order_acq_rel);
}

bool StartupInterventionMachine::capture_frame(std::uintptr_t base, const StartupInterventionRegisters& registers,
    const std::array<std::uint8_t, 3>& instruction, const StartupFrameMetadata& metadata, std::uint32_t thread) noexcept {
    auto expected = StartupInterventionPhase::armed;
    if (!phase_.compare_exchange_strong(expected, StartupInterventionPhase::handling_frame, std::memory_order_acq_rel)) return false;
    if (!thread || !startup_frame_clear_matches(base, registers, instruction, metadata)) { fail(); return false; }
    frame_base_ = registers.rbp;
    frame_thread_ = thread;
    phase_.store(StartupInterventionPhase::frame_captured, std::memory_order_release);
    return true;
}

bool StartupInterventionMachine::return_from_exit(std::uintptr_t base, StartupInterventionRegisters& registers,
    const std::array<std::uint8_t, 6>& instruction, std::uint64_t return_address, std::uint32_t thread) noexcept {
    auto expected = StartupInterventionPhase::frame_captured;
    if (!phase_.compare_exchange_strong(expected, StartupInterventionPhase::handling_exit, std::memory_order_acq_rel)) return false;
    const bool matches = base == supported_build::preferred_image_base && registers.rip == base + startup_exit_rva &&
        registers.rcx == std::numeric_limits<std::uint64_t>::max() && registers.rdx == 0x32 &&
        instruction == startup_exit_instruction && return_address == base + startup_exit_return_rva &&
        thread == frame_thread_ && registers.rbp == frame_base_ &&
        frame_base_ >= pins::exit_stack_offset && registers.rsp == frame_base_ - pins::exit_stack_offset;
    if (!matches) { fail(); return false; }
    registers.rax = FALSE;
    registers.rip = return_address;
    registers.rsp += sizeof(return_address);
    phase_.store(StartupInterventionPhase::complete, std::memory_order_release);
    return true;
}

void StartupInterventionMachine::fail() noexcept { phase_.store(StartupInterventionPhase::failed, std::memory_order_release); }
StartupInterventionPhase StartupInterventionMachine::phase() const noexcept { return phase_.load(std::memory_order_acquire); }

bool startup_intervention_fingerprints_match(std::uintptr_t base, std::uint32_t size,
    const std::array<std::uint8_t, 8>& frame, const std::array<std::uint8_t, 6>& exit) noexcept {
    return base == supported_build::preferred_image_base && size == startup_expected_image_size &&
        frame == startup_frame_fingerprint && exit == startup_exit_instruction;
}

bool start_startup_interventions(std::uintptr_t base, std::string& error) noexcept {
    error.clear();
    try {
        if (handler_handle || installed_base.load()) throw std::runtime_error("Startup interventions are already installed");
        IMAGE_DOS_HEADER dos{};
        if (!read_local(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000)
            throw std::runtime_error("Startup intervention image has an invalid DOS header");
        IMAGE_NT_HEADERS64 nt{};
        if (!read_local(base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            throw std::runtime_error("Startup intervention image has an invalid PE header");
        const auto frame_address = base + startup_frame_rva;
        const auto exit_address = base + startup_exit_rva;
        if (!executable_image_range(base, frame_address, startup_frame_fingerprint.size()) ||
            !executable_image_range(base, exit_address, startup_exit_instruction.size()))
            throw std::runtime_error("Startup intervention sites are outside executable image pages");
        std::array<std::uint8_t, 8> frame{};
        std::array<std::uint8_t, 6> exit{};
        if (!read_local(frame_address, frame) || !read_local(exit_address, exit) ||
            !startup_intervention_fingerprints_match(base, nt.OptionalHeader.SizeOfImage, frame, exit))
            throw std::runtime_error("Startup intervention image fingerprint does not match");
        installed_base.store(base, std::memory_order_release);
        handler_handle = patch_breakpoint_handler("startup interventions", intervention_handler);
        if (!handler_handle) { installed_base.store(0); throw std::runtime_error("Cannot install startup intervention exception handler"); }
        bool frame_attempted = false;
        // The fingerprints above hold the sites' first bytes, which patch_breakpoint checks again.
        if (patch_breakpoint("startup exit", exit_address, startup_exit_instruction[0]) != HookOk)
            error = "Cannot arm the guarded startup-exit intervention";
        if (error.empty()) {
            frame_attempted = true;
            if (patch_breakpoint("startup frame capture", frame_address, startup_frame_instruction[0]) != HookOk)
                error = "Cannot arm the guarded frame-capture intervention";
        }
        if (error.empty() && !machine.arm()) error = "Cannot arm the startup intervention state machine";
        if (!error.empty()) {
            if (frame_attempted) restore_opcode(frame_address, startup_frame_instruction[0]);
            restore_opcode(exit_address, startup_exit_instruction[0]);
            patch_breakpoint_handler_remove(handler_handle);
            handler_handle = nullptr;
            installed_base.store(0);
            machine.fail();
            return false;
        }
        logging::log(logging::Level::info, logging::Channel::runtime,
            "Startup interventions armed: frame {:#x}, exit {:#x}", startup_frame_rva, startup_exit_rva);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); machine.fail(); return false; }
}
} // namespace dingosdk
