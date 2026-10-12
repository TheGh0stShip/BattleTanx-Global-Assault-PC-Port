#include "btga/platform.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>

namespace btga {
class Win32Platform final : public Platform {
  public:
    explicit Win32Platform(std::string_view) {
        WNDCLASSA wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "BattleTanxGAPC"; RegisterClassA(&wc);
        window_ = CreateWindowA(wc.lpszClassName, "BattleTanx: Global Assault PC bring-up",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                kWidth + 16, kHeight + 39, nullptr, nullptr, wc.hInstance, this);
        info_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info_.bmiHeader.biWidth = kWidth;
        info_.bmiHeader.biHeight = -kHeight; info_.bmiHeader.biPlanes = 1;
        info_.bmiHeader.biBitCount = 32; info_.bmiHeader.biCompression = BI_RGB;
    }
    bool pump(Input& input) override {
        MSG message{};
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message); DispatchMessage(&message);
        }
        input = {(GetAsyncKeyState('W') & 0x8000) != 0, (GetAsyncKeyState('S') & 0x8000) != 0,
                 (GetAsyncKeyState('A') & 0x8000) != 0, (GetAsyncKeyState('D') & 0x8000) != 0,
                 (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0};
        return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0;
    }
    void present(const Framebuffer& framebuffer) override {
        HDC dc = GetDC(window_); StretchDIBits(dc, 0, 0, kWidth, kHeight, 0, 0, kWidth, kHeight,
            framebuffer.pixels.data(), &info_, DIB_RGB_COLORS, SRCCOPY); ReleaseDC(window_, dc);
    }
  private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProc(hwnd, message, w, l);
    }
    HWND window_{}; BITMAPINFO info_{};
};
Platform* Platform::create(std::string_view title) { return new Win32Platform(title); }
} // namespace btga
