#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

int wmain(const int argumentCount, wchar_t** arguments) {
    if (argumentCount != 2) {
        std::cerr << "usage: asi_bootstrap_host <plugin.asi>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path pluginPath(arguments[1]);
    std::filesystem::path logPath = pluginPath;
    logPath.replace_extension(L".log");
    std::error_code ignored;
    std::filesystem::remove(logPath, ignored);

    const HMODULE plugin = LoadLibraryW(pluginPath.c_str());
    if (plugin == nullptr) {
        std::cerr << "LoadLibraryW failed: " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }

    constexpr const char* expected =
        "Bootstrap ready. No runtime mappings or hooks were requested.";
    bool ready = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream stream(logPath, std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(stream)),
                                   std::istreambuf_iterator<char>());
        if (contents.find(expected) != std::string::npos) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!ready) {
        std::cerr << "bootstrap did not reach its no-hooks ready state\n";
        return EXIT_FAILURE;
    }
    // ASI loaders retain plugins for the process lifetime. Do the same here:
    // the ready log is emitted just before the bootstrap thread returns, so an
    // immediate FreeLibrary could race that thread's final return sequence.
    (void)plugin;
    std::cout << "ASI loaded, inspected its host, and failed safe without hooks\n";
    return EXIT_SUCCESS;
}
