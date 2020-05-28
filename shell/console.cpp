#include "console.h"
#include <windows.h>
#include <iostream>

console::console(bool hide_on_start)
    : allocated_(AllocConsole())
    , h_window_(GetConsoleWindow())
{
    if (!allocated_)
        return;
    if (hide_on_start) 
        ShowWindow(h_window_, SW_HIDE);
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
    FreeConsole();
}
