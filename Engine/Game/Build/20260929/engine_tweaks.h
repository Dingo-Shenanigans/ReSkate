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
// at 0x4db7ae9 and then executes `lock cmpxchg [rcx+0x11640], edx`. When two readers race or a
// synchronous writer (0x4e9abd0) briefly holds bit 31 of +0x11640, jumping to the lock-free
// retry-job path (0x4db7bad) reads +0x11648 without holding +0x11610; once the writer clears
// +0x11650 (0x321f0a0 at 0x4e9ae4c), 0x321e620 returns without scheduling the retry job and leaks
// the continuation token. Spinning with `pause` until +0x11640 is writer-free and the reader CAS
// succeeds avoids the racy +0x11648 job-chain path completely.
inline constexpr std::uintptr_t server_step_reader_lock_rva = 0x4db7ae9;
inline constexpr std::array<unsigned char, 28> server_step_reader_lock_contract{
    0xa9,0x00,0x00,0xff,0xff,                // test eax,0xffff0000
    0x0f,0x85,0xb9,0x00,0x00,0x00,           // jne wait_for_writer (0x4db7bad)
    0x8d,0x50,0x01,                          // lea edx,[rax+1]
    0xf0,0x0f,0xb1,0x91,0x40,0x16,0x01,0x00, // lock cmpxchg [rcx+0x11640],edx
    0x0f,0x85,0xa8,0x00,0x00,0x00};          // jne wait_for_writer (0x4db7bad)
inline constexpr std::uintptr_t server_step_reader_lock_patch_offset = 0;
inline constexpr std::array<unsigned char, 28> server_step_reader_lock_patch{
    0xf3,0x90,                               // spin: pause
    0x8b,0x81,0x40,0x16,0x01,0x00,           // mov eax,[rcx+0x11640]
    0xa9,0x00,0x00,0xff,0xff,                // check: test eax,0xffff0000
    0x75,0xf1,                               // jne spin
    0x8d,0x50,0x01,                          // lea edx,[rax+1]
    0xf0,0x0f,0xb1,0x91,0x40,0x16,0x01,0x00, // lock cmpxchg [rcx+0x11640],edx
    0x75,0xec};                              // jne check

// (c) Job worker idle wait (0x3228220) and worker loops (0x3221db0, 0x3222210):
// When EA::Thread::Semaphore::Wait (0x272afc0) returns >= 0 (at 0x3228390 on the timed branch or
// 0x32283a8 on the infinite branch), 0x3228220 skips `lock cmpxchg [rbx+0x5c], 3` (5 -> 3) and
// jumps over the 4 -> 3 CAS at 0x32283d1 (`eb 0f` at 0x32283c8). If Wait consumes a leftover
// kernel semaphore token from a prior timeout/wake race, 0x3228220 returns with worker->state
// (+0x5c) still at 5 (asleep) or 4 (entering_idle); the worker loop (`cmp eax, 3; jne exit` at
// 0x3221e88 / 0x32222e9) treats any state other than 3 or 8 as a thread-shutdown signal and
// permanently terminates worker[0], causing GameStall (0x18e13d2) 60s later.
inline constexpr std::uintptr_t job_idle_wait_sem_exit_rva = 0x322838e;
inline constexpr std::array<unsigned char, 31> job_idle_wait_sem_exit_contract{
    0x85,0xc0,                               // test eax,eax
    0x79,0x1b,                               // jns 0x32283ad
    0x41,0x8b,0xc4,                          // mov eax,r12d (5)
    0xf0,0x44,0x0f,0xb1,0x7b,0x5c,           // lock cmpxchg [rbx+0x5c],r15d (3)
    0xeb,0x10,                               // jmp 0x32283ad
    0x48,0x8d,0x15,0x84,0x65,0x20,0x03,      // lea rdx,[rip+0x3206584] (-1)
    0x48,0x8d,0x4b,0x70,                     // lea rcx,[rbx+0x70]
    0xe8,0x13,0x2c,0x50,0xff};               // call 0x272afc0
inline constexpr std::uintptr_t job_idle_wait_sem_exit_patch_offset = 2;
inline constexpr std::array<unsigned char, 29> job_idle_wait_sem_exit_patch{
    0x66,0x90,                               // nop2 (always run 5 -> 3 CAS after timed wait)
    0x41,0x8b,0xc4,                          // cas_5_to_3: mov eax,r12d (5)
    0xf0,0x44,0x0f,0xb1,0x7b,0x5c,           // lock cmpxchg [rbx+0x5c],r15d (3)
    0xeb,0x10,                               // jmp 0x32283ad
    0x48,0x8d,0x4b,0x70,                     // inf_wait: lea rcx,[rbx+0x70] (rdx already -> -1)
    0xe8,0x1a,0x2c,0x50,0xff,                // call 0x272afc0
    0xeb,0xea,                               // jmp cas_5_to_3 (0x3228392)
    0x0f,0x1f,0x44,0x00,0x00};               // nop5

inline constexpr std::uintptr_t job_idle_wait_unreg_exit_rva = 0x32283bc;
inline constexpr std::array<unsigned char, 29> job_idle_wait_unreg_exit_contract{
    0x33,0xc9,                               // xor ecx,ecx
    0xe8,0xcd,0xb5,0xff,0xff,                // call 0x3223990
    0x4c,0x8b,0x64,0x24,0x68,                // mov r12,[rsp+0x68]
    0xeb,0x0f,                               // jmp 0x32283d9
    0x8b,0xc8,                               // mov ecx,eax
    0xe8,0xbf,0xb5,0xff,0xff,                // call 0x3223990
    0x8b,0xc6,                               // mov eax,esi (4)
    0xf0,0x44,0x0f,0xb1,0x7b,0x5c};          // lock cmpxchg [rbx+0x5c],r15d (3)
inline constexpr std::uintptr_t job_idle_wait_unreg_exit_patch_offset = 12;
inline constexpr std::array<unsigned char, 2> job_idle_wait_unreg_exit_patch{
    0xeb,0x07};                              // jmp 0x32283d1 (also run 4 -> 3 CAS)

inline constexpr std::uintptr_t worker_loop_inner_state_exit_rva = 0x32221be;
inline constexpr std::array<unsigned char, 32> worker_loop_inner_state_exit_contract{
    0x02,0x00,0x00,0x00,0xeb,0x1a,0xc7,0x46,0x08,0x04,0x00,0x00,0x00,0xeb,0x11,0xc7,
    0x46,0x08,0x06,0x00,0x00,0x00,0xeb,0x08,0x83,0xf8,0x09,0x75,0x03,0x89,0x56,0x08};
inline constexpr std::uintptr_t worker_loop_inner_state_exit_patch_offset = 27;
inline constexpr std::array<unsigned char, 2> worker_loop_inner_state_exit_patch{
    0x75,0xd3};                              // jne 0x32221ae (recover state -> 3 unless stopping == 9)

inline constexpr std::uintptr_t worker_loop_pred_state_exit_rva = 0x322264c;
inline constexpr std::array<unsigned char, 32> worker_loop_pred_state_exit_contract{
    0x02,0x00,0x00,0x00,0xeb,0x1e,0xc7,0x46,0x08,0x04,0x00,0x00,0x00,0xeb,0x15,0xc7,
    0x46,0x08,0x01,0x00,0x00,0x00,0xeb,0x0c,0x83,0xf8,0x09,0x75,0x07,0xc7,0x46,0x08};
inline constexpr std::uintptr_t worker_loop_pred_state_exit_patch_offset = 27;
inline constexpr std::array<unsigned char, 2> worker_loop_pred_state_exit_patch{
    0x75,0xd3};                              // jne 0x322263c (recover state -> 3 unless stopping == 9)
}
