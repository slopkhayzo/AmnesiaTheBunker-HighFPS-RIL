#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (GetModuleHandleW(L"AmnesiaTheBunker.FPSFix.asi") != nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::cout << "development launcher injected the x64 ASI\n";
            return EXIT_SUCCESS;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cerr << "FAIL: injected ASI was not visible in the target process\n";
    return EXIT_FAILURE;
}
