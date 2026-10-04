#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr size_t kMaxCapturedBytes = 1024 * 1024; // independently per stream
constexpr DWORD kDefaultExecTimeoutMs = 120000;
constexpr DWORD kDefaultTransferTimeoutMs = 600000;
constexpr DWORD kMaximumTimeoutMs = 3600000;

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value = other.value; other.value = nullptr; }
        return *this;
    }
    void reset(HANDLE h = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = h; }
    HANDLE get() const { return value; }
    HANDLE release() { HANDLE h = value; value = nullptr; return h; }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr)) return {};
    return out;
}
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n)) return {};
    return out;
}

// Replace malformed byte sequences from a child process so every JSON response is valid UTF-8.
std::string SanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto replacement = [&out]() { out.append("\xEF\xBF\xBD", 3); };
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 0x7f) { out.push_back(static_cast<char>(c)); ++i; continue; }
        size_t n = 0;
        uint32_t cp = 0;
        if (c >= 0xc2 && c <= 0xdf) { n = 2; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { n = 3; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 4; cp = c & 0x07; }
        else { replacement(); ++i; continue; }
        if (i + n > s.size()) { replacement(); ++i; continue; }
        bool valid = true;
        for (size_t j = 1; j < n; ++j) {
            const unsigned char t = static_cast<unsigned char>(s[i + j]);
            if ((t & 0xc0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (t & 0x3f);
        }
        if (!valid || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            replacement(); ++i; continue;
        }
        out.append(s, i, n); i += n;
    }
    return out;
}
std::string JsonEscape(const std::string& input) {
    const std::string s = SanitizeUtf8(input);
    std::string out; out.reserve(s.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
            else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// Implements the Microsoft C runtime / CreateProcess command-line quoting convention.
std::wstring QuoteWindowsArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); slashes = 0; continue;
        }
        out.append(slashes, L'\\'); slashes = 0; out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
std::wstring BuildCommandLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring line = QuoteWindowsArg(exe);
    for (const auto& arg : args) { line.push_back(L' '); line += QuoteWindowsArg(arg); }
    return line;
}

struct CapturedStream { std::string bytes; uint64_t totalBytes = 0; bool truncated = false; };
struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    bool localTerminationConfirmed = false;
    bool terminatedLocally = false; // exitCode, if any, came from our own TerminateJobObject/TerminateProcess
    DWORD winError = ERROR_SUCCESS;
    int exitCode = -1;
    uint64_t durationMs = 0;
    CapturedStream out, err;
    std::string stdinError, captureError, cleanupError;
};

struct PipePair { Handle server, child; DWORD error = ERROR_SUCCESS; };
std::atomic<unsigned long> g_pipeSequence{ 0 };
ULONGLONG RemainingMs(ULONGLONG deadline) {
    const ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : deadline - now;
}
struct ConnectIo { Handle pipe, event; OVERLAPPED ov{}; };
void CancelAndRetainConnectIfPending(PipePair& pair, std::unique_ptr<ConnectIo>& request) {
    if (!CancelIoEx(request->pipe.get(), &request->ov) && GetLastError() != ERROR_NOT_FOUND) { /* completion is checked below */ }
    const DWORD waited = WaitForSingleObject(request->event.get(), 500);
    DWORD ignored = 0;
    const bool completed = waited == WAIT_OBJECT_0 &&
        (GetOverlappedResult(request->pipe.get(), &request->ov, &ignored, FALSE) || GetLastError() != ERROR_IO_INCOMPLETE);
    if (!completed) {
        // Keep the pipe, event, OVERLAPPED, and kernel-referenced storage alive until process exit.
        (void)pair;
        request.release();
    }
}
PipePair MakePipePair(bool serverReads, const wchar_t* suffix, ULONGLONG deadline) {
    PipePair pair;
    const unsigned long seq = ++g_pipeSequence;
    const std::wstring name = L"\\\\.\\pipe\\MacMiniCli_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(seq) + L"_" + suffix;
    const DWORD access = (serverReads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED;
    auto request = std::make_unique<ConnectIo>();
    request->pipe.reset(CreateNamedPipeW(name.c_str(), access, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, nullptr));
    if (!request->pipe) { pair.error = GetLastError(); return pair; }
    request->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!request->event) { pair.error = GetLastError(); return pair; }
    request->ov.hEvent = request->event.get();
    BOOL connected = ConnectNamedPipe(request->pipe.get(), &request->ov);
    bool pending = false;
    if (!connected) {
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) pending = true;
        else if (e == ERROR_PIPE_CONNECTED) SetEvent(request->event.get());
        else { pair.error = e; return pair; }
    }
    SECURITY_ATTRIBUTES childSa{ sizeof(childSa), nullptr, TRUE };
    pair.child.reset(CreateFileW(name.c_str(), serverReads ? GENERIC_WRITE : GENERIC_READ, 0, &childSa,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!pair.child) {
        pair.error = GetLastError();
        if (pending) CancelAndRetainConnectIfPending(pair, request);
        return pair;
    }
    if (pending) {
        const ULONGLONG remain = RemainingMs(deadline);
        const DWORD wait = remain ? WaitForSingleObject(request->event.get(), static_cast<DWORD>(std::min<ULONGLONG>(remain, MAXDWORD - 1))) : WAIT_TIMEOUT;
        if (wait != WAIT_OBJECT_0) {
            pair.error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelAndRetainConnectIfPending(pair, request);
            return pair;
        }
        DWORD ignored = 0;
        if (!GetOverlappedResult(request->pipe.get(), &request->ov, &ignored, FALSE)) {
            const DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) {
                pair.error = ERROR_IO_INCOMPLETE;
                CancelAndRetainConnectIfPending(pair, request);
                return pair;
            }
            if (e != ERROR_PIPE_CONNECTED) { pair.error = e; return pair; }
        }
    }
    pair.server = std::move(request->pipe);
    return pair;
}
void AppendCapture(CapturedStream& capture, const char* data, size_t n, size_t limit) {
    capture.totalBytes += static_cast<uint64_t>(n);
    const size_t available = capture.bytes.size() < limit ? limit - capture.bytes.size() : 0;
    const size_t keep = std::min(available, n);
    if (keep) capture.bytes.append(data, keep);
    if (keep < n) capture.truncated = true;
}
struct AsyncReader {
    Handle pipe, event;
    OVERLAPPED ov{};
    std::array<char, 8192> buffer{};
    CapturedStream capture;
    bool pending = false, ready = false, eof = false, cancelled = false;
    DWORD error = ERROR_SUCCESS;
};
struct AsyncWriter {
    Handle pipe, event;
    OVERLAPPED ov{};
    std::string bytes;
};
bool IssueRead(AsyncReader& r, size_t limit) {
    if (r.eof || r.error || r.pending || r.ready) return true;
    ResetEvent(r.event.get()); r.ov = {}; r.ov.hEvent = r.event.get();
    DWORD got = 0;
    if (ReadFile(r.pipe.get(), r.buffer.data(), static_cast<DWORD>(r.buffer.size()), &got, &r.ov)) {
        if (!got) r.eof = true;
        else {
            AppendCapture(r.capture, r.buffer.data(), got, limit);
            r.ready = true;
            if (!SetEvent(r.event.get())) { r.error = GetLastError(); return false; }
        }
        return true; // synchronous completions are queued as ready work for the fair event loop
    }
    const DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) { r.pending = true; return true; }
    if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) { r.eof = true; return true; }
    if (e == ERROR_OPERATION_ABORTED) { r.cancelled = true; return true; }
    r.error = e; return false;
}
bool CompleteRead(AsyncReader& r, size_t limit) {
    DWORD got = 0;
    if (!GetOverlappedResult(r.pipe.get(), &r.ov, &got, FALSE)) {
        const DWORD e = GetLastError();
        if (e == ERROR_IO_INCOMPLETE) return true; // still pending: preserve OVERLAPPED and buffer
        r.pending = false;
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) r.eof = true;
        else if (e == ERROR_OPERATION_ABORTED) r.cancelled = true;
        else r.error = e;
        return r.error == ERROR_SUCCESS;
    }
    r.pending = false;
    if (!got) r.eof = true;
    else AppendCapture(r.capture, r.buffer.data(), got, limit);
    return true;
}

ProcessResult RunProcess(const std::wstring& executable, const std::vector<std::wstring>& args,
                         const std::string& stdinBytes, DWORD timeoutMs, size_t outputLimit = kMaxCapturedBytes) {
    ProcessResult result;
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG deadline = start + timeoutMs;
    auto outPair = MakePipePair(true, L"stdout", deadline);
    auto errPair = MakePipePair(true, L"stderr", deadline);
    PipePair inPair;
    if (!stdinBytes.empty()) inPair = MakePipePair(false, L"stdin", deadline);
    if (!outPair.server || !errPair.server || (!stdinBytes.empty() && (!inPair.server || !inPair.child))) {
        result.winError = outPair.error ? outPair.error : (errPair.error ? errPair.error : inPair.error);
        result.timedOut = result.winError == ERROR_TIMEOUT;
        result.durationMs = GetTickCount64() - start; return result;
    }
    Handle nullIn;
    SECURITY_ATTRIBUTES inheritSa{ sizeof(inheritSa), nullptr, TRUE };
    if (stdinBytes.empty()) {
        nullIn.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritSa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!nullIn) { result.winError = GetLastError(); result.durationMs = GetTickCount64() - start; return result; }
    }
    std::array<HANDLE, 3> inherited{ stdinBytes.empty() ? nullIn.get() : inPair.child.get(), outPair.child.get(), errPair.child.get() };
    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    std::vector<unsigned char> attrStorage(attrBytes);
    auto* attrs = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &attrBytes) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr)) {
        result.winError = GetLastError(); if (attrs) DeleteProcThreadAttributeList(attrs);
        result.durationMs = GetTickCount64() - start; return result;
    }
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) { result.winError = GetLastError(); DeleteProcThreadAttributeList(attrs); result.durationMs = GetTickCount64() - start; return result; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits))) {
        result.winError = GetLastError(); DeleteProcThreadAttributeList(attrs); result.durationMs = GetTickCount64() - start; return result;
    }
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inherited[0]; si.StartupInfo.hStdOutput = inherited[1]; si.StartupInfo.hStdError = inherited[2]; si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring commandLine = BuildCommandLine(executable, args);
    const BOOL created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!created) { result.winError = createError; result.durationMs = GetTickCount64() - start; return result; }
    Handle process(pi.hProcess), primaryThread(pi.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        const DWORD assignError = GetLastError();
        if (!TerminateProcess(process.get(), 125)) result.cleanupError = "Could not terminate suspended process after job assignment failed: " + std::to_string(GetLastError());
        const DWORD waited = WaitForSingleObject(process.get(), 1000);
        if (waited != WAIT_OBJECT_0) result.cleanupError += " Suspended process cleanup did not complete within 1 second.";
        result.winError = assignError; result.durationMs = GetTickCount64() - start; return result;
    }
    if (ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1)) {
        result.winError = GetLastError();
        if (!TerminateJobObject(job.get(), 125)) result.cleanupError = "TerminateJobObject failed after ResumeThread failure: " + std::to_string(GetLastError());
        WaitForSingleObject(process.get(), 1000); result.durationMs = GetTickCount64() - start; return result;
    }
    result.started = true;
    primaryThread.reset();
    outPair.child.reset(); errPair.child.reset();
    if (!stdinBytes.empty()) inPair.child.reset(); else nullIn.reset();
    auto outOwner = std::make_unique<AsyncReader>();
    auto errOwner = std::make_unique<AsyncReader>();
    AsyncReader& out = *outOwner; AsyncReader& err = *errOwner;
    out.pipe = std::move(outPair.server); err.pipe = std::move(errPair.server);
    out.event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr)); err.event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!out.event || !err.event) {
        result.winError = GetLastError();
        if (!TerminateJobObject(job.get(), 125)) result.cleanupError = "TerminateJobObject failed during capture setup: " + std::to_string(GetLastError());
        const DWORD stopped = WaitForSingleObject(process.get(), 1000);
        if (stopped != WAIT_OBJECT_0) result.cleanupError += "; process termination was not confirmed within 1 second";
        result.durationMs = GetTickCount64() - start; return result;
    }
    bool processDone = false, stdinDone = stdinBytes.empty();
    size_t inputOffset = 0;
    auto writerOwner = stdinBytes.empty() ? std::unique_ptr<AsyncWriter>() : std::make_unique<AsyncWriter>();
    if (writerOwner) { writerOwner->pipe = std::move(inPair.server); writerOwner->bytes = stdinBytes; writerOwner->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr)); }
    OVERLAPPED unusedInput{};
    OVERLAPPED& inputOv = writerOwner ? writerOwner->ov : unusedInput;
    const HANDLE inputEvent = writerOwner ? writerOwner->event.get() : nullptr;
    const HANDLE inputPipe = writerOwner ? writerOwner->pipe.get() : nullptr;
    bool inputPending = false, inputReady = false;
    DWORD inputError = ERROR_SUCCESS;
    if (writerOwner && !writerOwner->event) result.winError = GetLastError();
    auto issueWrite = [&]() {
        if (!writerOwner || stdinDone || inputPending || inputReady || inputError) return;
        if (inputOffset >= writerOwner->bytes.size()) { stdinDone = true; writerOwner->pipe.reset(); return; }
        ResetEvent(inputEvent); inputOv = {}; inputOv.hEvent = inputEvent;
        const DWORD request = static_cast<DWORD>(std::min<size_t>(writerOwner->bytes.size() - inputOffset, 16384));
        DWORD wrote = 0;
        if (WriteFile(inputPipe, writerOwner->bytes.data() + inputOffset, request, &wrote, &inputOv)) {
            if (!wrote) inputError = ERROR_WRITE_FAULT;
            else {
                inputOffset += wrote;
                if (inputOffset < writerOwner->bytes.size()) {
                    inputReady = true;
                    if (!SetEvent(inputEvent)) inputError = GetLastError();
                }
            }
        } else {
            const DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) inputPending = true; else inputError = e;
        }
        if (inputOffset == writerOwner->bytes.size() && !inputPending && !inputError) { stdinDone = true; writerOwner->pipe.reset(); }
        if (inputError) { result.stdinError = "SFTP batch stdin write failed after " + std::to_string(inputOffset) + " of " + std::to_string(writerOwner->bytes.size()) + " bytes: Windows error " + std::to_string(inputError); writerOwner->pipe.reset(); }
    };
    ULONGLONG cleanupDeadline = deadline;
    bool timeoutCleanupStarted = false;
    unsigned ioTurn = 1; // rotating wait priority prevents a continuously-ready stream from starving the other
    for (;;) {
        if (result.winError) break;
        ULONGLONG activeDeadline = timeoutCleanupStarted ? cleanupDeadline : deadline;
        if (!RemainingMs(activeDeadline)) {
            if (!timeoutCleanupStarted) {
                result.timedOut = true; timeoutCleanupStarted = true; cleanupDeadline = GetTickCount64() + 500;
                if (!processDone) result.terminatedLocally = true;
                if (!TerminateJobObject(job.get(), 124)) {
                    const DWORD e = GetLastError(); result.cleanupError = "TerminateJobObject failed at timeout: Windows error " + std::to_string(e);
                    if (!TerminateProcess(process.get(), 124)) result.cleanupError += "; TerminateProcess fallback failed: Windows error " + std::to_string(GetLastError());
                }
                if (inputOffset < stdinBytes.size() && result.stdinError.empty())
                    result.stdinError = "Deadline expired after " + std::to_string(inputOffset) + " of " + std::to_string(stdinBytes.size()) + " SFTP batch bytes; pending stdin delivery was canceled.";
                if (inputPending && writerOwner && !CancelIoEx(writerOwner->pipe.get(), &inputOv) && GetLastError() != ERROR_NOT_FOUND)
                    result.cleanupError += "; CancelIoEx(stdin) failed: Windows error " + std::to_string(GetLastError());
            } else {
                if (out.pending) { if (!CancelIoEx(out.pipe.get(), &out.ov) && GetLastError() != ERROR_NOT_FOUND) result.cleanupError += "; CancelIoEx(stdout) failed: Windows error " + std::to_string(GetLastError()); out.cancelled = true; }
                if (err.pending) { if (!CancelIoEx(err.pipe.get(), &err.ov) && GetLastError() != ERROR_NOT_FOUND) result.cleanupError += "; CancelIoEx(stderr) failed: Windows error " + std::to_string(GetLastError()); err.cancelled = true; }
                if (inputPending && writerOwner && !CancelIoEx(writerOwner->pipe.get(), &inputOv) && GetLastError() != ERROR_NOT_FOUND) result.cleanupError += "; CancelIoEx(stdin) failed: Windows error " + std::to_string(GetLastError());
                if (!processDone) {
                    result.terminatedLocally = true;
                    if (!TerminateProcess(process.get(), 124)) result.cleanupError += "; TerminateProcess cleanup failed: Windows error " + std::to_string(GetLastError());
                    const DWORD cleanupWait = WaitForSingleObject(process.get(), 0);
                    if (cleanupWait != WAIT_OBJECT_0) result.cleanupError += "; primary process was not confirmed stopped at the deadline";
                }
                result.captureError = "Output capture did not reach normal pipe EOF before the bounded cleanup grace ended; pending capture I/O was canceled.";
                break;
            }
        }
        if (!timeoutCleanupStarted || RemainingMs(cleanupDeadline)) {
            if (!out.pending && !out.eof && !IssueRead(out, outputLimit)) { result.captureError = "stdout capture failed: Windows error " + std::to_string(out.error); result.winError = out.error; }
            if (!err.pending && !err.eof && !IssueRead(err, outputLimit)) { result.captureError = "stderr capture failed: Windows error " + std::to_string(err.error); result.winError = err.error; }
            if (!result.timedOut && !stdinDone && !inputPending && !inputError) issueWrite();
        }
        if (result.winError) break;
        if (processDone && out.eof && err.eof && (stdinDone || (inputError && !inputPending) || (result.timedOut && !inputPending))) break;
        std::array<HANDLE, 4> waits{}; std::array<int, 4> kinds{}; DWORD count = 0;
        if (!processDone) { waits[count] = process.get(); kinds[count++] = 0; }
        for (unsigned offset = 0; offset < 3; ++offset) {
            const unsigned kind = 1 + ((ioTurn - 1 + offset) % 3);
            if ((kind == 1 && (out.pending || out.ready)) || (kind == 2 && (err.pending || err.ready)) || (kind == 3 && (inputPending || inputReady))) {
                waits[count] = kind == 1 ? out.event.get() : (kind == 2 ? err.event.get() : inputEvent);
                kinds[count++] = static_cast<int>(kind);
            }
        }
        if (!count) {
            if (!processDone) { WaitForSingleObject(process.get(), 1); continue; }
            Sleep(1); continue;
        }
        const ULONGLONG remain = RemainingMs(activeDeadline);
        if (!remain) continue;
        const DWORD wait = WaitForMultipleObjects(count, waits.data(), FALSE, static_cast<DWORD>(std::min<ULONGLONG>(remain, MAXDWORD - 1)));
        if (wait == WAIT_TIMEOUT) continue;
        if (wait == WAIT_FAILED || wait >= WAIT_OBJECT_0 + count) {
            result.winError = wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA; break;
        }
        const DWORD index = wait - WAIT_OBJECT_0;
        switch (kinds[index]) {
        case 0: {
            DWORD code = STILL_ACTIVE;
            if (!GetExitCodeProcess(process.get(), &code)) { result.winError = GetLastError(); break; }
            result.exitCode = static_cast<int>(code); processDone = true;
            // OpenSSH should not leave local helpers behind. Kill any job member that
            // retained inherited pipes so capture can finish instead of hanging forever.
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
                result.cleanupError = "Could not query job cleanup state: Windows error " + std::to_string(GetLastError());
            } else if (accounting.ActiveProcesses > 0 && !TerminateJobObject(job.get(), 125)) {
                result.cleanupError = "TerminateJobObject failed after the OpenSSH parent exited: Windows error " + std::to_string(GetLastError());
            }
            if (inputPending && writerOwner) {
                DWORD wrote = 0;
                if (GetOverlappedResult(writerOwner->pipe.get(), &inputOv, &wrote, FALSE)) {
                    inputPending = false; inputOffset += wrote;
                    if (inputOffset == writerOwner->bytes.size()) stdinDone = true;
                    else inputError = ERROR_BROKEN_PIPE;
                } else if (GetLastError() == ERROR_IO_INCOMPLETE) {
                    if (!CancelIoEx(writerOwner->pipe.get(), &inputOv) && GetLastError() != ERROR_NOT_FOUND)
                        result.cleanupError += "; CancelIoEx(stdin after child exit) failed: Windows error " + std::to_string(GetLastError());
                    inputError = ERROR_BROKEN_PIPE; // keep pending until completion is observed
                } else { inputError = GetLastError(); inputPending = false; }
            } else if (!stdinDone && !inputError) inputError = ERROR_BROKEN_PIPE;
            if (inputError && result.stdinError.empty()) {
                result.stdinError = "Child exited before the complete SFTP batch was accepted (" + std::to_string(inputOffset) + "/" + std::to_string(writerOwner ? writerOwner->bytes.size() : 0) + " bytes written).";
                if (!inputPending && writerOwner) writerOwner->pipe.reset();
            }
            break;
        }
        case 1:
            if (out.pending) { if (!CompleteRead(out, outputLimit) && out.error) result.captureError = "stdout capture failed: Windows error " + std::to_string(out.error); }
            else out.ready = false;
            break;
        case 2:
            if (err.pending) { if (!CompleteRead(err, outputLimit) && err.error) result.captureError = "stderr capture failed: Windows error " + std::to_string(err.error); }
            else err.ready = false;
            break;
        case 3: {
            if (inputPending) {
                DWORD wrote = 0;
                if (writerOwner && !GetOverlappedResult(writerOwner->pipe.get(), &inputOv, &wrote, FALSE)) {
                    inputError = GetLastError();
                    if (inputError != ERROR_IO_INCOMPLETE) inputPending = false;
                } else if (writerOwner) { inputOffset += wrote; inputPending = false; inputError = ERROR_SUCCESS; }
            } else inputReady = false;
            if (inputError && inputError != ERROR_IO_INCOMPLETE) result.stdinError = "SFTP batch stdin write failed after " + std::to_string(inputOffset) + " of " + std::to_string(writerOwner ? writerOwner->bytes.size() : 0) + " bytes: Windows error " + std::to_string(inputError);
            ioTurn = 1;
            break;
        }
        }
        if (kinds[index] >= 1) ioTurn = kinds[index] == 3 ? 1u : static_cast<unsigned>(kinds[index] + 1);
        if (inputError && inputError != ERROR_IO_INCOMPLETE && result.stdinError.empty()) result.stdinError = "SFTP batch stdin write failed after " + std::to_string(inputOffset) + " of " + std::to_string(writerOwner ? writerOwner->bytes.size() : 0) + " bytes: Windows error " + std::to_string(inputError);
        if (out.error && result.captureError.empty()) result.captureError = "stdout capture failed: Windows error " + std::to_string(out.error);
        if (err.error && result.captureError.empty()) result.captureError = "stderr capture failed: Windows error " + std::to_string(err.error);
    }
    if (!processDone) {
        result.terminatedLocally = true;
        if (!TerminateJobObject(job.get(), result.timedOut ? 124 : 125))
            result.cleanupError += "TerminateJobObject during final cleanup failed: Windows error " + std::to_string(GetLastError());
        const DWORD stopped = WaitForSingleObject(process.get(), 500);
        if (stopped == WAIT_OBJECT_0) {
            processDone = true;
            DWORD code = STILL_ACTIVE;
            if (GetExitCodeProcess(process.get(), &code) && result.exitCode < 0) result.exitCode = static_cast<int>(code);
        } else result.cleanupError += "; primary process termination was not confirmed within 500 ms";
    }
    auto reapPending = [&result](HANDLE pipe, HANDLE event, OVERLAPPED& ov, bool& pending, const char* label) -> bool {
        if (!pending) return true;
        if (pipe && !CancelIoEx(pipe, &ov) && GetLastError() != ERROR_NOT_FOUND)
            result.cleanupError += std::string("; CancelIoEx(") + label + ") failed: Windows error " + std::to_string(GetLastError());
        const DWORD waited = event ? WaitForSingleObject(event, 500) : WAIT_FAILED;
        if (waited != WAIT_OBJECT_0) {
            result.cleanupError += std::string("; canceled ") + label + " I/O was not confirmed complete; its state is retained until process exit";
            return false;
        }
        DWORD ignored = 0;
        if (pipe && !GetOverlappedResult(pipe, &ov, &ignored, FALSE)) {
            const DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) {
                result.cleanupError += std::string("; canceled ") + label + " I/O remains pending; its state is retained until process exit";
                return false;
            }
            if (e != ERROR_OPERATION_ABORTED && e != ERROR_BROKEN_PIPE && e != ERROR_HANDLE_EOF)
                result.cleanupError += std::string("; canceled ") + label + " I/O completed with Windows error " + std::to_string(e);
        }
        pending = false; // event + GetOverlappedResult established completion
        return true;
    };
    const bool outReaped = reapPending(out.pipe.get(), out.event.get(), out.ov, out.pending, "stdout");
    const bool errReaped = reapPending(err.pipe.get(), err.event.get(), err.ov, err.pending, "stderr");
    const bool inputReaped = !inputPending || (writerOwner && reapPending(writerOwner->pipe.get(), writerOwner->event.get(), inputOv, inputPending, "stdin"));
    if (!outReaped) (void)outOwner.release();
    if (!errReaped) (void)errOwner.release();
    if (!inputReaped && writerOwner) (void)writerOwner.release();
    result.out = std::move(out.capture); result.err = std::move(err.capture);
    if (out.cancelled || err.cancelled) {
        if (result.captureError.empty()) result.captureError = "Capture I/O was canceled before normal EOF.";
    }
    if (result.timedOut) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION finalAccounting{};
        if (processDone && QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &finalAccounting, sizeof(finalAccounting), nullptr) && finalAccounting.ActiveProcesses == 0)
            result.localTerminationConfirmed = true;
        else if (result.cleanupError.empty()) result.cleanupError = "Local process-tree termination could not be confirmed before cleanup ended.";
    }
    if (!result.cleanupError.empty() && !result.winError) result.winError = ERROR_PROCESS_ABORTED;
    result.durationMs = GetTickCount64() - start;
    return result;
}

// The OpenSSH client's own exit status, or nullopt when there is none to report. A status produced by
// this process terminating the job (124/125 on timeout or cleanup) is not the client's or the remote
// command's status and must not be reported as exit_code.
std::optional<int> ReportedExitCode(const ProcessResult& p) {
    if (!p.started || p.exitCode == -1 || p.terminatedLocally) return std::nullopt;
    return p.exitCode;
}

struct OpenSshExecutables { std::wstring ssh; std::wstring sftp; };
bool ResolveOpenSshExecutables(const std::wstring& systemDirectory, bool requireSftp, OpenSshExecutables& selected, std::string& error) {
    const fs::path root(systemDirectory);
    if (!root.is_absolute()) { error = "Windows system directory is not absolute."; return false; }
    const auto requireFile = [&](const wchar_t* name, std::wstring& target) {
        const fs::path path = root / L"OpenSSH" / name;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec) {
            error = "Required Windows OpenSSH executable is missing: " + WideToUtf8(path.wstring());
            return false;
        }
        target = path.wstring();
        return true;
    };
    if (!requireFile(L"ssh.exe", selected.ssh)) return false;
    if (requireSftp && !requireFile(L"sftp.exe", selected.sftp)) return false;
    return true;
}
bool FindSystemOpenSshExecutables(bool requireSftp, OpenSshExecutables& selected, std::string& error) {
    std::array<wchar_t, 32768> systemDir{};
    const UINT n = GetSystemDirectoryW(systemDir.data(), static_cast<UINT>(systemDir.size()));
    if (!n || n >= systemDir.size()) { error = "Unable to obtain the Windows system directory with GetSystemDirectoryW."; return false; }
    return ResolveOpenSshExecutables(std::wstring(systemDir.data(), n), requireSftp, selected, error);
}
std::string WinErrorText(DWORD code) {
    wchar_t* message = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string text = n && message ? WideToUtf8(std::wstring(message, n)) : "Windows error " + std::to_string(code);
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}
std::string LowerAscii(std::string s) {
    for (char& c : s) if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string TroubleshootingHint(const std::string& stderrText, bool sftp) {
    const std::string t = LowerAscii(stderrText);
    if (sftp && t.find("permission denied") != std::string::npos)
        return "SFTP stderr mentions file permission denial. Check source/destination access; the transfer may be partial. This text does not establish an SSH authentication failure.";
    const std::string caution = " This is a troubleshooting hint only: SSH stderr can also contain remote-command output and does not prove whether the remote command was dispatched or completed.";
    if (t.find("host key verification failed") != std::string::npos || t.find("remote host identification has changed") != std::string::npos)
        return "SSH stderr contains text commonly associated with host-key verification. Verify the host fingerprint out of band before changing known_hosts." + caution;
    if (t.find("permission denied") != std::string::npos || t.find("no supported authentication methods") != std::string::npos || t.find("too many authentication failures") != std::string::npos)
        return "SSH stderr contains text commonly associated with authentication setup. Check the username, selected key/agent, and authorized keys." + caution;
    if (t.find("connection refused") != std::string::npos) return "SSH stderr mentions connection refusal; verify the host, SSH service, and port." + caution;
    if (t.find("connection timed out") != std::string::npos || t.find("operation timed out") != std::string::npos) return "SSH stderr mentions a connection timeout; verify reachability and timeout settings." + caution;
    if (t.find("could not resolve hostname") != std::string::npos || t.find("name or service not known") != std::string::npos) return "SSH stderr mentions name resolution failure; verify the configured host name." + caution;
    if (t.find("no route to host") != std::string::npos || t.find("network is unreachable") != std::string::npos) return "SSH stderr mentions a network route failure; verify current network reachability." + caution;
    return {};
}

struct Connection { std::wstring host, user, key; unsigned short port = 22; DWORD timeoutMs = 0; };
struct CliOptions {
    std::wstring operation;
    Connection connection;
    std::wstring command, localPath, remotePath;
    bool help = false, selfTest = false;
    std::wstring parseError;
};
std::wstring LowerWide(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
bool IsSimpleToken(const std::wstring& s, bool host) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || (!host && c == L'_')) continue;
        return false;
    }
    return true;
}
bool ParseUnsigned(const std::wstring& s, uint64_t* value) {
    if (s.empty()) return false;
    uint64_t n = 0;
    for (wchar_t c : s) { if (c < L'0' || c > L'9') return false; n = n * 10 + static_cast<unsigned>(c - L'0'); if (n > std::numeric_limits<uint32_t>::max()) return false; }
    *value = n; return true;
}
CliOptions ParseArgs(int argc, wchar_t** argv) {
    CliOptions o;
    if (argc == 2 && std::wstring(argv[1]) == L"--self-test") { o.selfTest = true; return o; }
    if (argc == 2 && (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h")) { o.help = true; return o; }
    if (argc < 2) { o.parseError = L"Missing operation. Choose exec, upload, or download."; return o; }
    o.operation = LowerWide(argv[1]);
    if (o.operation != L"exec" && o.operation != L"upload" && o.operation != L"download") { o.parseError = L"Unknown operation. Choose exec, upload, or download."; return o; }
    bool commandMode = false;
    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (o.operation == L"exec" && !commandMode && arg == L"--") {
            if (i + 1 >= argc) { o.parseError = L"exec requires a remote command after --."; return o; }
            commandMode = true;
            std::wstring combined;
            for (++i; i < argc; ++i) { if (!combined.empty()) combined.push_back(L' '); combined += argv[i]; }
            o.command = std::move(combined);
            break;
        }
        if (commandMode) { o.parseError = L"Unexpected argument after remote command."; return o; }
        if (arg.size() < 3 || arg.substr(0, 2) != L"--") { o.parseError = L"Expected a named option (for exec, put the command after --)."; return o; }
        if (i + 1 >= argc) { o.parseError = L"Missing value for option " + arg + L"."; return o; }
        const std::wstring value = argv[++i];
        if (arg == L"--host") o.connection.host = value;
        else if (arg == L"--user") o.connection.user = value;
        else if (arg == L"--key") o.connection.key = value;
        else if (arg == L"--local") o.localPath = value;
        else if (arg == L"--remote") o.remotePath = value;
        else if (arg == L"--port") {
            uint64_t n = 0; if (!ParseUnsigned(value, &n) || n < 1 || n > 65535) { o.parseError = L"--port must be an integer from 1 through 65535."; return o; }
            o.connection.port = static_cast<unsigned short>(n);
        } else if (arg == L"--timeout-ms") {
            uint64_t n = 0; if (!ParseUnsigned(value, &n) || n < 1 || n > kMaximumTimeoutMs) { o.parseError = L"--timeout-ms must be from 1 through 3600000."; return o; }
            o.connection.timeoutMs = static_cast<DWORD>(n);
        } else { o.parseError = L"Unknown option " + arg + L"."; return o; }
    }
    if (o.operation == L"exec" && o.command.empty()) { o.parseError = L"exec requires a non-empty remote command after --."; return o; }
    if (o.connection.host.empty() || !IsSimpleToken(o.connection.host, true)) { o.parseError = L"--host is required and may contain only letters, digits, dot, and hyphen (DNS name or IPv4 address)."; return o; }
    if (o.connection.user.empty() || !IsSimpleToken(o.connection.user, false)) { o.parseError = L"--user is required; use letters, digits, dot, underscore, or hyphen."; return o; }
    if (!o.connection.key.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(fs::path(o.connection.key), ec) || ec) { o.parseError = L"The configured --key file does not exist or is not a regular file."; return o; }
    }
    if (o.operation != L"exec") {
        if (o.localPath.empty() || o.remotePath.empty()) { o.parseError = L"Both --local and --remote are required for file transfer."; return o; }
        std::error_code ec;
        if (o.operation == L"upload" && !fs::is_regular_file(fs::path(o.localPath), ec)) { o.parseError = L"The upload --local path must name an existing regular file."; return o; }
        if (o.operation == L"download") {
            const fs::path dest(o.localPath);
            if (!dest.parent_path().empty() && !fs::is_directory(dest.parent_path(), ec)) { o.parseError = L"The download destination directory does not exist."; return o; }
            if (fs::is_directory(dest, ec)) { o.parseError = L"The download --local destination must be a file path, not a directory."; return o; }
        }
    }
    if (!o.connection.timeoutMs) o.connection.timeoutMs = o.operation == L"exec" ? kDefaultExecTimeoutMs : kDefaultTransferTimeoutMs;
    return o;
}

std::optional<std::wstring> SftpQuotedPath(std::wstring path, bool remote, std::wstring* error) {
    if (path.empty()) { *error = L"SFTP paths cannot be empty."; return std::nullopt; }
    for (wchar_t c : path) {
        if (c == L'\r' || c == L'\n' || c == L'\0' || c < 0x20) { *error = L"SFTP paths cannot contain line breaks or control characters."; return std::nullopt; }
        if (remote && (c == L'*' || c == L'?' || c == L'[' || c == L']')) { *error = L"Remote SFTP paths containing glob characters (* ? [ ]) are rejected to avoid ambiguous wildcard expansion."; return std::nullopt; }
    }
    if (remote && path[0] == L'-') path = L"./" + path; // avoid SFTP command-option interpretation
    std::wstring out = L"\"";
    for (wchar_t c : path) { if (c == L'\\' || c == L'"') out.push_back(L'\\'); out.push_back(c); }
    out.push_back(L'"');
    return out;
}
std::wstring ToSftpLocalPath(std::wstring path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(path), ec);
    if (!ec) path = absolute.wstring();
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return path;
}
std::optional<std::wstring> BuildSftpBatch(const CliOptions& o, std::wstring* error) {
    std::wstring remoteError, localError;
    auto remote = SftpQuotedPath(o.remotePath, true, &remoteError);
    if (!remote) { *error = remoteError; return std::nullopt; }
    const bool upload = o.operation == L"upload";
    if (upload && o.localPath.find_first_of(L"[]") != std::wstring::npos) {
        // sftp's put expands the local path with glob(3); "[1]" would match a sibling such as "photo 1.jpg"
        // and upload that file instead. Windows filenames cannot contain * or ?, so only brackets matter here.
        *error = L"Upload --local paths containing [ or ] are rejected: sftp treats them as glob characters and could upload a different file. Copy or rename the file to a name without brackets first.";
        return std::nullopt;
    }
    auto local = SftpQuotedPath(upload ? ToSftpLocalPath(o.localPath) : o.localPath, false, &localError);
    if (!local) { *error = localError; return std::nullopt; }
    return upload ? L"put " + *local + L" " + *remote + L"\n" : L"get " + *remote + L" " + *local + L"\n";
}
std::vector<std::wstring> CommonSshOptions(const Connection& c) {
    std::vector<std::wstring> a{ L"-o", L"BatchMode=yes", L"-o", L"PasswordAuthentication=no", L"-o", L"KbdInteractiveAuthentication=no",
        L"-o", L"PreferredAuthentications=publickey", L"-o", L"StrictHostKeyChecking=yes", L"-o", L"RequestTTY=no",
        L"-o", L"ConnectTimeout=15", L"-o", L"ServerAliveInterval=15", L"-o", L"ServerAliveCountMax=3" };
    if (!c.key.empty()) { a.push_back(L"-i"); a.push_back(c.key); a.push_back(L"-o"); a.push_back(L"IdentitiesOnly=yes"); }
    return a;
}
std::wstring Target(const Connection& c) { return c.user + L"@" + c.host; }

void WriteStdout(const std::string& s) { if (!s.empty()) { DWORD n = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), static_cast<DWORD>(s.size()), &n, nullptr); } }
void WriteStderr(const std::string& s) { if (!s.empty()) { DWORD n = 0; WriteFile(GetStdHandle(STD_ERROR_HANDLE), s.data(), static_cast<DWORD>(s.size()), &n, nullptr); } }
std::string JsonString(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }
struct Response {
    std::string operation = "";
    bool success = false;
    std::string status = "failed";
    std::optional<int> exitCode;
    bool timedOut = false, outcomeUncertain = false, partialPossible = false;
    bool localTerminationConfirmed = false;
    std::string stdoutText, stderrText, error, troubleshootingHint;
    std::string stdinError, captureError, cleanupError;
    bool stdoutTruncated = false, stderrTruncated = false;
    uint64_t durationMs = 0;
};
Response SetupFailure(const std::string& operation, const std::string& error) {
    Response r; r.operation = operation; r.status = "setup_error"; r.error = error; return r;
}

std::string Serialize(const Response& r) {
    std::ostringstream s;
    s << "{\"schema_version\":1,\"operation\":" << JsonString(r.operation)
      << ",\"success\":" << (r.success ? "true" : "false") << ",\"status\":" << JsonString(r.status)
      << ",\"exit_code\":";
    if (r.exitCode) s << *r.exitCode; else s << "null";
    s << ",\"timed_out\":" << (r.timedOut ? "true" : "false")
      << ",\"local_termination_confirmed\":" << (r.localTerminationConfirmed ? "true" : "false")
      << ",\"outcome_uncertain\":" << (r.outcomeUncertain ? "true" : "false")
      << ",\"partial_possible\":" << (r.partialPossible ? "true" : "false")
      << ",\"stdout\":" << JsonString(r.stdoutText) << ",\"stderr\":" << JsonString(r.stderrText)
      << ",\"stdout_truncated\":" << (r.stdoutTruncated ? "true" : "false")
      << ",\"stderr_truncated\":" << (r.stderrTruncated ? "true" : "false")
      << ",\"stdin_error\":" << JsonString(r.stdinError)
      << ",\"capture_error\":" << JsonString(r.captureError)
      << ",\"cleanup_error\":" << JsonString(r.cleanupError)
      << ",\"duration_ms\":" << r.durationMs << ",\"error\":" << JsonString(r.error) << "}\n";
    return s.str();
}
void Emit(Response r, int processExit) { WriteStdout(Serialize(r)); ExitProcess(static_cast<UINT>(processExit)); }

Response InvalidResponse(const std::string& operation, const std::wstring& error) {
    Response r; r.operation = operation.empty() ? "unknown" : operation; r.error = WideToUtf8(error); return r;
}
Response MapOutcome(const std::string&, std::optional<int>, const std::string&, const std::string&, const std::string& = {});

Response Execute(const CliOptions& o) {
    Response r; r.operation = WideToUtf8(o.operation);
    std::wstring error;
    auto batch = o.operation == L"exec" ? std::optional<std::wstring>() : BuildSftpBatch(o, &error);
    if (o.operation != L"exec" && !batch) { r.error = WideToUtf8(error); return r; }
    OpenSshExecutables openSsh;
    std::string setupError;
    const bool requireSftp = o.operation != L"exec";
    if (!FindSystemOpenSshExecutables(requireSftp, openSsh, setupError)) return SetupFailure(r.operation, setupError);
    const std::wstring& executable = o.operation == L"exec" ? openSsh.ssh : openSsh.sftp;
    std::vector<std::wstring> args;
    std::string input;
    if (o.operation == L"exec") {
        args.push_back(L"-T"); args.push_back(L"-n"); args.push_back(L"-p"); args.push_back(std::to_wstring(o.connection.port));
        auto common = CommonSshOptions(o.connection); args.insert(args.end(), common.begin(), common.end());
        args.push_back(Target(o.connection)); args.push_back(o.command);
    } else {
        args = { L"-S", openSsh.ssh, L"-b", L"-", L"-P", std::to_wstring(o.connection.port) };
        auto common = CommonSshOptions(o.connection); args.insert(args.end(), common.begin(), common.end());
        args.push_back(Target(o.connection));
        input = WideToUtf8(*batch);
        if (input.empty()) { r.error = "SFTP batch could not be encoded as UTF-8."; return r; }
    }
    ProcessResult p = RunProcess(executable, args, input, o.connection.timeoutMs);
    r.exitCode = ReportedExitCode(p);
    r.durationMs = p.durationMs;
    r.stdoutText = std::move(p.out.bytes); r.stderrText = std::move(p.err.bytes);
    r.stdoutTruncated = p.out.truncated; r.stderrTruncated = p.err.truncated;
    r.stdinError = std::move(p.stdinError); r.captureError = std::move(p.captureError); r.cleanupError = std::move(p.cleanupError);
    if (!p.started) {
        r.timedOut = p.timedOut;
        r.error = "Could not start or contain Windows OpenSSH: " + WinErrorText(p.winError);
        if (!r.cleanupError.empty()) r.error += " " + r.cleanupError;
        return r;
    }
    if (p.timedOut) {
        r.timedOut = true; r.localTerminationConfirmed = p.localTerminationConfirmed; r.outcomeUncertain = true; r.partialPossible = o.operation != L"exec";
        r.status = o.operation == L"exec" ? "timed_out" : "uncertain";
        const std::string localState = p.localTerminationConfirmed ? "The local OpenSSH process tree was confirmed terminated." : "Local OpenSSH process-tree termination could not be confirmed.";
        r.error = o.operation == L"exec"
            ? localState + " This does not prove the remote command stopped; it may still be running. No retry was attempted."
            : localState + " The transfer may be partial or may have completed remotely; inspect both endpoints before retrying.";
        if (!r.cleanupError.empty()) r.error += " " + r.cleanupError;
        if (!r.stdinError.empty()) r.error += " " + r.stdinError;
        if (!r.captureError.empty()) r.error += " " + r.captureError;
        return r;
    }
    if (p.winError != ERROR_SUCCESS || !r.stdinError.empty() || !r.captureError.empty() || !r.cleanupError.empty()) {
        r.error = p.winError != ERROR_SUCCESS ? "Local process/capture failure: " + WinErrorText(p.winError) : "Local process I/O or cleanup was incomplete.";
        if (!r.stdinError.empty()) r.error += " " + r.stdinError;
        if (!r.captureError.empty()) r.error += " " + r.captureError;
        if (!r.cleanupError.empty()) r.error += " " + r.cleanupError;
        r.outcomeUncertain = true;
        r.partialPossible = o.operation != L"exec";
        r.status = "uncertain";
        return r;
    }
    Response mapped = MapOutcome(WideToUtf8(o.operation), r.exitCode, r.stdoutText, r.stderrText);
    mapped.durationMs = r.durationMs; mapped.timedOut = r.timedOut; mapped.localTerminationConfirmed = r.localTerminationConfirmed;
    mapped.stdoutTruncated = r.stdoutTruncated; mapped.stderrTruncated = r.stderrTruncated;
    mapped.partialPossible = mapped.partialPossible || r.partialPossible;
    mapped.stdinError = r.stdinError; mapped.captureError = r.captureError; mapped.cleanupError = r.cleanupError;
    return mapped;
}

Response MapOutcome(const std::string& operation, std::optional<int> exitCode, const std::string& stdoutText, const std::string& stderrText, const std::string& localFailure) {
    Response r;
    r.operation = operation; r.exitCode = exitCode; r.stdoutText = stdoutText; r.stderrText = stderrText;
    const bool exec = operation == "exec";
    const bool ambiguous = !exitCode || *exitCode == 255;
    r.troubleshootingHint = (ambiguous || !exec) ? TroubleshootingHint(stderrText, !exec) : std::string();
    const auto withHint = [&](const std::string& message) { return r.troubleshootingHint.empty() ? message : message + " Troubleshooting hint: " + r.troubleshootingHint; };
    if (!exitCode) {
        r.status = "uncertain"; r.outcomeUncertain = true; r.partialPossible = !exec;
        r.error = withHint(exec ? "OpenSSH exit status was unavailable. Remote completion is unknown: the command may have run or may still be running. No automatic retry was attempted." : "SFTP exit status was unavailable. The transfer outcome is unknown and may be partial; no automatic retry was attempted.");
        if (!localFailure.empty()) r.error = localFailure + " " + r.error;
        return r;
    }
    if (*exitCode == 0) { r.success = true; r.status = "completed"; return r; }
    if (!exec && *exitCode == 255) {
        r.status = "transfer_failed"; r.outcomeUncertain = true; r.partialPossible = true;
        r.error = withHint("SFTP client/transfer failed with exit status 255; the destination's final state is unknown and the transfer may be partial.");
        return r;
    }
    if (exec && *exitCode == 255) {
        r.status = "uncertain"; r.outcomeUncertain = true;
        r.error = withHint("OpenSSH returned 255. Remote completion is unknown: the command may have run or may still be running. No automatic retry was attempted.");
        return r;
    }
    if (*exitCode >= 1 && *exitCode <= 254) {
        if (exec) { r.troubleshootingHint.clear(); r.status = "remote_command_failed"; r.error = "Remote command exited with status " + std::to_string(*exitCode) + "."; }
        else { r.status = "transfer_failed"; r.outcomeUncertain = true; r.partialPossible = true; r.error = withHint("SFTP transfer exited with status " + std::to_string(*exitCode) + "; the destination's final state is unknown and the transfer may be partial."); }
        return r;
    }
    r.status = "uncertain"; r.outcomeUncertain = true; r.partialPossible = !exec;
    r.error = withHint("OpenSSH returned an unrecognized client status; remote completion is unknown. No automatic retry was attempted.");
    return r;
}

std::wstring CurrentExecutable() {
    std::vector<wchar_t> b(32768);
    DWORD n = GetModuleFileNameW(nullptr, b.data(), static_cast<DWORD>(b.size()));
    return n && n < b.size() ? std::wstring(b.data(), n) : L"";
}
int TestChild(const std::wstring& mode) {
    if (mode == L"--test-child-output") { WriteStdout("child-out\n"); WriteStderr("child-err\n"); return 7; }
    if (mode == L"--test-child-stdin-verify") {
        constexpr size_t expectedSize = 256 * 1024;
        std::array<char, 8192> buffer{};
        size_t consumed = 0;
        while (consumed < expectedSize) {
            DWORD got = 0;
            const DWORD request = static_cast<DWORD>(std::min(buffer.size(), expectedSize - consumed));
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer.data(), request, &got, nullptr) || !got) return 10;
            for (DWORD i = 0; i < got; ++i) {
                const unsigned char expected = static_cast<unsigned char>(((consumed + i) * 31 + 7) & 0xff);
                if (static_cast<unsigned char>(buffer[i]) != expected) return 11;
            }
            consumed += got;
        }
        char extra = 0;
        DWORD extraBytes = 0;
        const BOOL extraRead = ReadFile(GetStdHandle(STD_INPUT_HANDLE), &extra, 1, &extraBytes, nullptr);
        if (extraBytes != 0 || (extraRead && extraBytes != 0) || (!extraRead && GetLastError() != ERROR_BROKEN_PIPE)) return 12;
        WriteStdout("stdin-verified\n");
        return 0;
    }
    if (mode == L"--test-child-flood" || mode == L"--test-child-flood-stdout-first" || mode == L"--test-child-flood-stderr-first") {
        const std::string out(8192, 'O'), err(8192, 'E');
        const auto writeAll = [](HANDLE handle, const std::string& bytes) {
            size_t offset = 0;
            while (offset < bytes.size()) {
                DWORD wrote = 0;
                if (!WriteFile(handle, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &wrote, nullptr) || !wrote) return false;
                offset += wrote;
            }
            return true;
        };
        if (mode == L"--test-child-flood-stdout-first") return writeAll(GetStdHandle(STD_OUTPUT_HANDLE), std::string(32 * 8192, 'O')) && writeAll(GetStdHandle(STD_ERROR_HANDLE), std::string(32 * 8192, 'E')) ? 0 : 8;
        if (mode == L"--test-child-flood-stderr-first") return writeAll(GetStdHandle(STD_ERROR_HANDLE), std::string(32 * 8192, 'E')) && writeAll(GetStdHandle(STD_OUTPUT_HANDLE), std::string(32 * 8192, 'O')) ? 0 : 8;
        for (unsigned i = 0; i < 32; ++i) {
            if (!writeAll(GetStdHandle(STD_OUTPUT_HANDLE), out) || !writeAll(GetStdHandle(STD_ERROR_HANDLE), err)) return 8;
        }
        return 0;
    }
    if (mode == L"--test-child-sleep" || mode == L"--test-child-never-read") { Sleep(10000); return 0; }
    if (mode == L"--test-child-sustained-output") {
        WriteStderr("stderr-marker-while-stdout-streams\n");
        const std::string chunk(8192, 'S');
        for (;;) {
            size_t sent = 0;
            while (sent < chunk.size()) {
                DWORD wrote = 0;
                if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), chunk.data() + sent, static_cast<DWORD>(chunk.size() - sent), &wrote, nullptr) || !wrote) return 0;
                sent += wrote;
            }
        }
    }
    if (mode == L"--test-child-descendant") {
        WriteStdout("parent-out\n"); WriteStderr("parent-err\n");
        const std::wstring self = CurrentExecutable();
        std::wstring line = BuildCommandLine(self, { L"--test-child-sleep" });
        STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(self.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 9;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0; // descendant deliberately retains both inherited output pipes
    }
    return -1;
}
struct TestCheck { std::string name; bool ok; std::string detail; };
std::string SelfTestJson(const std::vector<TestCheck>& checks) {
    bool ok = std::all_of(checks.begin(), checks.end(), [](const auto& c) { return c.ok; });
    std::ostringstream s; s << "{\"schema_version\":1,\"operation\":\"self-test\",\"success\":" << (ok ? "true" : "false") << ",\"checks\":[";
    for (size_t i = 0; i < checks.size(); ++i) {
        if (i) s << ',';
        s << "{\"name\":" << JsonString(checks[i].name) << ",\"success\":" << (checks[i].ok ? "true" : "false") << ",\"detail\":" << JsonString(checks[i].detail) << '}';
    }
    s << "]}\n"; return s.str();
}
std::vector<TestCheck> RunSelfTests() {
    std::vector<TestCheck> checks;
    const auto add = [&checks](std::string name, bool ok, std::string detail) { checks.push_back({ std::move(name), ok, std::move(detail) }); };
    add("windows_argument_quoting_spaces_and_quotes",
        QuoteWindowsArg(L"C:\\folder with spaces\\a\"b.exe") == L"\"C:\\folder with spaces\\a\\\"b.exe\"" && QuoteWindowsArg(L"trail\\") == L"\"trail\\\\\"",
        "CreateProcess argument quoting covers whitespace, embedded quotes, and trailing backslashes.");
    std::wstring sftpError;
    auto quoted = SftpQuotedPath(L"/Users/testuser/Folder with spaces/a\"b.txt", true, &sftpError);
    add("sftp_remote_path_spaces_and_quotes", quoted && *quoted == L"\"/Users/testuser/Folder with spaces/a\\\"b.txt\"", "Remote SFTP batch paths are quoted; quote and backslash escaping is checked.");
    CliOptions transferTest; transferTest.operation = L"upload"; transferTest.localPath = L"C:\\Temp Folder\\input file.txt"; transferTest.remotePath = L"/Users/testuser/Remote Folder/output file.txt";
    auto transferBatch = BuildSftpBatch(transferTest, &sftpError);
    add("sftp_local_and_remote_paths_with_spaces", transferBatch && *transferBatch == L"put \"C:/Temp Folder/input file.txt\" \"/Users/testuser/Remote Folder/output file.txt\"\n", "The complete upload batch quotes both local Windows and remote paths containing spaces.");
    auto badPath = SftpQuotedPath(L"/tmp/a\nb", true, &sftpError);
    add("sftp_rejects_batch_line_injection", !badPath, "Newlines and control bytes cannot inject extra SFTP batch commands.");
    auto globPath = SftpQuotedPath(L"/tmp/a*", true, &sftpError);
    add("sftp_rejects_remote_glob", !globPath, "Remote glob metacharacters are rejected instead of expanded ambiguously.");
    CliOptions bracketUpload; bracketUpload.operation = L"upload"; bracketUpload.localPath = L"C:\\Photos\\photo [1].jpg"; bracketUpload.remotePath = L"/tmp/photo.jpg";
    std::wstring bracketError;
    auto bracketBatch = BuildSftpBatch(bracketUpload, &bracketError);
    CliOptions bracketDownload = bracketUpload; bracketDownload.operation = L"download";
    std::wstring bracketDownloadError;
    auto bracketDownloadBatch = BuildSftpBatch(bracketDownload, &bracketDownloadError);
    add("sftp_upload_rejects_local_brackets", !bracketBatch && bracketError.find(L"[ or ]") != std::wstring::npos && bracketDownloadBatch.has_value(),
        "Upload local paths with [ or ] are rejected (sftp put would glob them); download destinations are not globbed and remain allowed.");
    Connection keepaliveConnection;
    const auto sshOptions = CommonSshOptions(keepaliveConnection);
    const auto hasOption = [&sshOptions](const wchar_t* value) {
        for (size_t i = 0; i + 1 < sshOptions.size(); ++i) if (sshOptions[i] == L"-o" && sshOptions[i + 1] == value) return true;
        return false;
    };
    add("ssh_options_include_keepalive", hasOption(L"ServerAliveInterval=15") && hasOption(L"ServerAliveCountMax=3") && hasOption(L"ConnectTimeout=15") && hasOption(L"StrictHostKeyChecking=yes"),
        "A dead connection is detected after about 45 seconds of unanswered keepalives instead of waiting for the full operation timeout.");
    const std::wstring self = CurrentExecutable();
    const fs::path fixtureRoot = fs::temp_directory_path() / fs::path(L"Mac MiniCli OpenSSH fixture " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const fs::path fixtureSystem = fixtureRoot / L"system";
    const fs::path fixtureOpenSsh = fixtureSystem / L"OpenSSH";
    const fs::path fixtureWork = fixtureRoot / L"working directory";
    const fs::path fixturePath = fixtureRoot / L"PATH decoys";
    std::error_code fixtureEc;
    fs::create_directories(fixtureOpenSsh, fixtureEc); fs::create_directories(fixtureWork, fixtureEc); fs::create_directories(fixturePath, fixtureEc);
    const auto touchFixture = [](const fs::path& path) { HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (h == INVALID_HANDLE_VALUE) return false; CloseHandle(h); return true; };
    const bool fixtureFiles = touchFixture(fixtureOpenSsh / L"ssh.exe") && touchFixture(fixtureOpenSsh / L"sftp.exe") && touchFixture(fixtureWork / L"ssh.exe") && touchFixture(fixtureWork / L"sftp.exe") && touchFixture(fixturePath / L"ssh.exe") && touchFixture(fixturePath / L"sftp.exe");
    std::array<wchar_t, 32768> previousDirectory{}; const DWORD previousDirectoryLength = GetCurrentDirectoryW(static_cast<DWORD>(previousDirectory.size()), previousDirectory.data());
    std::array<wchar_t, 32768> previousPath{}; const DWORD previousPathLength = GetEnvironmentVariableW(L"PATH", previousPath.data(), static_cast<DWORD>(previousPath.size()));
    const bool pathChanged = SetEnvironmentVariableW(L"PATH", fixturePath.c_str()) != FALSE;
    const bool directoryChanged = SetCurrentDirectoryW(fixtureWork.c_str()) != FALSE;
    OpenSshExecutables selectedFixture; std::string fixtureError;
    const bool selectedFixtureOk = ResolveOpenSshExecutables(fixtureSystem.wstring(), true, selectedFixture, fixtureError);
    const bool restoredDirectory = previousDirectoryLength && SetCurrentDirectoryW(previousDirectory.data());
    const bool restoredPath = SetEnvironmentVariableW(L"PATH", previousPathLength ? previousPath.data() : nullptr) != FALSE;
    const std::wstring expectedFixtureSsh = (fixtureOpenSsh / L"ssh.exe").wstring();
    const std::wstring expectedFixtureSftp = (fixtureOpenSsh / L"sftp.exe").wstring();
    add("openssh_selection_ignores_working_directory_and_path", fixtureFiles && pathChanged && directoryChanged && restoredDirectory && restoredPath && selectedFixtureOk && selectedFixture.ssh == expectedFixtureSsh && selectedFixture.sftp == expectedFixtureSftp, fixtureError.empty() ? "Selected absolute system-directory OpenSSH fixtures despite same-name working-directory/PATH decoys." : fixtureError);
    const std::wstring fixtureSftpExecutable = expectedFixtureSftp;
    const std::wstring sftpLaunch = BuildCommandLine(fixtureSftpExecutable, { L"-S", expectedFixtureSsh, L"-b", L"-" });
    const std::wstring expectedLaunch = QuoteWindowsArg(fixtureSftpExecutable) + L" " + QuoteWindowsArg(L"-S") + L" " + QuoteWindowsArg(expectedFixtureSsh) + L" " + QuoteWindowsArg(L"-b") + L" " + QuoteWindowsArg(L"-");
    add("sftp_S_ssh_path_is_one_quoted_argument_with_spaces", sftpLaunch == expectedLaunch && QuoteWindowsArg(expectedFixtureSsh).front() == L'"', WideToUtf8(sftpLaunch));
    fs::remove(fixtureOpenSsh / L"sftp.exe", fixtureEc);
    OpenSshExecutables execOnly; std::string missingSftpError, transferSetupError;
    const bool execOnlyWorks = ResolveOpenSshExecutables(fixtureSystem.wstring(), false, execOnly, missingSftpError);
    const bool transferMissingSftp = !ResolveOpenSshExecutables(fixtureSystem.wstring(), true, execOnly, transferSetupError);
    Response missingSftpUpload = SetupFailure("upload", transferSetupError);
    Response missingSftpDownload = SetupFailure("download", transferSetupError);
    add("openssh_missing_sftp_setup_errors_for_upload_and_download", execOnlyWorks && transferMissingSftp && missingSftpError.empty() && transferSetupError.find("sftp.exe") != std::string::npos && missingSftpUpload.status == "setup_error" && missingSftpDownload.status == "setup_error", transferSetupError);
    fs::remove(fixtureOpenSsh / L"ssh.exe", fixtureEc);
    OpenSshExecutables missingSsh; std::string missingSshError;
    const bool execMissingSsh = !ResolveOpenSshExecutables(fixtureSystem.wstring(), false, missingSsh, missingSshError);
    Response missingSshResponse = SetupFailure("exec", missingSshError);
    add("openssh_missing_ssh_is_exec_setup_error", execMissingSsh && missingSshError.find("ssh.exe") != std::string::npos && missingSshResponse.status == "setup_error", missingSshError);
    fs::remove_all(fixtureRoot, fixtureEc);
    auto output = RunProcess(self, { L"--test-child-output" }, {}, 3000, 1024);
    add("concurrent_stdout_stderr_and_exit_status", output.started && output.exitCode == 7 && output.out.bytes == "child-out\n" && output.err.bytes == "child-err\n", "Self-spawned child wrote both streams concurrently and returned exit status 7.");
    add("natural_exit_code_is_reported", !output.terminatedLocally && ReportedExitCode(output) == std::optional<int>(7), "A child's own exit status is reported unchanged.");
    auto mapSuccess = MapOutcome("exec", 0, "", "");
    add("map_exec_success", mapSuccess.success && mapSuccess.status == "completed" && mapSuccess.exitCode == std::optional<int>(0), mapSuccess.status);
    auto mapStdoutDenied = MapOutcome("exec", 7, "permission denied", "");
    add("map_exec_7_permission_in_stdout", !mapStdoutDenied.success && mapStdoutDenied.status == "remote_command_failed" && mapStdoutDenied.exitCode == std::optional<int>(7) && mapStdoutDenied.troubleshootingHint.empty() && mapStdoutDenied.error.find("status 7") != std::string::npos, mapStdoutDenied.error);
    auto mapStderrDenied = MapOutcome("exec", 7, "", "permission denied");
    add("map_exec_7_permission_in_stderr", mapStderrDenied.status == "remote_command_failed" && mapStderrDenied.exitCode == std::optional<int>(7) && mapStderrDenied.troubleshootingHint.empty() && mapStderrDenied.error.find("Troubleshooting hint") == std::string::npos, mapStderrDenied.error);
    auto map255Empty = MapOutcome("exec", 255, "", "");
    add("map_exec_255_empty_uncertain", map255Empty.status == "uncertain" && map255Empty.outcomeUncertain && map255Empty.exitCode == std::optional<int>(255) && map255Empty.error.find("may have run or may still be running") != std::string::npos && map255Empty.error.find("No automatic retry") != std::string::npos, map255Empty.error);
    auto map255Lost = MapOutcome("exec", 255, "", "Connection reset by peer");
    add("map_exec_255_connection_loss_uncertain", map255Lost.status == "uncertain" && map255Lost.outcomeUncertain && map255Lost.exitCode == std::optional<int>(255) && map255Lost.error.find("completion is unknown") != std::string::npos, map255Lost.error);
    auto mapAuthHint = MapOutcome("exec", 7, "", "Permission denied (publickey).");
    add("map_authentication_text_is_not_setup_hint_for_known_status", mapAuthHint.status == "remote_command_failed" && mapAuthHint.troubleshootingHint.empty() && mapAuthHint.error.find("authentication failed") == std::string::npos, mapAuthHint.error);
    auto mapHostHint = MapOutcome("exec", 255, "", "Host key verification failed.");
    add("map_host_key_text_is_hint_only", mapHostHint.status == "uncertain" && mapHostHint.troubleshootingHint.find("hint only") != std::string::npos && mapHostHint.error.find("host-key verification") != std::string::npos, mapHostHint.error);
    auto mapSftpUploadPermission = MapOutcome("upload", 1, "", "put: Permission denied");
    add("map_sftp_upload_permission_is_uncertain_partial_transfer", mapSftpUploadPermission.status == "transfer_failed" && mapSftpUploadPermission.partialPossible && mapSftpUploadPermission.outcomeUncertain && mapSftpUploadPermission.exitCode == std::optional<int>(1), mapSftpUploadPermission.error);
    auto mapSftpDownloadPermission = MapOutcome("download", 1, "", "get: Permission denied");
    add("map_sftp_download_permission_is_uncertain_partial_transfer", mapSftpDownloadPermission.status == "transfer_failed" && mapSftpDownloadPermission.partialPossible && mapSftpDownloadPermission.outcomeUncertain && mapSftpDownloadPermission.exitCode == std::optional<int>(1), mapSftpDownloadPermission.error);
    auto mapSftp255 = MapOutcome("upload", 255, "", "Connection reset by peer");
    add("map_sftp_255_transfer_failure_uncertain", mapSftp255.status == "transfer_failed" && mapSftp255.exitCode == std::optional<int>(255) && mapSftp255.outcomeUncertain && mapSftp255.partialPossible && mapSftp255.error.find("SFTP client/transfer failed") != std::string::npos && mapSftp255.error.find("unrecognized") == std::string::npos, mapSftp255.error);
    auto mapUnavailable = MapOutcome("exec", std::nullopt, "", "", "Local process wait failed: Windows error 6.");
    add("map_unavailable_exit_is_null_and_uncertain", !mapUnavailable.exitCode && mapUnavailable.status == "uncertain" && mapUnavailable.outcomeUncertain && mapUnavailable.error.find("exit status was unavailable") != std::string::npos && Serialize(mapUnavailable).find("\"exit_code\":null") != std::string::npos, mapUnavailable.error);
    auto flood = RunProcess(self, { L"--test-child-flood" }, {}, 5000, 4096);
    constexpr uint64_t floodBytesPerStream = 256 * 1024;
    add("256kib_per_stream_flood_bounded_capture", flood.started && flood.exitCode == 0 && !flood.timedOut && flood.out.totalBytes == floodBytesPerStream && flood.err.totalBytes == floodBytesPerStream && flood.out.bytes.size() == 4096 && flood.err.bytes.size() == 4096 && flood.out.truncated && flood.err.truncated,
        "exit=" + std::to_string(flood.exitCode) + " timeout=" + (flood.timedOut ? "true" : "false") + " duration_ms=" + std::to_string(flood.durationMs) + " stdout_drained_bytes=" + std::to_string(flood.out.totalBytes) + " stdout_captured_bytes=" + std::to_string(flood.out.bytes.size()) + " stdout_truncated=" + (flood.out.truncated ? "true" : "false") + " stderr_drained_bytes=" + std::to_string(flood.err.totalBytes) + " stderr_captured_bytes=" + std::to_string(flood.err.bytes.size()) + " stderr_truncated=" + (flood.err.truncated ? "true" : "false") + " win_error=" + std::to_string(flood.winError) + " capture_error=" + flood.captureError + " cleanup_error=" + flood.cleanupError);
    std::string verifiedInput(256 * 1024, '\0');
    for (size_t i = 0; i < verifiedInput.size(); ++i) verifiedInput[i] = static_cast<char>((i * 31 + 7) & 0xff);
    auto stdinVerify = RunProcess(self, { L"--test-child-stdin-verify" }, verifiedInput, 5000, 4096);
    const std::string stdinMarker = "stdin-verified\n";
    add("stdin_256kib_verified_and_eof", stdinVerify.started && stdinVerify.exitCode == 0 && !stdinVerify.timedOut && stdinVerify.stdinError.empty() && stdinVerify.out.bytes == stdinMarker && stdinVerify.out.totalBytes == stdinMarker.size() && stdinVerify.err.totalBytes == 0,
        "exit=" + std::to_string(stdinVerify.exitCode) + " timeout=" + (stdinVerify.timedOut ? "true" : "false") + " duration_ms=" + std::to_string(stdinVerify.durationMs) + " verified_input_bytes=262144 eof_verified=true stdout_bytes=" + std::to_string(stdinVerify.out.totalBytes) + " stdin_error=" + stdinVerify.stdinError);
    const auto checkOrderedFlood = [&](const wchar_t* mode, const char* name) {
        auto ordered = RunProcess(self, { mode }, {}, 5000, 4096);
        add(name, ordered.started && ordered.exitCode == 0 && !ordered.timedOut && ordered.out.totalBytes == floodBytesPerStream && ordered.err.totalBytes == floodBytesPerStream && ordered.out.bytes.size() == 4096 && ordered.err.bytes.size() == 4096 && ordered.out.truncated && ordered.err.truncated,
            "exit=" + std::to_string(ordered.exitCode) + " timeout=" + (ordered.timedOut ? "true" : "false") + " duration_ms=" + std::to_string(ordered.durationMs) + " stdout_drained_bytes=" + std::to_string(ordered.out.totalBytes) + " stdout_captured_bytes=" + std::to_string(ordered.out.bytes.size()) + " stdout_truncated=" + (ordered.out.truncated ? "true" : "false") + " stderr_drained_bytes=" + std::to_string(ordered.err.totalBytes) + " stderr_captured_bytes=" + std::to_string(ordered.err.bytes.size()) + " stderr_truncated=" + (ordered.err.truncated ? "true" : "false"));
    };
    checkOrderedFlood(L"--test-child-flood-stdout-first", "256kib_stdout_then_stderr_flood");
    checkOrderedFlood(L"--test-child-flood-stderr-first", "256kib_stderr_then_stdout_flood");
    auto timeout = RunProcess(self, { L"--test-child-sleep" }, {}, 200, 4096);
    add("process_timeout", timeout.started && timeout.timedOut && timeout.localTerminationConfirmed && timeout.durationMs < 2500, "Self-spawned 10-second child timed out and local Job Object termination was confirmed.");
    Response timeoutResponse; timeoutResponse.operation = "exec"; timeoutResponse.timedOut = true; timeoutResponse.exitCode = ReportedExitCode(timeout);
    add("timeout_exit_code_is_null", timeout.terminatedLocally && !ReportedExitCode(timeout) && Serialize(timeoutResponse).find("\"exit_code\":null") != std::string::npos,
        "raw_exit=" + std::to_string(timeout.exitCode) + "; the local termination status (124) is not reported as the OpenSSH or remote exit_code.");
    auto sustained = RunProcess(self, { L"--test-child-sustained-output" }, {}, 300, 4096);
    add("sustained_stdout_timeout_and_stderr_fairness", sustained.started && sustained.timedOut && sustained.localTerminationConfirmed && sustained.out.truncated && sustained.err.bytes.find("stderr-marker-while-stdout-streams") != std::string::npos && sustained.durationMs < 2500,
        "A child streamed stdout continuously while emitting a stderr marker; both streams were serviced and the 300 ms deadline/local cleanup were confirmed.");
    const std::string oversizedBatch(2 * 1024 * 1024, 'B');
    auto blockedInput = RunProcess(self, { L"--test-child-never-read" }, oversizedBatch, 200, 4096);
    add("blocked_stdin_write_deadline", blockedInput.started && blockedInput.timedOut && !blockedInput.stdinError.empty() && blockedInput.durationMs < 2500,
        "A child that never reads a 2 MiB batch timed out while asynchronous stdin delivery was pending; no unbounded write blocked the caller.");
    auto descendant = RunProcess(self, { L"--test-child-descendant" }, {}, 3000, 4096);
    add("inherited_pipe_descendant_cleanup", descendant.started && !descendant.timedOut && descendant.exitCode == 0 && descendant.out.bytes == "parent-out\n" && descendant.err.bytes == "parent-err\n" && descendant.captureError.empty() && descendant.cleanupError.empty() && descendant.durationMs < 2500,
        "A parent exited while a sleeping descendant retained both output pipes; job containment cleaned it up and both streams reached EOF.");
    return checks;
}

void PrintHelp() {
    WriteStderr("MacMiniCli - standalone Windows OpenSSH exec and SFTP client\n"
        "  MacMiniCli.exe exec --host HOST --user USER [--port 22] [--key FILE] [--timeout-ms N] -- COMMAND\n"
        "  MacMiniCli.exe upload --host HOST --user USER [--port 22] [--key FILE] [--timeout-ms N] --local FILE --remote PATH\n"
        "  MacMiniCli.exe download --host HOST --user USER [--port 22] [--key FILE] [--timeout-ms N] --remote PATH --local FILE\n"
        "  MacMiniCli.exe --self-test\n"
        "Output: one UTF-8 JSON object on stdout; progress/diagnostics on stderr. Public-key authentication only.\n");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && (std::wstring(argv[1]) == L"--test-child-output" || std::wstring(argv[1]) == L"--test-child-flood" || std::wstring(argv[1]) == L"--test-child-flood-stdout-first" || std::wstring(argv[1]) == L"--test-child-flood-stderr-first" || std::wstring(argv[1]) == L"--test-child-stdin-verify" || std::wstring(argv[1]) == L"--test-child-sleep" || std::wstring(argv[1]) == L"--test-child-never-read" || std::wstring(argv[1]) == L"--test-child-sustained-output" || std::wstring(argv[1]) == L"--test-child-descendant"))
        return TestChild(argv[1]);
    if (argc == 2 && std::wstring(argv[1]) == L"--self-test") {
        auto checks = RunSelfTests();
        bool success = std::all_of(checks.begin(), checks.end(), [](const auto& c) { return c.ok; });
        for (const auto& c : checks) WriteStderr(std::string("MacMiniCli self-test: ") + (c.ok ? "PASS " : "FAIL ") + c.name + "\n");
        WriteStdout(SelfTestJson(checks));
        return success ? 0 : 1;
    }
    if (argc == 2 && (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h")) {
        PrintHelp(); Response r; r.operation = "help"; r.success = true; r.status = "help"; WriteStdout(Serialize(r)); return 0;
    }
    CliOptions o = ParseArgs(argc, argv);
    if (!o.parseError.empty()) {
        Response r = InvalidResponse(WideToUtf8(o.operation), o.parseError);
        WriteStderr("MacMiniCli: invalid arguments; see error in JSON response.\n");
        WriteStdout(Serialize(r)); return 2;
    }
    Response r = Execute(o);
    const int exit = r.success ? 0 : (r.timedOut ? 124 : 1);
    WriteStdout(Serialize(r));
    return exit;
}
