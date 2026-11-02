#pragma once
#include "Engine/Game/Build/fingerprint.h"
#include <array>
#include <cstdint>

// Engine work the developers left on the table (Extension/Rendering/engine_tweaks.cpp, `perf`
// toggles; analysis/sim-performance.md "Engine code").
namespace dingosdk::game::build::v20260929::engine_tweaks {
// 1. Enlighten GI's per-frame update(manager, mode): runs on the render thread every rendered
// frame and dispatches the probe-set solves (SolveEntireProbeSetTask 0x54d5090, SolveIrradianceTask
// 0x54c7350). Its own skip path (manager + 0x18 bits 0/1) returns at once with nothing done.
// The GI frame (0x5196270) calls it once a frame (gi_update_frame_return), or, while a setting
// (+0x13d of the object its +0x250 getter returns) is on, settings + 0x78 times in a row
// (gi_update_loop_return).
inline constexpr Fingerprint gi_update{0x518c4c0, {
    0x40,0x55,0x41,0x55,0x41,0x56,0x48,0x8d,0xac,0x24,0x90,0xf6,0xff,0xff,0x48,0x81,
    0xec,0x70,0x0a,0x00,0x00,0x48,0x8b,0x05,0xe4,0x7e,0x03,0x02,0x48,0x33,0xc4,0x48}};
inline constexpr std::uintptr_t gi_update_frame_return = 0x51966f4;
inline constexpr std::uintptr_t gi_update_loop_return = 0x519660c;

// 2. meshCull's scheduler, run once per frame before the cull jobs; 11 arguments (rcx, rdx, r9
// and stack slots 5, 6 and 11 are read; no floating-point ones). It and the job (0x4eecaa0)
// each read WorldRenderSettings.MeshCullTreeEnabled (+0x95d): 0 = flat path testing every chunk
// against every view, 1 = octree pre-cull (meshCullPrepare 0x4eb2b70). The settings come from
// settings_lookup(*settings_manager, world_render_settings_type).
inline constexpr Fingerprint mesh_cull_schedule{0x4eb6620, {
    0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
    0x48,0x8d,0xac,0x24,0xe0,0xfd,0xff,0xff,0x48,0x81,0xec,0x20,0x03,0x00,0x00,0x48}};
inline constexpr std::uintptr_t settings_lookup = 0x18d6ef0;
inline constexpr std::uintptr_t settings_manager = 0x7482ce0;
inline constexpr std::uintptr_t world_render_settings_type = 0x78adf88;
inline constexpr std::uintptr_t mesh_cull_tree_enabled = 0x95d;

// 3. Job wake-ups. push(scheduler, job): if scheduler + 0x271 runs the job at once (execute
// 0x321ed00 on scheduler + 0x2c0); else pushes it (lockfree_push(scheduler + 0x10, job, job + 0x40))
// and wakes the first worker, in bit order, of the waiter mask (scheduler + 0x2a8) that wake()
// accepts. Worker states (worker_table + i * worker_stride + worker_state): 3 running, 4 entering
// idle (wake = one CAS 4 -> 3), 5 asleep (wake = CAS 5 -> 3 and a semaphore release). The idle
// wait (0x3228220) sets 3 -> 4, calls register_idle (no arguments), which puts the worker's bit in
// its schedulers' and queues' masks and returns non-zero if work was already there; on 0 it sets
// 4 -> 5 and sleeps, or, when a pusher got there first (state 3), unregisters and runs again.
// The worker is read from the thread's TLS block (gs:[0x58][*tls_index] + tls_worker).
inline constexpr Fingerprint job_push{0x321d710, {
    0x40,0x53,0x48,0x83,0xec,0x20,0x80,0xb9,0x71,0x02,0x00,0x00,0x00,0x48,0x8b,0xd9,
    0x75,0x6a,0x4c,0x8d,0x42,0x40,0x48,0x89,0x74,0x24,0x38,0x48,0x83,0xc1,0x10,0xe8}};
inline constexpr Fingerprint job_register_idle{0x3227290, {
    0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x28,0x8b,0x0d,0x41,0xce,0x6a,0x04,0x65,
    0x48,0x8b,0x04,0x25,0x58,0x00,0x00,0x00,0xbd,0x19,0x0b,0x00,0x00,0x48,0x8b,0x1c}};
inline constexpr std::uintptr_t job_wake = 0x3228400;
inline constexpr std::uintptr_t job_lockfree_push = 0x11eb850;
inline constexpr std::uintptr_t job_execute = 0x321ed00;
inline constexpr std::uintptr_t job_scheduler_immediate = 0x271;
inline constexpr std::uintptr_t job_scheduler_queue = 0x10;
inline constexpr std::uintptr_t job_scheduler_inline = 0x2c0;
inline constexpr std::uintptr_t job_scheduler_waiters = 0x2a8;
inline constexpr std::uintptr_t job_link = 0x40;
inline constexpr std::uintptr_t worker_table = 0x7607770;
inline constexpr std::uintptr_t worker_stride = 0x178;
inline constexpr std::uintptr_t worker_state = 0x5c;
inline constexpr std::uintptr_t tls_index = 0x78d40e0;
inline constexpr std::uintptr_t tls_worker = 0x6c8;

// 4. Precise sleep(timer, nanoseconds): an auto-reset, non-high-resolution waitable timer (timer +
// 8, made by 0x18b0220) for all but the last 1 ms (0x73c7ba0, from timeGetDevCaps), then a spin,
// at thread priority 15 while it waits; clears and obeys an abort flag (timer + 0x10) on every
// pass. The abort (0x18bd9d0) sets the flag and fires the timer at once. The main loop (0x4645080)
// calls it with what is left of its 10 ms budget; main_sleep_return is that call's return address.
inline constexpr Fingerprint precise_sleep{0x18bcc30, {
    0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xc9,0x48,0x81,0xec,0xa0,
    0x00,0x00,0x00,0x4c,0x8b,0x35,0x26,0xb1,0xb0,0x05,0x48,0x8b,0xda,0x48,0x8b,0xf1}};
inline constexpr std::uintptr_t main_sleep_return = 0x4645328;
inline constexpr std::uintptr_t sleep_timer_handle = 0x8;
inline constexpr std::uintptr_t sleep_abort = 0x10;

// 5. Expression shader section dirty-bit clear(section), every frame for every section (only
// caller 0x505a9a0): takes a byte spinlock (+0x16) to clear the dirty bits -- bits 0-17 of the
// dword at +0x18 when the list at +0x128..+0x130 is empty, else bits 0-5 of the word at +0x1c --
// and only does more (sets bit 18, notifies) when some were set. With none set it writes back
// the same values: no effect but the lock.
inline constexpr Fingerprint section_dirty_clear{0x506d620, {
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x81,0x30,0x01,0x00,0x00,0x48,0x8b,0xd9,
    0x48,0x39,0x81,0x28,0x01,0x00,0x00,0xb9,0x01,0x00,0x00,0x00,0x74,0x32,0x66,0x90}};
inline constexpr std::uintptr_t section_list_begin = 0x128;
inline constexpr std::uintptr_t section_list_end = 0x130;
inline constexpr std::uintptr_t section_dirty = 0x18;            // dword
inline constexpr std::uint32_t section_dirty_mask = 0x3ffff;
inline constexpr std::uintptr_t section_list_dirty = 0x1c;       // word
inline constexpr std::uint16_t section_list_dirty_mask = 0x3f;

// 6. Simulation stall guards under Wine on macOS (Rosetta 2 + hybrid P/E cores):
// (a) TaskQueue::runnerLoop (0x3225d00): when a runner sees an empty ring buffer (+0xc0) and
// decrements the active runner count (+0x14c) via `lock xaddl [rax+0x14c], -1`, it only re-reads
// the ring buffer after the barrier when the old runner count was 1 (`cmp ecx, 1; jne exit`).
// When two runners exit concurrently while EntityGraph pushes tasks (0x184d630, which skips
// spawning a runner whenever +0x14c != 0), skipping the post-barrier check strands tasks in the
// queue with 0 active runners and stalls GameSimulationLoop (GameStall 0x18e13d2). Replacing the
// 6-byte `jne` at +16 with a 6-byte nop makes every exiting runner re-check +0xc0 after `xaddl`.
inline constexpr std::uintptr_t task_queue_runner_exit_rva = 0x3225d98;
inline constexpr std::array<unsigned char, 32> task_queue_runner_exit_contract{
    0x48,0x8b,0x03,                          // mov rax,[rbx]
    0x8b,0xcf,                               // mov ecx,edi (-1)
    0xf0,0x0f,0xc1,0x88,0x4c,0x01,0x00,0x00, // lock xadd [rax+0x14c],ecx
    0x83,0xf9,0x01,                          // cmp ecx,1
    0x0f,0x85,0x96,0x01,0x00,0x00,           // jne exit (0x3225f44)
    0x48,0x8b,0x03,                          // mov rax,[rbx]
    0x8b,0x88,0xc0,0x00,0x00,0x00,           // mov ecx,[rax+0xc0]
    0x90};                                   // nop
inline constexpr std::uintptr_t task_queue_runner_exit_patch_offset = 16;
inline constexpr std::array<unsigned char, 6> task_queue_runner_exit_patch{
    0x0f,0x1f,0x40,0x00,0x66,0x90};          // 6-byte nop

// (b) Server step shared reader lock (0x4db7ad0): tests `eax & 0xffff0000` (writer active/waiting)
// at 0x4db7ae9 and then executes a single `lock cmpxchg [rcx+0x11640], edx` without a retry loop.
// If two readers race on +0x11640 while no writer holds the lock, the losing reader jumps to the
// writer-wait path (0x4db7bad), which queues a retry job on +0x11648; with no writer active
// (+0x11650 == 0), 0x321e620 returns without scheduling the retry job and leaks the Server step
// continuation token. Retargeting the 6-byte `jne` at +22 back to +0 (`jne 0x4db7ae9`) retries
// the reader CAS until it succeeds or a writer sets the upper 16 bits.
inline constexpr std::uintptr_t server_step_reader_lock_rva = 0x4db7ae9;
inline constexpr std::array<unsigned char, 28> server_step_reader_lock_contract{
    0xa9,0x00,0x00,0xff,0xff,                // retry: test eax,0xffff0000
    0x0f,0x85,0xb9,0x00,0x00,0x00,           // jne wait_for_writer (0x4db7bad)
    0x8d,0x50,0x01,                          // lea edx,[rax+1]
    0xf0,0x0f,0xb1,0x91,0x40,0x16,0x01,0x00, // lock cmpxchg [rcx+0x11640],edx
    0x0f,0x85,0xa8,0x00,0x00,0x00};          // jne wait_for_writer (0x4db7bad)
inline constexpr std::uintptr_t server_step_reader_lock_patch_offset = 22;
inline constexpr std::array<unsigned char, 6> server_step_reader_lock_patch{
    0x75,0xe8,                               // jne retry (0x4db7ae9)
    0x0f,0x1f,0x40,0x00};                    // 4-byte nop
static_assert(static_cast<signed char>(server_step_reader_lock_patch[1]) ==
              0 - static_cast<int>(server_step_reader_lock_patch_offset + 2));
}
