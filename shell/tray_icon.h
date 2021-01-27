#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <tchar.h>
#include <shellapi.h>

class tray_icon
{
public:
    tray_icon(HINSTANCE h_instance);
    ~tray_icon();
    void close();
    static bool show_previous_instance();
private:
    HWND hidden_window_;
    const HINSTANCE h_instance_;
    ATOM a_class_;
};

