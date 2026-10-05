#include <nodsynth/song/Process.h>

#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace nodsynth::song {
namespace {
#if defined(_WIN32)
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::wstring quoteArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    int backslashes = 0;
    for (const wchar_t character : arg) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') out.append(static_cast<std::size_t>(backslashes) * 2 + 1, L'\\');
        else out.append(static_cast<std::size_t>(backslashes), L'\\');
        backslashes = 0;
        out.push_back(character);
    }
    out.append(static_cast<std::size_t>(backslashes) * 2, L'\\');
    out.push_back(L'"');
    return out;
}
#endif
} // namespace

ProcessResult runProcess(const ProcessRequest& request) {
    ProcessResult result;
    if (request.arguments.empty() || request.arguments.front().empty()) {
        result.message = "missing executable";
        return result;
    }
#if defined(_WIN32)
    std::wstring command;
    for (const auto& argument : request.arguments) {
        if (!command.empty()) command.push_back(L' ');
        command += quoteArg(widen(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    startup.hStdInput = nul;
    startup.hStdOutput = nul;
    startup.hStdError = nul;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == 0) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        result.message = "failed to start the external renderer";
        return result;
    }
    result.started = true;
    const auto wait = WaitForSingleObject(process.hProcess, request.timeoutMs == 0 ? INFINITE : request.timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
        result.timedOut = true;
        result.message = "external renderer timed out";
    } else {
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        result.exitCode = static_cast<int>(code);
        if (code != 0) result.message = "external renderer failed";
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return result;
#else
    std::vector<std::string> storage = request.arguments;
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto pid = fork();
    if (pid < 0) {
        result.message = "failed to start the external renderer";
        return result;
    }
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    result.started = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeoutMs == 0 ? 3600000 : request.timeoutMs);
    for (;;) {
        int status = 0;
        const auto done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
            else result.exitCode = 1;
            if (result.exitCode != 0) result.message = "external renderer failed";
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result.timedOut = true;
            result.message = "external renderer timed out";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}
} // namespace nodsynth::song
