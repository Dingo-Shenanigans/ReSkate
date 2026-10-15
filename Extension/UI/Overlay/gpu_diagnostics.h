#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <cstdint>

namespace dingosdk::overlay {
bool graphics_option_enabled(const wchar_t* name) noexcept;
void initialize_graphics_diagnostics() noexcept;
void record_device_failure(ID3D12Device* device, HRESULT result,
    const char* operation) noexcept;
// With RESKATE_GPU_DIAGNOSTICS on: records the failure the moment the device is removed, from a
// thread of its own, instead of when the overlay next draws. The game's own error dialog can
// otherwise come first and end the process before then.
void watch_device_removal(ID3D12Device* device) noexcept;
// With RESKATE_GPU_DIAGNOSTICS on: every DrawInstanced and DrawIndexedInstanced of the process (the
// runtime's, found through this command list) is checked, and one of more than ten million
// vertices or indices over its instances is said in the log with the code that recorded it. A
// count gone wrong makes a draw the GPU cannot finish. (Draws the GPU counts itself, through
// ExecuteIndirect, are not seen.)
void watch_draws(ID3D12GraphicsCommandList* list) noexcept;
// With RESKATE_GPU_DIAGNOSTICS on, before the game makes its device: every heap and resource the
// device makes is kept with the code that made it, and the code that freed it once it goes. A
// page fault's report (record_device_failure) then says what was at, or freed from, the address.
void watch_gpu_memory() noexcept;
void describe_gpu_address(std::uint64_t address) noexcept;
// With the GPU memory watch on: every GPU address the game binds for a draw or dispatch (vertex and
// index buffers, root buffer views), puts in a constant-buffer or acceleration-structure view, or
// builds a ray-tracing structure from is checked against the memory that exists. One in nothing
// is said in the log once per calling code, with what was there before.
void watch_addresses(ID3D12GraphicsCommandList* list) noexcept;
// The graphics card the game renders on and its driver's version: said in the log once, and kept
// for crash reports' tags (crash_report::set_gpu). Always on.
void record_gpu_identity(ID3D12Device* device) noexcept;
}
