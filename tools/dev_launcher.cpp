#include <Windows.h>
#include <TlHelp32.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct FileStamp {
    bool exists = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type writeTime{};
};

FileStamp CaptureFileStamp(const std::filesystem::path& path) {
    std::error_code error;
    FileStamp stamp{};
    stamp.exists = std::filesystem::is_regular_file(path, error);
    if (!stamp.exists || error) {
        return {};
    }
    stamp.size = std::filesystem::file_size(path, error);
    if (error) {
        return {};
    }
    stamp.writeTime = std::filesystem::last_write_time(path, error);
    if (error) {
        return {};
    }
    return stamp;
}

bool FileChanged(const FileStamp& before, const FileStamp& after) {
    return after.exists &&
           (!before.exists || before.size != after.size ||
            before.writeTime != after.writeTime);
}

std::wstring Win32Message(const DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length != 0 && buffer != nullptr
        ? std::wstring(buffer, length)
        : L"unknown error";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

std::wstring BaseName(const std::wstring& path) {
    return std::filesystem::path(path).filename().wstring();
}

std::uintptr_t RemoteModuleBase(const DWORD processId,
                                const std::wstring& moduleName) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::uintptr_t result = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName.c_str()) == 0) {
                result = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::uintptr_t RemoteLoadLibraryAddress(const DWORD processId,
                                        std::wstring& error) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const FARPROC loadLibrary = kernel32 == nullptr
        ? nullptr
        : GetProcAddress(kernel32, "LoadLibraryW");
    if (loadLibrary == nullptr) {
        error = L"Could not resolve local LoadLibraryW";
        return 0;
    }

    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(loadLibrary), &owner)) {
        error = L"Could not resolve the module owning LoadLibraryW";
        return 0;
    }
    std::vector<wchar_t> ownerPath(32768);
    const DWORD copied = GetModuleFileNameW(
        owner, ownerPath.data(), static_cast<DWORD>(ownerPath.size()));
    if (copied == 0 || copied >= ownerPath.size()) {
        error = L"Could not resolve the path of the LoadLibraryW owner";
        return 0;
    }
    const std::wstring ownerName = BaseName(std::wstring(ownerPath.data(), copied));
    const std::uintptr_t remoteOwner = RemoteModuleBase(processId, ownerName);
    if (remoteOwner == 0) {
        error = L"Remote process has no loaded " + ownerName;
        return 0;
    }
    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(loadLibrary) -
                                  reinterpret_cast<std::uintptr_t>(owner);
    return remoteOwner + offset;
}

int TerminateLaunchedProcess(PROCESS_INFORMATION& process,
                             const std::wstring& message) {
    std::wcerr << L"FAIL: " << message << L'\n';
    TerminateProcess(process.hProcess, EXIT_FAILURE);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return EXIT_FAILURE;
}

} // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount != 3 && argumentCount != 4) {
        std::wcerr << L"usage: atb_dev_launcher <game.exe> <plugin.asi> [--wait]\n";
        return EXIT_FAILURE;
    }
    const bool waitForExit = argumentCount == 4 &&
                             std::wstring(arguments[3]) == L"--wait";
    if (argumentCount == 4 && !waitForExit) {
        std::wcerr << L"FAIL: the only supported optional argument is --wait\n";
        return EXIT_FAILURE;
    }

    std::error_code filesystemError;
    const std::filesystem::path executable =
        std::filesystem::absolute(arguments[1], filesystemError);
    if (filesystemError || !std::filesystem::is_regular_file(executable)) {
        std::wcerr << L"FAIL: target executable does not exist\n";
        return EXIT_FAILURE;
    }
    const std::filesystem::path plugin =
        std::filesystem::absolute(arguments[2], filesystemError);
    if (filesystemError || !std::filesystem::is_regular_file(plugin)) {
        std::wcerr << L"FAIL: plugin does not exist\n";
        return EXIT_FAILURE;
    }
    std::filesystem::path logPath = plugin;
    logPath.replace_extension(L".log");
    const FileStamp logBefore = CaptureFileStamp(logPath);

    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = executable.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), mutableCommandLine.data(), nullptr,
                        nullptr, FALSE, 0, nullptr,
                        workingDirectory.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        std::wcerr << L"FAIL: CreateProcessW returned " << error << L": "
                   << Win32Message(error) << L'\n';
        return EXIT_FAILURE;
    }

    USHORT processMachine = 0;
    USHORT nativeMachine = 0;
    if (!IsWow64Process2(process.hProcess, &processMachine, &nativeMachine) ||
        processMachine != IMAGE_FILE_MACHINE_UNKNOWN ||
        nativeMachine != IMAGE_FILE_MACHINE_AMD64) {
        return TerminateLaunchedProcess(
            process, L"target is not a native x64 process");
    }

    std::wstring resolutionError;
    std::uintptr_t remoteLoadLibrary = 0;
    const ULONGLONG resolutionDeadline = GetTickCount64() + 10000;
    do {
        remoteLoadLibrary =
            RemoteLoadLibraryAddress(process.dwProcessId, resolutionError);
        if (remoteLoadLibrary != 0 ||
            WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        Sleep(1);
    } while (GetTickCount64() < resolutionDeadline);
    if (remoteLoadLibrary == 0) {
        return TerminateLaunchedProcess(process, resolutionError);
    }

    const std::wstring pluginPath = plugin.wstring();
    const SIZE_T pathBytes = (pluginPath.size() + 1) * sizeof(wchar_t);
    void* const remotePath = VirtualAllocEx(
        process.hProcess, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (remotePath == nullptr) {
        return TerminateLaunchedProcess(
            process, L"VirtualAllocEx failed: " + Win32Message(GetLastError()));
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process.hProcess, remotePath, pluginPath.c_str(),
                            pathBytes, &written) || written != pathBytes) {
        VirtualFreeEx(process.hProcess, remotePath, 0, MEM_RELEASE);
        return TerminateLaunchedProcess(
            process, L"WriteProcessMemory failed: " + Win32Message(GetLastError()));
    }

    const HANDLE injectionThread = CreateRemoteThread(
        process.hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoadLibrary),
        remotePath, 0, nullptr);
    if (injectionThread == nullptr) {
        VirtualFreeEx(process.hProcess, remotePath, 0, MEM_RELEASE);
        return TerminateLaunchedProcess(
            process, L"CreateRemoteThread failed: " + Win32Message(GetLastError()));
    }
    const DWORD waitResult = WaitForSingleObject(injectionThread, 30000);
    DWORD remoteResult = 0;
    const bool gotResult = GetExitCodeThread(injectionThread, &remoteResult) != FALSE;
    CloseHandle(injectionThread);
    VirtualFreeEx(process.hProcess, remotePath, 0, MEM_RELEASE);
    if (waitResult != WAIT_OBJECT_0 || !gotResult || remoteResult == 0) {
        return TerminateLaunchedProcess(
            process, L"remote LoadLibraryW did not complete successfully");
    }

    bool bootstrapLogged = false;
    const ULONGLONG logDeadline = GetTickCount64() + 5000;
    do {
        if (FileChanged(logBefore, CaptureFileStamp(logPath))) {
            bootstrapLogged = true;
            break;
        }
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        Sleep(20);
    } while (GetTickCount64() < logDeadline);
    if (!bootstrapLogged) {
        return TerminateLaunchedProcess(
            process, L"plugin bootstrap did not update " + logPath.wstring());
    }

    if (waitForExit) {
        std::wcout << L"Injected " << plugin << L" into PID "
                   << process.dwProcessId << L" running " << executable
                   << L"; plugin bootstrap log verified\n";
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = EXIT_FAILURE;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        return static_cast<int>(exitCode);
    }

    const DWORD survivalResult = WaitForSingleObject(process.hProcess, 2000);
    if (survivalResult == WAIT_OBJECT_0) {
        DWORD exitCode = EXIT_FAILURE;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        std::wcerr << L"FAIL: injected process exited during the two-second "
                      L"startup check (exit code "
                   << exitCode << L"). Ensure no other game instance is running.\n";
        return EXIT_FAILURE;
    }
    if (survivalResult == WAIT_FAILED) {
        const DWORD error = GetLastError();
        return TerminateLaunchedProcess(
            process, L"startup process check returned " +
                         std::to_wstring(error) + L": " + Win32Message(error));
    }
    std::wcout << L"Injected " << plugin << L" into PID "
               << process.dwProcessId << L" running " << executable
               << L"; plugin log verified and process remained active through "
                  L"startup check\n";
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return EXIT_SUCCESS;
}
