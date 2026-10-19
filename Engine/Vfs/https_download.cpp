#include "https_download.h"
#include <Windows.h>
#include <winhttp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace dingosdk::https {
namespace {
std::uint64_t now_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    const std::filesystem::path& path;
    bool complete{};
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
            if (!complete) DeleteFileW(path.c_str());
        }
    }
};
struct Internet {
    HINTERNET value{};
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
struct AsyncRequest {
    HINTERNET value{};
    std::mutex mutex;
    std::condition_variable changed;
    DWORD expected{}, error{}, read{};
    bool completed{}, closed{};

    ~AsyncRequest() { close(); }

    void close() {
        if (!value) return;
        // Async closure interrupts pending I/O. Keep the context and read buffer
        // alive until WinHTTP's final callback, including on cancellation.
        WinHttpCloseHandle(value);
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return closed; });
        value = nullptr;
    }

    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD size) {
        if (!context) return;
        auto& request = *reinterpret_cast<AsyncRequest*>(context);
        std::lock_guard<std::mutex> lock(request.mutex);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            request.closed = true;
        } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            request.error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
            request.completed = true;
        } else if (status == request.expected) {
            if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) request.read = size;
            request.completed = true;
        }
        request.changed.notify_all();
    }

    void prepare(DWORD status) {
        std::lock_guard<std::mutex> lock(mutex);
        expected = status;
        completed = false;
        error = read = 0;
    }

    template<class Stop>
    DWORD wait(const Stop& stop) {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (const auto stopped = stop()) return stopped;
            if (completed) return error;
            changed.wait_for(lock, std::chrono::milliseconds(20));
        }
    }
};
}

Download get(std::wstring_view url, const std::filesystem::path& destination,
    std::uint64_t max_bytes, std::uint32_t timeout_seconds, const wchar_t* agent, Watch* watch) {
    Download result;
    const auto started = now_us();
    const auto timeout_deadline = started + std::uint64_t{timeout_seconds} * 1000000;
    const auto stop = [&]() -> DWORD {
        if (watch && watch->cancel.load()) return ERROR_CANCELLED;
        const auto external = watch ? watch->deadline_us.load() : 0;
        const auto deadline = external && external < timeout_deadline ? external : timeout_deadline;
        return now_us() >= deadline ? ERROR_TIMEOUT : ERROR_SUCCESS;
    };
    const auto fail = [&](DWORD error) {
        const auto stopped = stop();
        result.error = stopped ? stopped : error;
        return result;
    };
    if (const auto error = stop()) return fail(error);
    if (url.empty() || url.size() >= 4096 || url.find_first_of(L"\r\n\t #") != std::wstring_view::npos)
        return fail(ERROR_INVALID_PARAMETER);
    const std::wstring address(url);
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(address.c_str(), static_cast<DWORD>(address.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength)
        return fail(ERROR_INVALID_PARAMETER);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet session{WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, WINHTTP_FLAG_ASYNC)};
    if (!session.value) return fail(GetLastError());
    if (!WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000)) return fail(GetLastError());
    Internet connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value) return fail(GetLastError());
    Internet request{WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr, nullptr,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) return fail(GetLastError());
    // Declared before AsyncRequest so pending reads cannot outlive their buffer.
    std::array<char, 65536> buffer{};
    AsyncRequest async;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon))) return fail(GetLastError());
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&async);
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        return fail(GetLastError());
    if (WinHttpSetStatusCallback(request.value, AsyncRequest::callback,
            WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
            WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
            WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        return fail(GetLastError());
    async.value = std::exchange(request.value, nullptr);
    const auto finish = [&](BOOL accepted) -> DWORD {
        if (!accepted) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) return error;
        }
        return async.wait(stop);
    };
    if (const auto error = stop()) return fail(error);
    async.prepare(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    if (const auto error = finish(WinHttpSendRequest(async.value, nullptr, 0, nullptr, 0, 0, context)))
        return fail(error);
    async.prepare(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
    if (const auto error = stop()) return fail(error);
    if (const auto error = finish(WinHttpReceiveResponse(async.value, nullptr))) return fail(error);
    DWORD size = sizeof(result.http_status);
    if (!WinHttpQueryHeaders(async.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &result.http_status, &size, nullptr)) return fail(GetLastError());
    if (result.http_status != 200) return fail(ERROR_BAD_NET_RESP);
    DWORD length{}; size = sizeof(length);
    if (WinHttpQueryHeaders(async.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                           nullptr, &length, &size, nullptr) && length > max_bytes) return fail(ERROR_FILE_TOO_LARGE);
    if (watch) watch->total.store(length), watch->received.store(0);
    if (const auto error = stop()) return fail(error);
    Handle output{CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr),
                  destination};
    if (output.value == INVALID_HANDLE_VALUE) return fail(GetLastError());
    std::uint64_t total{};
    for (;;) {
        if (const auto error = stop()) return fail(error);
        async.prepare(WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
        if (const auto error = finish(WinHttpReadData(async.value, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr)))
            return fail(error);
        const auto read = async.read;
        DWORD written{};
        if (!read) break;
        if (total + read > max_bytes) return fail(ERROR_FILE_TOO_LARGE);
        if (!WriteFile(output.value, buffer.data(), read, &written, nullptr) || written != read) return fail(ERROR_WRITE_FAULT);
        total += read;
        if (watch) watch->received.store(total);
    }
    if (!total) return fail(ERROR_BAD_LENGTH);
    if (!FlushFileBuffers(output.value)) return fail(GetLastError());
    async.close();
    if (const auto error = stop()) return fail(error);
    output.complete = true;
    result.ok = true;
    return result;
}

std::optional<std::string> get_text(std::wstring_view url, std::uint64_t max_bytes,
    std::uint32_t timeout_seconds, const wchar_t* agent, Download* result, Watch* watch) {
    static std::atomic<unsigned> serial{};
    wchar_t folder[MAX_PATH + 1]{};
    const auto length = GetTempPathW(MAX_PATH, folder);
    const auto path = std::filesystem::path(length ? std::wstring(folder, length) : L".") /
        (L"reskate-get-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial) + L".tmp");
    std::error_code error;
    std::filesystem::remove(path, error); // discard a stale file left by an earlier process
    const auto download = get(url, path, max_bytes, timeout_seconds, agent, watch);
    if (result) *result = download;
    std::optional<std::string> text;
    if (download.ok) {
        std::ifstream in(path, std::ios::binary);
        text.emplace(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (download.ok) std::filesystem::remove(path, error);
    return text;
}
}
