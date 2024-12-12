#include "console.h"
#include <windows.h>
#include <iostream>
#include "resource.h"
#include "version.h"
#include <common/env.h>

void CALLBACK WinEventHandler(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime
) 
{
    if (hwnd != ::GetConsoleWindow() || event != EVENT_SYSTEM_MINIMIZESTART)
        return;
    ::ShowWindow(hwnd, SW_HIDE);
}

console::console(bool hide_on_start)
    : allocated_(::AllocConsole())
    , h_window_(::GetConsoleWindow())
    , g_hook_(::SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, NULL, WinEventHandler, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
{
    if (!allocated_)
        return;

    if (hide_on_start)
        ::ShowWindow(h_window_, SW_HIDE);
    // disable Ctrl+C
    ::SetConsoleCtrlHandler(NULL, true);
    
    auto hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);

    // Disable close button in console to avoid shutdown without cleanup.
    ::EnableMenuItem(::GetSystemMenu(h_window_, FALSE), SC_CLOSE, MF_GRAYED);
    ::DrawMenuBar(h_window_);

    // Configure console size and position.
    auto coord = ::GetLargestConsoleWindowSize(hOut);
    coord.X /= 2;

    ::SetConsoleScreenBufferSize(hOut, coord);

    SMALL_RECT display_area = { 0, 0, 0, 0 };
    display_area.Right = coord.X - 1;
    display_area.Bottom = (coord.Y - 1) / 2;
    ::SetConsoleWindowInfo(hOut, TRUE, &display_area);

    // set the window title before configuration file is read
    set_window_title_prefix(L"");

    // redirect streams
    cin_buffer = std::wcin.rdbuf();
    cout_buffer = std::wcout.rdbuf();
    cerr_buffer = std::wcerr.rdbuf();
    console_input.open("CONIN$", std::ios::in);
    console_output.open("CONOUT$", std::ios::out);
    console_error.open("CONOUT$", std::ios::out);
    std::wcin.rdbuf(console_input.rdbuf());
    std::wcout.rdbuf(console_output.rdbuf());
    std::wcerr.rdbuf(console_error.rdbuf());

}

console::~console()
{
    ::UnhookWinEvent(g_hook_);
    if (!allocated_)
        return;
    ::FreeConsole();
    console_input.close();
    console_output.close();
    console_error.close();
    std::wcin.rdbuf(cin_buffer);
    std::wcout.rdbuf(cout_buffer);
    std::wcerr.rdbuf(cerr_buffer);
}

void console::terminate()
{
    if (!allocated_)
        return;
    std::wcin.setstate(std::ios_base::badbit, true);
}

void console::hide()
{
    ::ShowWindow(h_window_, SW_MINIMIZE);
}

void console::set_window_title_prefix(const std::wstring& window_title)
{
    std::wstringstream str;
    if (!window_title.empty())
        str << window_title << L" | ";
    str << CASPAR_NAME << L" " << caspar::env::version();
#ifdef COMPILE_PROFILE
    str << " Profile";
#elif  COMPILE_DEVELOP
    str << " Develop";
#elif  COMPILE_DEBUG
    str << " Debug";
#endif
    ::SetConsoleTitle(str.str().c_str());
}
