#include "console.h"
#include <windows.h>
#include <iostream>

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
        hide();
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
    ::FreeConsole();
}

void console::hide()
{
    ::ShowWindow(h_window_, SW_HIDE);
}
