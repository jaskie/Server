/*
* Copyright 2013 Sveriges Television AB http://casparcg.com/
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

// tbbmalloc_proxy: 
// Replace the standard memory allocation routines in Microsoft* C/C++ RTL 
// (malloc/free, global new/delete, etc.) with the TBB memory allocator. 

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#ifdef _DEBUG
	#define _CRTDBG_MAP_ALLOC
	#include <stdlib.h>
	#include <crtdbg.h>
#else
	#include <tbb/tbbmalloc_proxy.h>
#endif

#include "resource.h"

#include "server.h"
#include "tray_icon.h"
#include "console.h"
#include "version.h"

#include <locale>

#include <windows.h>
#include <winnt.h>
#include <mmsystem.h>
#include <atlbase.h>
#include <ShellAPI.h>

#include <protocol/amcp/AMCPProtocolStrategy.h>

#include <modules/bluefish/bluefish.h>
#include <modules/decklink/decklink.h>
#include <modules/flash/flash.h>
#include <modules/ffmpeg/ffmpeg.h>
#include <modules/image/image.h>
#include <modules/newtek/util/air_send.h>
#include <modules/ndi/ndi.h>

#include <common/env.h>
#include <common/exception/win32_exception.h>
#include <common/exception/exceptions.h>
#include <common/log/log.h>
#include <common/gl/gl_check.h>
#include <common/os/windows/current_version.h>
#include <common/os/windows/system_info.h>

#include <tbb/task_scheduler_init.h>
#include <tbb/task_scheduler_observer.h>

#include <boost/property_tree/detail/file_parser_error.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>
#include <boost/thread/future.hpp>
#include <boost/locale.hpp>
#include <boost/algorithm/string/case_conv.hpp>

// NOTE: This is needed in order to make CComObject work since this is not a real ATL project.
CComModule _AtlModule;
extern __declspec(selectany) CAtlModule* _pAtlModule = &_AtlModule;


void setup_global_locale()
{
	boost::locale::generator gen;
	gen.categories(boost::locale::codepage_facet);

	std::locale::global(gen(""));
}

void print_info()
{
	CASPAR_LOG(info) << L"############################################################################";
	CASPAR_LOG(info) << L"CasparCG Server is distributed by the Swedish Broadcasting Corporation (SVT)";
	CASPAR_LOG(info) << L"under the GNU General Public License GPLv3 or higher.";
	CASPAR_LOG(info) << L"Please see LICENSE.TXT for details.";
	CASPAR_LOG(info) << L"http://www.casparcg.com/";
	CASPAR_LOG(info) << L"############################################################################";
	CASPAR_LOG(info) << L"Starting CasparCG Video and Graphics Playout Server " << caspar::env::version();
	CASPAR_LOG(info) << L"on " << caspar::get_win_product_name() << L" " << caspar::get_win_sp_version();
	CASPAR_LOG(info) << caspar::get_cpu_info();
	CASPAR_LOG(info) << caspar::get_system_product_name();
	
	CASPAR_LOG(info) << L"Decklink: " << caspar::decklink::get_version();
	BOOST_FOREACH(auto device, caspar::decklink::get_device_list())
		CASPAR_LOG(info) << L" - " << device;	
		
	CASPAR_LOG(info) << L"Bluefish: " << caspar::bluefish::get_version();
	BOOST_FOREACH(auto device, caspar::bluefish::get_device_list())
		CASPAR_LOG(info) << L" - " << device;	
	
	CASPAR_LOG(info) << L"Newtek NDI: "				<< caspar::ndi::get_version();
	CASPAR_LOG(info) << L"NewTek iVGA: " << (caspar::newtek::airsend::is_available() ? L"available" : L"unavailable (" + caspar::newtek::airsend::dll_name() + L")");
	CASPAR_LOG(info) << L"FreeImage: "		<< caspar::image::get_version();
	CASPAR_LOG(info) << L"FFMPEG-avcodec: "  << caspar::ffmpeg::get_avcodec_version();
	CASPAR_LOG(info) << L"FFMPEG-avformat: " << caspar::ffmpeg::get_avformat_version();
	CASPAR_LOG(info) << L"FFMPEG-avfilter: " << caspar::ffmpeg::get_avfilter_version();
	CASPAR_LOG(info) << L"FFMPEG-avutil: "	<< caspar::ffmpeg::get_avutil_version();
	CASPAR_LOG(info) << L"FFMPEG-swscale: "  << caspar::ffmpeg::get_swscale_version();
	CASPAR_LOG(info) << L"Flash: "			<< caspar::flash::get_version();
	CASPAR_LOG(info) << L"Template-Host: "	<< caspar::flash::get_cg_version();
}

LONG WINAPI UserUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
	try
	{
		CASPAR_LOG(fatal) << L"#######################\n UNHANDLED EXCEPTION: \n" 
			<< L"Adress:" << info->ExceptionRecord->ExceptionAddress << L"\n"
			<< L"Code:" << info->ExceptionRecord->ExceptionCode << L"\n"
			<< L"Flag:" << info->ExceptionRecord->ExceptionFlags << L"\n"
			<< L"Info:" << info->ExceptionRecord->ExceptionInformation << L"\n"
			<< L"Continuing execution. \n#######################";
	}
	catch(...){}

    return EXCEPTION_EXECUTE_HANDLER;
}

std::wstring make_upper_case(const std::wstring& str)
{
	return boost::to_upper_copy(str);
}

int __stdcall WinMain(HINSTANCE h_instance, HINSTANCE, LPSTR, int)
{
	static_assert(sizeof(void*) == 4, "64-bit code generation is not supported.");

	::SetUnhandledExceptionFilter(UserUnhandledExceptionFilter);

	setup_global_locale();

	int n_args;
	auto argv = ::CommandLineToArgvW(::GetCommandLineW(), &n_args);
	bool hide_on_startup = false;

	for (int i = 0; i < n_args; i++)
	{
		auto arg_upper = make_upper_case(argv[i]);
		if (arg_upper == L"-HIDE")
			hide_on_startup = true;
	}

	// Create and setup console window
	console console(hide_on_startup);

	std::wcout << L"Type \"q\" to close application." << std::endl;

	// Set debug mode.
#ifdef _DEBUG
	HANDLE hLogFile;
	hLogFile = CreateFile(L"crt_log.txt", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	std::shared_ptr<void> crt_log(nullptr, [](HANDLE h) {::CloseHandle(h); });

	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_WARN, hLogFile);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, hLogFile);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, hLogFile);
#endif

	// Increase process priotity.
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

	// Install structured exception handler.
	caspar::win32_exception::ensure_handler_installed_for_thread("main-thread");

	// Increase time precision. This will increase accuracy of function like Sleep(1) from 10 ms to 1 ms.
	struct inc_prec
	{
		inc_prec() { timeBeginPeriod(1); }
		~inc_prec() { timeEndPeriod(1); }
	} inc_prec;

	// Install unstructured exception handlers into all tbb threads.
	struct tbb_thread_installer : public tbb::task_scheduler_observer
	{
		tbb_thread_installer() { observe(true); }
		void on_scheduler_entry(bool is_worker)
		{
			caspar::win32_exception::ensure_handler_installed_for_thread("tbb-worker-thread");
		}
	} tbb_thread_installer;

	tbb::task_scheduler_init init;
	tbb::atomic<bool> wait_for_keypress;
	wait_for_keypress = false;

	try
	{
		// Configure environment properties from configuration.
		caspar::env::configure(L"casparcg.config");

		caspar::log::set_log_level(caspar::env::properties().get(L"configuration.log-level", L"debug"));

#ifdef _DEBUG
		if (caspar::env::properties().get(L"configuration.debugging.remote", false))
			MessageBox(nullptr, TEXT("Now is the time to connect for remote debugging..."), TEXT("Debug"), MB_OK | MB_TOPMOST);
#endif	 

		// Start logging to file.
		caspar::log::add_file_sink(caspar::env::log_folder());
		std::wcout << L"Logging [info] or higher severity to " << caspar::env::log_folder() << std::endl << std::endl;

		// Print environment information.
		print_info();

		std::wstringstream str;
		boost::property_tree::xml_writer_settings<wchar_t> w(' ', 3);
		boost::property_tree::write_xml(str, caspar::env::properties(), w);
		CASPAR_LOG(info) << L"casparcg.config:\n-----------------------------------------\n" << str.str().c_str() << L"-----------------------------------------";
		{
			tray_icon tray(h_instance);

			// Create server object which initializes channels, protocols and controllers.
			caspar::server caspar_server;
			// Use separate thread for the blocking console input, will be terminated 
			// anyway when the main thread terminates.
			boost::thread stdin_thread([&caspar_server, &wait_for_keypress, &tray, &console]
			{
				caspar::win32_exception::ensure_handler_installed_for_thread("stdin-thread");

				// Create a amcp parser for console commands.
				caspar::protocol::amcp::AMCPProtocolStrategy amcp(
					caspar_server.get_channels(),
					caspar_server.get_recorders(),
					caspar_server.get_thumbnail_generator(),
					caspar_server.get_media_info_repo()
				);

				// Create a dummy client which prints amcp responses to console.
				auto console_client = std::make_shared<caspar::IO::ConsoleClientInfo>();
				std::wstring wcmd;
				std::wcin.clear();
				while (true)
				{
					if (std::getline(std::wcin, wcmd) == nullptr)
					{
						if (std::wcin.bad())
							break;
						std::wcin.clear();
						continue;
					}
					//boost::to_upper(wcmd);  // TODO COMPILER crashes on this line, Strange!
					auto upper_cmd = make_upper_case(wcmd);

					if (upper_cmd == L"EXIT" || upper_cmd == L"QUIT")
					{
						wait_for_keypress = true;
						tray.close();
						break;
					}
					if (upper_cmd == L"HIDE")
					{
						console.hide();
						CASPAR_LOG(info) << L"Console was hidden using HIDE command";
						continue;
					}
#ifndef COMPILE_RELEASE
					try
					{
						// This is just dummy code for testing.
						if (wcmd.substr(0, 1) == L"1")
							wcmd = L"PLAY 1-0 udp://225.100.10.25:5500";
						else if (wcmd.substr(0, 1) == L"2")
							wcmd = L"CALL 1-0 SEEK 100";
						else if (wcmd.substr(0, 1) == L"3")
							wcmd = L"REMOVE 1 FILE RECORDING.MXF";
						else if (wcmd.substr(0, 1) == L"4")
							wcmd = L"PLAY 2-0 route://1-0";
						else if (wcmd.substr(0, 1) == L"5")
							wcmd = L"PLAY 1-0 SYNCHRO8 LOOP";
						else if (wcmd.substr(0, 1) == L"6")
							wcmd = L"CAPTURE 1 recorder 1 IN 0:31:0:0 OUT 0:31:20:0 FILE record.mp4";
						else if (wcmd.substr(0, 1) == L"7")
							wcmd = L"CAPTURE 1 recorder 1 LIMIT 250 FILE limit1.mov";
						else if (wcmd.substr(0, 1) == L"8")
							wcmd = L"CAPTURE 1 recorder 1 LIMIT 250 FILE limit2.mov";
						else if (wcmd.substr(0, 1) == L"0")
							wcmd = L"RECORDER FINISH 1";
						else if (upper_cmd.substr(0, 1) == L"X")
						{
							int num = 0;
							std::wstring file;
							try
							{
								num = boost::lexical_cast<int>(wcmd.substr(1, 2));
								file = wcmd.substr(4, wcmd.length() - 1);
							}
							catch (...)
							{
								num = boost::lexical_cast<int>(wcmd.substr(1, 1));
								file = wcmd.substr(3, wcmd.length() - 1);
							}

							int n = 0;
							int num2 = num;
							while (num2 > 0)
							{
								num2 >>= 1;
								n++;
							}

							wcmd = L"MIXER 1 GRID " + boost::lexical_cast<std::wstring>(n);

							for (int i = 1; i <= num; ++i)
								wcmd += L"\r\nPLAY 1-" + boost::lexical_cast<std::wstring>(i) + L" " + file + L" LOOP";// + L" SLIDE 100 LOOP";
						}
					}
					catch (...)
					{
						CASPAR_LOG_CURRENT_EXCEPTION();
						continue;
					}
#endif
					wcmd += L"\r\n";
					amcp.Parse(wcmd.c_str(), wcmd.length(), console_client);
				}
				tray.close();
			});
			stdin_thread.detach();
			MSG stMsg;
			while (GetMessage(&stMsg, NULL, 0, 0) > 0)
			{
				TranslateMessage(&stMsg);
				DispatchMessage(&stMsg);
			}
		} // end of server scope

		CASPAR_LOG(info) << "Successfully shutdown CasparCG Server.";
	}
	catch (boost::property_tree::file_parser_error&)
	{
		CASPAR_LOG_CURRENT_EXCEPTION();
		CASPAR_LOG(fatal) << L"Unhandled configuration error in main thread. Please check the configuration file (casparcg.config) for errors.";
		system("pause");
		return 1;
	}
	catch (...)
	{
		CASPAR_LOG_CURRENT_EXCEPTION();
		CASPAR_LOG(fatal) << L"Unhandled exception in main thread. Please report this error on GitHub (https://github.com/jaskie/Server/issues).";
		Sleep(1000);
		std::wcout << L"\n\nCasparCG will automatically shutdown. See the log file located at the configured log-path folder for more information.\n\n";
		Sleep(4000);
	}
	if (wait_for_keypress)
		system("pause");
	console.terminate();
	return 0;
}
