#include <Windows.h>
#include <winhttp.h>
#include "../https_download.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace fake_http {
enum class Stage { none, send, receive, read, eof, close };
struct Handle {
    DWORD_PTR context{};
    WINHTTP_STATUS_CALLBACK callback{};
    DWORD notifications{};
    bool closed{};
};
struct State {
    dingosdk::https::Watch* watch{};
    Stage stop_at{Stage::none}, fail_at{Stage::none};
    bool timeout{}, callback_error{}, content_length{true}, downgrade{};
    unsigned opens{}, reads{}, closing_callbacks{}, pending_buffer_closes{};
    DWORD redirect{}, request_flags{}, session_flags{};
    void* pending_buffer{};
    Handle session, connection, request;
    std::string body{"payload"};
};
State state;

void stop() {
    if (state.timeout) state.watch->deadline_us.store(1);
    else state.watch->cancel.store(true);
}
void notify(DWORD status, void* info = nullptr, DWORD size = 0) {
    state.request.callback(&state.request, state.request.context, status, info, size);
}
bool pending(Stage stage) {
    if (state.stop_at != stage) return false;
    stop();
    return true;
}
bool failure(Stage stage) {
    if (state.fail_at != stage) return false;
    if (state.callback_error) {
        WINHTTP_ASYNC_RESULT error{};
        error.dwError = ERROR_WINHTTP_CONNECTION_ERROR;
        notify(WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, &error, sizeof(error));
    } else {
        SetLastError(ERROR_WINHTTP_CONNECTION_ERROR);
    }
    return true;
}
HINTERNET WINAPI open(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD flags) {
    ++state.opens;
    state.session_flags = flags;
    return &state.session;
}
HINTERNET WINAPI connect(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD) {
    return &state.connection;
}
HINTERNET WINAPI open_request(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD flags) {
    state.request_flags = flags;
    return &state.request;
}
BOOL WINAPI set_timeouts(HINTERNET, int, int, int, int) { return TRUE; }
BOOL WINAPI set_option(HINTERNET handle, DWORD option, LPVOID value, DWORD) {
    if (option == WINHTTP_OPTION_CONTEXT_VALUE)
        static_cast<Handle*>(handle)->context = *static_cast<DWORD_PTR*>(value);
    if (option == WINHTTP_OPTION_REDIRECT_POLICY) state.redirect = *static_cast<DWORD*>(value);
    return TRUE;
}
WINHTTP_STATUS_CALLBACK WINAPI set_callback(HINTERNET handle, WINHTTP_STATUS_CALLBACK callback,
                                           DWORD notifications, DWORD_PTR) {
    auto& request = *static_cast<Handle*>(handle);
    const auto previous = request.callback;
    request.callback = callback;
    request.notifications = notifications;
    return previous;
}
BOOL WINAPI send(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR context) {
    state.request.context = context;
    if (pending(Stage::send)) return TRUE;
    if (failure(Stage::send)) return state.callback_error ? TRUE : FALSE;
    notify(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    return TRUE;
}
BOOL WINAPI receive(HINTERNET, LPVOID) {
    if (pending(Stage::receive)) return TRUE;
    if (failure(Stage::receive)) return state.callback_error ? TRUE : FALSE;
    if (state.downgrade && state.redirect == WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP) {
        SetLastError(ERROR_WINHTTP_REDIRECT_FAILED);
        return FALSE;
    }
    notify(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
    return TRUE;
}
BOOL WINAPI query(HINTERNET, DWORD query, LPCWSTR, LPVOID value, LPDWORD size, LPDWORD) {
    if (query == (WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER)) {
        *static_cast<DWORD*>(value) = 200;
    } else if (query == (WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER) && state.content_length) {
        *static_cast<DWORD*>(value) = static_cast<DWORD>(state.body.size());
    } else {
        SetLastError(ERROR_WINHTTP_HEADER_NOT_FOUND);
        return FALSE;
    }
    *size = sizeof(DWORD);
    return TRUE;
}
BOOL WINAPI read(HINTERNET, LPVOID buffer, DWORD capacity, LPDWORD count) {
    // The production async path must receive the count through its callback.
    if (count || capacity < state.body.size()) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (++state.reads == 1) {
        std::memcpy(buffer, state.body.data(), state.body.size());
        notify(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, buffer, static_cast<DWORD>(state.body.size()));
    } else {
        if (state.stop_at == Stage::read) {
            state.pending_buffer = buffer;
            stop();
            return TRUE;
        }
        if (failure(Stage::read)) return state.callback_error ? TRUE : FALSE;
        if (state.stop_at == Stage::eof) stop();
        notify(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, buffer, 0);
    }
    return TRUE;
}
BOOL WINAPI close(HINTERNET handle) {
    auto& object = *static_cast<Handle*>(handle);
    if (object.closed) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    object.closed = true;
    if (handle == &state.request && object.callback) {
        if (state.pending_buffer) {
            // Pending I/O can still touch the caller's buffer during closure.
            *static_cast<char*>(state.pending_buffer) = 'x';
            ++state.pending_buffer_closes;
            state.pending_buffer = nullptr;
        }
        if (state.stop_at == Stage::close) stop();
        if (object.context && (object.notifications & WINHTTP_CALLBACK_FLAG_HANDLES)) {
            ++state.closing_callbacks;
            notify(WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING);
        }
    }
    return TRUE;
}
}

#define WinHttpOpen fake_http::open
#define WinHttpConnect fake_http::connect
#define WinHttpOpenRequest fake_http::open_request
#define WinHttpSetTimeouts fake_http::set_timeouts
#define WinHttpSetOption fake_http::set_option
#define WinHttpSetStatusCallback fake_http::set_callback
#define WinHttpSendRequest fake_http::send
#define WinHttpReceiveResponse fake_http::receive
#define WinHttpQueryHeaders fake_http::query
#define WinHttpReadData fake_http::read
#define WinHttpCloseHandle fake_http::close
#include "../https_download.cpp"
#undef WinHttpOpen
#undef WinHttpConnect
#undef WinHttpOpenRequest
#undef WinHttpSetTimeouts
#undef WinHttpSetOption
#undef WinHttpSetStatusCallback
#undef WinHttpSendRequest
#undef WinHttpReceiveResponse
#undef WinHttpQueryHeaders
#undef WinHttpReadData
#undef WinHttpCloseHandle

namespace {
int failures{};
void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Fixtures {
    std::filesystem::path root;
    bool created;
    explicit Fixtures(std::filesystem::path path) : root(std::move(path)),
        created(std::filesystem::create_directory(root)) {}
    ~Fixtures() {
        if (created) {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    }
};
void reset(dingosdk::https::Watch& watch) {
    fake_http::state = {};
    fake_http::state.watch = &watch;
    watch.cancel.store(false);
    watch.deadline_us.store(0);
    watch.received.store(0);
    watch.total.store(0);
}
}

int main(int argc, char** argv) {
    using namespace dingosdk::https;
    using fake_http::Stage;
    Fixtures fixtures(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path() /
        ("https-download-timeout-fixtures-" + std::to_string(GetCurrentProcessId())));
    if (!fixtures.created) {
        std::cerr << "Fixture directory already exists; refusing to overwrite it\n";
        return 1;
    }
    const auto path = fixtures.root / "download.bin";
    Watch watch;
    const auto get = [&](std::uint64_t limit = 1024, std::uint32_t seconds = 5) {
        return dingosdk::https::get(L"https://example.invalid/file", path, limit, seconds, L"test", &watch);
    };
    const auto removed = [&] { return !std::filesystem::exists(path); };
    const auto closed = [&] {
        return fake_http::state.closing_callbacks == 1 && fake_http::state.request.closed &&
            fake_http::state.connection.closed && fake_http::state.session.closed;
    };

    reset(watch);
    auto result = get();
    check(result.ok && result.http_status == 200 && result.error == 0, "actual GET succeeds");
    check(contents(path) == "payload" && watch.received.load() == 7 && watch.total.load() == 7,
          "successful bytes and progress are retained");
    check(closed(), "successful request receives final closing callback");
    check(fake_http::state.session_flags & WINHTTP_FLAG_ASYNC, "session uses asynchronous WinHTTP");
    check(fake_http::state.request_flags & WINHTTP_FLAG_SECURE, "request stays HTTPS");
    check(fake_http::state.redirect == WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP,
          "HTTPS downgrade redirect policy is configured");
    std::filesystem::remove(path);

    for (const auto stage : {Stage::send, Stage::receive, Stage::read, Stage::eof, Stage::close}) {
        for (const bool timeout : {false, true}) {
            reset(watch);
            fake_http::state.stop_at = stage;
            fake_http::state.timeout = timeout;
            result = get();
            check(!result.ok && result.error == static_cast<DWORD>(timeout ? ERROR_TIMEOUT : ERROR_CANCELLED),
                  "pending operation or final completion reports the authoritative stop reason");
            check(removed(), "stopped GET removes only its created output, including partial bytes");
            check(closed(), "stopped request completes handle-closing lifetime handshake");
            if (stage == Stage::read)
                check(watch.received.load() == 7 && fake_http::state.pending_buffer_closes == 1,
                      "partial output is cleaned while pending read buffer survives close");
        }
    }

    for (const auto stage : {Stage::send, Stage::receive, Stage::read}) {
        for (const bool callback_error : {false, true}) {
            reset(watch);
            fake_http::state.fail_at = stage;
            fake_http::state.callback_error = callback_error;
            result = get();
            check(!result.ok && result.error == ERROR_WINHTTP_CONNECTION_ERROR,
                  "immediate and callback API errors are reported");
            check(removed() && closed(), "API errors clean up output and close registered request");
        }
    }

    reset(watch);
    watch.deadline_us.store(1);
    result = get();
    check(!result.ok && result.error == ERROR_TIMEOUT && fake_http::state.opens == 0 && removed(),
          "already expired external deadline prevents network and output creation");
    reset(watch);
    watch.deadline_us.store(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) + 60000000);
    result = get(1024, 0);
    check(!result.ok && result.error == ERROR_TIMEOUT && fake_http::state.opens == 0,
          "internal timeout wins over a later external steady-clock deadline");
    reset(watch);
    watch.cancel.store(true);
    watch.deadline_us.store(1);
    result = get();
    check(!result.ok && result.error == ERROR_CANCELLED && fake_http::state.opens == 0,
          "explicit cancellation wins when both stop conditions hold");

    reset(watch);
    {
        std::ofstream output(path, std::ios::binary);
        output << "existing destination";
    }
    result = get();
    check(!result.ok && contents(path) == "existing destination",
          "CREATE_NEW failure preserves a pre-existing destination");
    check(closed(), "existing-destination failure still closes the request");
    std::filesystem::remove(path);

    for (const bool content_length : {false, true}) {
        reset(watch);
        fake_http::state.content_length = content_length;
        result = get(3);
        check(!result.ok && result.error == ERROR_FILE_TOO_LARGE && removed() && closed(),
              "announced and streamed oversized bodies remain rejected and cleaned up");
    }
    reset(watch);
    fake_http::state.downgrade = true;
    result = get();
    check(!result.ok && result.error == ERROR_WINHTTP_REDIRECT_FAILED && removed() && closed(),
          "HTTPS-to-HTTP redirects remain refused");
    reset(watch);
    fake_http::state.body.clear();
    result = get();
    check(!result.ok && result.error == ERROR_BAD_LENGTH && removed() && closed(),
          "empty body remains rejected and its created output is removed");

    reset(watch);
    fake_http::state.stop_at = Stage::send;
    const auto text = get_text(L"https://example.invalid/metadata", 1024, 5, L"test", &result, &watch);
    check(!text && !result.ok && result.error == ERROR_CANCELLED && closed(),
          "get_text forwards its Watch into the actual asynchronous GET");

    if (failures) return 1;
    std::cout << "HTTPS deadline/cancellation tests passed\n";
    return 0;
}
