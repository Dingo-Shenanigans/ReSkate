#include "app_audio_capture.h"
#include "audio_dsp.h"
#include <Windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "mmdevapi")
#pragma comment(lib, "ole32")

namespace dingosdk::app_audio {
namespace {
// One writer (the capture thread), one reader (the game's audio thread): interleaved stereo
// floats at capture_rate. A little over a second.
constexpr std::uint32_t ring_frames = 65536; // a power of two
std::array<float, ring_frames * 2> ring{};
std::atomic<std::uint64_t> written{}, taken{}; // frames, ever
struct Reader {
    std::atomic<std::uint64_t> key{}, used{}; // whose it is (0: nobody's), and when it last read
    std::atomic<double> position{};           // its place, in frames, with its fraction
};
std::array<Reader, 8> readers{};
std::atomic<std::uint64_t> starved_frames{}, read_frames{};
std::atomic<float> peak{};
std::atomic<bool> running{}, stopping{}, capturing{}, want_mute{}, is_muted{};
std::atomic<unsigned long> source_process{};
std::thread worker;
std::mutex text_mutex;
std::string problem_text;
std::wstring executable_name;

// The reader keeps this far behind the writer: room for the capture's bursts.
constexpr double target_delay = capture_rate * 0.08, most_delay = capture_rate * 0.30;

void say(const std::string& text) {
    std::lock_guard lock(text_mutex);
    problem_text = text;
}

template <class T> struct Com {
    T* p{};
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Every process of the application, the one the others descend from first.
std::vector<DWORD> processes_of(const std::wstring& name) {
    std::vector<DWORD> all, parents;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return all;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) {
            all.push_back(entry.th32ProcessID);
            parents.push_back(entry.th32ParentProcessID);
        }
    CloseHandle(snapshot);
    for (std::size_t i = 0; i < all.size(); ++i)
        if (std::find(all.begin(), all.end(), parents[i]) == all.end()) {
            std::swap(all[0], all[i]);
            break;
        }
    return all;
}

// The answer to ActivateAudioInterfaceAsync, which comes on a thread of the system's.
struct Activation : IActivateAudioInterfaceCompletionHandler, IAgileObject {
    std::atomic<ULONG> references{1};
    HANDLE done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT result{E_FAIL};
    IAudioClient* client{};
    virtual ~Activation() { if (done) CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler)) *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (id == __uuidof(IAgileObject)) *out = static_cast<IAgileObject*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references;
        if (!left) delete this;
        return left;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activated = E_FAIL;
        IUnknown* unknown{};
        result = operation->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(result)) result = activated;
        if (SUCCEEDED(result) && unknown) result = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
        if (unknown) unknown->Release();
        SetEvent(done);
        return S_OK;
    }
};

IAudioClient* loopback_of(DWORD process, HRESULT& result) {
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = process;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT variant{};
    variant.vt = VT_BLOB;
    variant.blob.cbSize = sizeof(parameters);
    variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
    auto* handler = new Activation;
    Com<IActivateAudioInterfaceAsyncOperation> operation;
    result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &variant, handler, &operation);
    IAudioClient* client{};
    if (SUCCEEDED(result)) {
        result = WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0 ? handler->result : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(result)) client = handler->client;
    }
    handler->Release();
    return client;
}

// Windows takes the capture after the application's volume, so muting it would silence the
// capture as well. Its volume is turned down this far instead (it cannot be heard), and what
// is captured, which is floating point and loses nothing by it, is turned up by as much.
constexpr float quiet_volume = 0.001f;

// The application's own sound to the speakers, turned down or put back: its sessions on the
// default output. A session found turned down that this did not turn down (the game ended
// without putting it back) is put back to full.
bool set_mute(const std::vector<DWORD>& processes, bool mute) {
    static float before = 1.0f;
    Com<IMMDeviceEnumerator> devices;
    Com<IMMDevice> device;
    Com<IAudioSessionManager2> manager;
    Com<IAudioSessionEnumerator> sessions;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices))) ||
        FAILED(devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) ||
        FAILED(manager->GetSessionEnumerator(&sessions)))
        return false;
    int count{};
    sessions->GetCount(&count);
    bool any{};
    for (int i = 0; i < count; ++i) {
        Com<IAudioSessionControl> control;
        Com<IAudioSessionControl2> detail;
        Com<ISimpleAudioVolume> volume;
        DWORD owner{};
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&detail))) ||
            FAILED(detail->GetProcessId(&owner)) || std::find(processes.begin(), processes.end(), owner) == processes.end() ||
            FAILED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))))
            continue;
        float current = 1.0f;
        volume->GetMasterVolume(&current);
        if (mute) {
            if (current > quiet_volume * 2) before = current;
            any = (current <= quiet_volume * 2 || SUCCEEDED(volume->SetMasterVolume(quiet_volume, nullptr))) || any;
        } else if (current <= quiet_volume * 2) {
            any = SUCCEEDED(volume->SetMasterVolume(before, nullptr)) || any;
        } else {
            any = true;
        }
    }
    return any;
}

std::atomic<bool> external{};
std::atomic<unsigned> source_rate{capture_rate};

// Puts sound into the ring, turned up by `gain` and shaped as the player set it (audio_dsp.h);
// silence when there are no samples. How loud the loudest of it was.
float store(const float* samples, unsigned frames, float gain, unsigned rate) {
    auto at = written.load(std::memory_order_relaxed);
    float loudest = 0, piece[1024];
    for (unsigned done = 0; done < frames;) {
        const unsigned count = std::min(frames - done, 512u);
        for (unsigned i = 0; i < count * 2; ++i) piece[i] = samples ? std::clamp(samples[done * 2 + i] * gain, -1.0f, 1.0f) : 0.0f;
        audio_dsp::process(piece, count, rate);
        for (unsigned i = 0; i < count; ++i, ++at) {
            ring[(at & (ring_frames - 1)) * 2] = piece[i * 2];
            ring[(at & (ring_frames - 1)) * 2 + 1] = piece[i * 2 + 1];
            loudest = std::max({loudest, std::fabs(piece[i * 2]), std::fabs(piece[i * 2 + 1])});
        }
        done += count;
    }
    written.store(at, std::memory_order_release);
    return loudest;
}

void write_frames(const BYTE* data, UINT32 frames, bool silent) {
    if (external.load(std::memory_order_relaxed)) return; // (the sound is another's for now)
    float raw = 0;
    const auto* samples = reinterpret_cast<const float*>(data);
    if (!silent)
        for (UINT32 i = 0; i < frames * 2; ++i) raw = std::max(raw, std::fabs(samples[i]));
    // Turned up by what the application was turned down by, unless this piece plainly was not
    // turned down (the change had not reached it yet): then it is left as it is.
    float gain = is_muted.load(std::memory_order_relaxed) ? 1.0f / quiet_volume : 1.0f;
    if (raw * gain > 1.5f) gain = 1.0f;
    const float loudest = store(silent ? nullptr : samples, frames, gain, capture_rate);
    // (A slow fall, so the level reads as the last second's.)
    const auto before = peak.load(std::memory_order_relaxed);
    peak.store(std::max(loudest, before * 0.97f), std::memory_order_relaxed);
}

// The application to capture (it can be changed while this runs: target()).
std::wstring wanted_name() {
    std::lock_guard lock(text_mutex);
    return executable_name;
}

// One capture, from finding the application to its end, a failure or another being wanted.
void capture_once() {
    const auto name = wanted_name();
    const auto processes = processes_of(name);
    if (processes.empty()) {
        source_process.store(0);
        say("the application is not running");
        return;
    }
    source_process.store(processes.front());
    HRESULT result{};
    Com<IAudioClient> client;
    client.p = loopback_of(processes.front(), result);
    if (!client) {
        char text[96];
        sprintf_s(text, "its sound could not be opened (0x%08lx; this needs Windows 10 version 2004 or later)", static_cast<unsigned long>(result));
        say(text);
        return;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = capture_rate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = capture_rate * 8;
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Com<IAudioCaptureClient> capture;
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 0, 0, &format, nullptr);
    if (SUCCEEDED(result)) result = client->SetEventHandle(event);
    if (SUCCEEDED(result)) result = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (SUCCEEDED(result)) result = client->Start();
    if (FAILED(result)) {
        char text[64];
        sprintf_s(text, "its sound could not be started (0x%08lx)", static_cast<unsigned long>(result));
        say(text);
        CloseHandle(event);
        return;
    }
    say({});
    capturing.store(true);
    ULONGLONG checked = GetTickCount64();
    bool muted_now = false;
    while (!stopping.load()) {
        WaitForSingleObject(event, 200);
        for (;;) {
            BYTE* data{};
            UINT32 frames{};
            DWORD flags{};
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)) || !frames) break;
            write_frames(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            capture->ReleaseBuffer(frames);
        }
        // Its mute follows what is asked, and is set again for a session that came since.
        const bool mute = want_mute.load();
        const auto now = GetTickCount64();
        if (mute != muted_now || (mute && now - checked > 2000)) {
            checked = now;
            const auto current = processes_of(name);
            if (current.empty()) break; // it closed
            if (set_mute(current, mute)) muted_now = mute;
            is_muted.store(muted_now);
        } else if (now - checked > 2000) {
            checked = now;
            if (processes_of(name).empty()) break;
        }
        if (wanted_name() != name) break; // another application is wanted
    }
    client->Stop();
    if (muted_now) {
        set_mute(processes_of(name), false);
        is_muted.store(false);
    }
    capturing.store(false);
    CloseHandle(event);
}

void run() noexcept {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    while (!stopping.load()) {
        try {
            capture_once();
        } catch (...) {
            capturing.store(false);
        }
        const auto before = wanted_name();
        for (int i = 0; i < 20 && !stopping.load() && wanted_name() == before; ++i) Sleep(100); // then it is looked for again
    }
    if (initialized) CoUninitialize();
    running.store(false);
}
} // namespace

void start(const wchar_t* executable) noexcept {
    if (running.exchange(true)) return;
    stopping.store(false);
    try {
        {
            std::lock_guard lock(text_mutex);
            executable_name = executable;
        }
        worker = std::thread(run);
    } catch (...) {
        running.store(false);
    }
}
void target(const wchar_t* executable) noexcept {
    try {
        {
            std::lock_guard lock(text_mutex);
            if (running.load() && executable_name == executable) return;
            executable_name = executable;
        }
        start(executable);
    } catch (...) {}
}
void stop() noexcept {
    if (!running.load()) return;
    stopping.store(true);
    if (worker.joinable()) worker.join();
}
void mute_source(bool mute) noexcept { want_mute.store(mute); }
void restore_source(const wchar_t* executable) noexcept {
    try {
        std::thread([name = std::wstring(executable)] {
            const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            try {
                if (const auto processes = processes_of(name); !processes.empty()) set_mute(processes, false);
            } catch (...) {}
            if (initialized) CoUninitialize();
        }).detach();
    } catch (...) {}
}

unsigned read(std::uint64_t key, float* left, float* right, unsigned frames, float rate) noexcept {
    const auto end = written.load(std::memory_order_acquire);
    const auto now = GetTickCount64();
    if (!key) key = 1;
    Reader* reader{};
    for (auto& candidate : readers)
        if (candidate.key.load(std::memory_order_relaxed) == key) {
            reader = &candidate;
            break;
        }
    if (!reader)
        for (auto& candidate : readers) {
            auto held = candidate.key.load(std::memory_order_relaxed);
            if (held && now - candidate.used.load(std::memory_order_relaxed) < 1000) continue;
            if (candidate.key.compare_exchange_strong(held, key, std::memory_order_relaxed)) {
                candidate.position.store(-1.0, std::memory_order_relaxed); // (its place is taken up below)
                reader = &candidate;
                break;
            }
        }
    if (!reader) {
        for (unsigned i = 0; i < frames; ++i) left[i] = right[i] = 0.0f;
        starved_frames.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }
    reader->used.store(now, std::memory_order_relaxed);
    double at = reader->position.load(std::memory_order_relaxed);
    const double step = rate > 1000.0f ? static_cast<double>(source_rate.load(std::memory_order_relaxed)) / static_cast<double>(rate) : 1.0;
    // Too far behind (it was not read for a while) or ahead of the writer (the capture began
    // again): take up the place a little behind the writer.
    const double behind = static_cast<double>(end) - at;
    if (behind > most_delay || behind < 0 || at < 0) at = std::max(0.0, static_cast<double>(end) - target_delay);
    unsigned real{};
    for (unsigned i = 0; i < frames; ++i) {
        const auto whole = static_cast<std::uint64_t>(at);
        if (whole + 1 >= end) { // nothing more yet: silence, and the place is kept
            left[i] = right[i] = 0.0f;
            continue;
        }
        const float fraction = static_cast<float>(at - static_cast<double>(whole));
        const auto a = (whole & (ring_frames - 1)) * 2, b = ((whole + 1) & (ring_frames - 1)) * 2;
        left[i] = ring[a] + (ring[b] - ring[a]) * fraction;
        right[i] = ring[a + 1] + (ring[b + 1] - ring[a + 1]) * fraction;
        at += step;
        ++real;
    }
    reader->position.store(at, std::memory_order_relaxed);
    read_frames.fetch_add(real, std::memory_order_relaxed);
    if (real < frames) starved_frames.fetch_add(frames - real, std::memory_order_relaxed);
    return real;
}

void set_external(bool on, unsigned rate) noexcept {
    external.store(on, std::memory_order_relaxed);
    source_rate.store(on && rate ? rate : capture_rate, std::memory_order_relaxed);
}
void push(const float* interleaved, unsigned frames) noexcept {
    const float loudest = store(interleaved, frames, 1.0f, source_rate.load(std::memory_order_relaxed));
    peak.store(std::max(loudest, peak.load(std::memory_order_relaxed) * 0.97f), std::memory_order_relaxed);
}
std::uint64_t ahead_frames() noexcept {
    const auto end = written.load(std::memory_order_acquire);
    const auto now = GetTickCount64();
    double furthest = -1.0;
    for (const auto& reader : readers)
        if (reader.key.load(std::memory_order_relaxed) && now - reader.used.load(std::memory_order_relaxed) < 1000)
            furthest = std::max(furthest, reader.position.load(std::memory_order_relaxed));
    if (furthest < 0) return ~0ull;
    return static_cast<double>(end) > furthest ? end - static_cast<std::uint64_t>(furthest) : 0;
}

State state() {
    State out;
    out.capturing = capturing.load();
    out.process = source_process.load();
    out.frames = written.load();
    out.read = read_frames.load();
    out.starved = starved_frames.load();
    for (const auto now = GetTickCount64(); const auto& reader : readers)
        if (reader.key.load() && now - reader.used.load() < 1000) ++out.readers;
    out.level = peak.load();
    out.muted = is_muted.load();
    std::lock_guard lock(text_mutex);
    out.problem = problem_text;
    return out;
}
} // namespace dingosdk::app_audio
