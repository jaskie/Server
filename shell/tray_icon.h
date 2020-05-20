#pragma once
#include <windows.h>
#include <tchar.h>
#include <shellapi.h>

class tray_icon
{
public:
    tray_icon(HINSTANCE h_instance, const wchar_t* window_name);
    ~tray_icon();
    void close();
    HWND GetWindow() const;
private:
    HWND hidden_window_;
    const HINSTANCE h_instance_;
    ATOM a_class_;
};

