#include "tray_icon.h"
#include "version.h"
#include "resource.h"
#include <string.h>
#include <string>

const UINT WM_TRAY = WM_USER + 1;
const UINT WM_RESTORE = WM_USER + 2;

BOOL show_and_restore_if_minimized()
{
	HWND h_console = ::GetConsoleWindow();
	::ShowWindow(h_console, SW_SHOW);
	WINDOWPLACEMENT place;
	memset(&place, 0, sizeof(WINDOWPLACEMENT));
	place.length = sizeof(WINDOWPLACEMENT);
	::GetWindowPlacement(h_console, &place);
	if (place.showCmd == SW_SHOWMINIMIZED)
		::ShowWindow(h_console, SW_RESTORE);
	return ::SetForegroundWindow(h_console);
}

void hide_console_window()
{
	::ShowWindow(::GetConsoleWindow(), SW_MINIMIZE);
}

LRESULT CALLBACK HiddenWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CREATE:
	{
		NOTIFYICONDATA stData;
		::ZeroMemory(&stData, sizeof(stData));
		stData.cbSize = sizeof(stData);
		stData.hWnd = hWnd;
		stData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		stData.uCallbackMessage = WM_TRAY;
		stData.hIcon = ::LoadIcon(GetModuleHandle(0), MAKEINTRESOURCE(101));
		wcscpy(stData.szTip, CASPAR_NAME);
		if (!::Shell_NotifyIcon(NIM_ADD, &stData))
			return -1; 
	}
	return 0;

	case WM_DESTROY:
	{
		NOTIFYICONDATA stData;
		::ZeroMemory(&stData, sizeof(stData));
		stData.cbSize = sizeof(stData);
		stData.hWnd = hWnd;
		::Shell_NotifyIcon(NIM_DELETE, &stData);
	}
	return 0;

	case WM_TRAY:
		switch (lParam)
		{
		case WM_LBUTTONDBLCLK:
			show_and_restore_if_minimized();
			break;

		case WM_RBUTTONDOWN:
			HMENU hMenu = ::LoadMenu(GetModuleHandle(0), MAKEINTRESOURCE(IDR_TRAY_MENU));
			if (hMenu)
			{
				HMENU hSubMenu = ::GetSubMenu(hMenu, 0);
				if (hSubMenu)
				{
					HWND h_console = ::GetConsoleWindow();
					if (::IsWindowVisible(h_console))
						::DeleteMenu(hMenu, ID_CASPARCG_SHOW, MF_BYCOMMAND);
					else
						::DeleteMenu(hMenu, ID_CASPARCG_HIDE, MF_BYCOMMAND);
					POINT stPoint;
					::GetCursorPos(&stPoint);
					::SetForegroundWindow(hWnd);
					::TrackPopupMenu(hSubMenu, TPM_BOTTOMALIGN | TPM_LEFTBUTTON, stPoint.x, stPoint.y, 0, hWnd, NULL);
				}
				::DestroyMenu(hMenu);
			}
			break;
		}
		return 0;

	case WM_RESTORE:
		show_and_restore_if_minimized();
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case ID_CASPARCG_SHOW:
			show_and_restore_if_minimized();
			break;
		case ID_CASPARCG_HIDE:
			hide_console_window();
			break;
		case ID_CASPARCG_CLOSE:
			if (::MessageBoxW(NULL, L"Are you sure to close CasparCG server?\nThis will terminate your broadcast.", CASPAR_NAME, MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2 | MB_TOPMOST) != IDYES)
				return 0;
			::PostQuitMessage(0);
			break;
		}
		return 0;
	}
	return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
}

tray_icon::tray_icon(HINSTANCE h_instance)
	: h_instance_(h_instance)
	, hidden_window_(0)
{
	WNDCLASS stWC;
	::ZeroMemory(&stWC, sizeof(stWC));
	stWC.lpszClassName = CASPAR_NAME;
	stWC.hInstance = h_instance;
	stWC.lpfnWndProc = HiddenWndProc;
	a_class_ = ::RegisterClass(&stWC);
	if (!a_class_)
		return;
	hidden_window_ = ::CreateWindowEx(WS_EX_NOACTIVATE, CASPAR_NAME, CASPAR_NAME, WS_MINIMIZE, 0, 0, 0, 0, NULL, NULL, h_instance, NULL);
}

tray_icon::~tray_icon()
{
	if (::IsWindow(hidden_window_))
		::DestroyWindow(hidden_window_);
	if (a_class_)
		::UnregisterClass((LPCTSTR)a_class_, h_instance_);
}

void tray_icon::close()
{
	::PostMessage(hidden_window_, WM_QUIT, 0, 0);
}

void tray_icon::show_previous_instance()
{
	HWND window = ::FindWindow(CASPAR_NAME, CASPAR_NAME);
	if (window == NULL)
		return;
	SendMessage(window, WM_RESTORE, 0, 0);
}

