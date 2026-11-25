// Windows side of ps2exit (see include/runtime/ps2_exit.h): watches the game window's own
// messages for a close - the close button, Alt+F4, the taskbar, Windows shutting down - so the
// game reacts to it straight away, whatever state its frame loop is in. Kept apart from
// ps2_exit.cpp because <windows.h> and raylib.h clash.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>

namespace ps2exit
{
    namespace
    {
        WNDPROC g_previous = nullptr;
        std::atomic<bool> g_closeAsked{false};

        LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if ((message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONUP) && wParam == HTCLOSE)
                std::fprintf(stderr, "[exit] close button %s\n", message == WM_NCLBUTTONDOWN ? "pressed" : "released");
            // A release on the close button reaching the window means Windows' own tracking of the
            // press missed it: it is a click on the close button all the same.
            const bool close = message == WM_CLOSE || (message == WM_SYSCOMMAND && (wParam & 0xFFF0u) == SC_CLOSE) ||
                               (message == WM_ENDSESSION && wParam != 0) || (message == WM_NCLBUTTONUP && wParam == HTCLOSE);
            if (close && !g_closeAsked.exchange(true))
                std::fprintf(stderr, "[exit] the window was asked to close (message 0x%x)\n", message);
            return CallWindowProcW(g_previous, hwnd, message, wParam, lParam);
        }
    }

    void watchWindowClose(void *window)
    {
        if (!window || g_previous)
            return;
        g_previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(static_cast<HWND>(window), GWLP_WNDPROC,
                                                                 reinterpret_cast<LONG_PTR>(&windowProc)));
    }

    bool closeAsked() noexcept
    {
        return g_closeAsked.load(std::memory_order_relaxed);
    }
}
#endif
