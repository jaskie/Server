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

#include "StdAfx.h"

#include "consumer/ffmpeg_consumer.h"
#include "producer/ffmpeg_producer.h"
#include "producer/util/util.h"

#include <common/log/log.h>
#include <common/exception/win32_exception.h>

#include <core/parameters/parameters.h>
#include <core/consumer/frame_consumer.h>
#include <core/producer/frame_producer.h>
#include <core/producer/media_info/media_info.h>
#include <core/producer/media_info/media_info_repository.h>

#include <tbb/recursive_mutex.h>

#include <boost/thread.hpp>

#if defined(_MSC_VER)
#pragma warning (disable : 4244)
#pragma warning (disable : 4603)
#pragma warning (disable : 4996)
#endif

extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/avutil.h>
	#include <libavfilter/avfilter.h>
	#include <libavdevice/avdevice.h>
}

namespace caspar { namespace ffmpeg {

static void sanitize(uint8_t *line)
{
    while(*line)
	{
        if(*line < 0x08 || (*line > 0x0D && *line < 0x20))
            *line='?';
        line++;
    }
}

void log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    static int print_prefix=1;
    static int count;
    static char prev[1024];
    char line[8192];
    static int is_atty;
    AVClass* avc= ptr ? *(AVClass**)ptr : NULL;
    if(level > av_log_get_level())
        return;
    line[0]=0;
	
#undef fprintf
    if(print_prefix && avc) 
	{
        if (avc->parent_log_context_offset) 
		{
            AVClass** parent= *(AVClass***)(((uint8_t*)ptr) + avc->parent_log_context_offset);
            if(parent && *parent)
                std::sprintf(line, "[%s @ %p] ", (*parent)->item_name(parent), parent);            
        }
        std::sprintf(line + strlen(line), "[%s @ %p] ", avc->item_name(ptr), ptr);
    }

    std::vsprintf(line + strlen(line), fmt, vl);

    print_prefix = strlen(line) && line[strlen(line)-1] == '\n';
	
    strcpy(prev, line);
    sanitize((uint8_t*)line);

	int len = strlen(line);
	if(len > 0)
		line[len-1] = 0;
	
	if(level == AV_LOG_DEBUG)
		CASPAR_LOG(debug) << L"[ffmpeg] " << line;
	else if(level == AV_LOG_INFO)
		CASPAR_LOG(info) << L"[ffmpeg] " << line;
	else if(level == AV_LOG_WARNING)
		CASPAR_LOG(warning) << L"[ffmpeg] " << line;
	else if(level == AV_LOG_ERROR)
		CASPAR_LOG(error) << L"[ffmpeg] " << line;
	else if(level == AV_LOG_FATAL)
		CASPAR_LOG(fatal) << L"[ffmpeg] " << line;
	else
		CASPAR_LOG(trace) << L"[ffmpeg] " << line;

}

boost::thread_specific_ptr<bool>& get_disable_logging_for_thread()
{
	static boost::thread_specific_ptr<bool> disable_logging_for_thread;

	return disable_logging_for_thread;
}

void disable_logging_for_thread()
{
	if (get_disable_logging_for_thread().get() == nullptr)
		get_disable_logging_for_thread().reset(new bool); // bool value does not matter
}

bool is_logging_already_disabled_for_thread()
{
	return get_disable_logging_for_thread().get() != nullptr;
}

std::shared_ptr<void> temporary_disable_logging_for_thread(bool disable)
{
	if (!disable || is_logging_already_disabled_for_thread())
		return std::shared_ptr<void>();

	disable_logging_for_thread();

	return std::shared_ptr<void>(nullptr, [] (void*)
	{
		get_disable_logging_for_thread().release(); // Only works correctly if destructed in same thread as original caller.
	});
}

void log_for_thread(void* ptr, int level, const char* fmt, va_list vl)
{
	win32_exception::ensure_handler_installed_for_thread("ffmpeg-thread");
	log_callback(ptr, level, fmt, vl);
}


void init(const safe_ptr<core::media_info_repository>& media_info_repo)
{
	av_log_set_callback(log_for_thread);
	avformat_network_init();
	
	core::register_consumer_factory([](const core::parameters& params){return ffmpeg::create_consumer(params);});
	core::register_producer_factory(create_producer);

	media_info_repo->register_extractor(
			[](const std::wstring& file, core::media_info& info) -> bool
			{
				auto disable_logging = temporary_disable_logging_for_thread(true);

				return is_valid_file(file) && try_get_duration(file, info.duration, info.time_base);
			});
}

void uninit()
{
	avformat_network_deinit();
}

std::wstring make_version(unsigned int ver)
{
	std::wstringstream str;
	str << ((ver >> 16) & 0xFF) << L"." << ((ver >> 8) & 0xFF) << L"." << ((ver >> 0) & 0xFF);
	return str.str();
}

std::wstring get_avcodec_version()
{
	return make_version(avcodec_version());
}

std::wstring get_avformat_version()
{
	return make_version(avformat_version());
}

std::wstring get_avutil_version()
{
	return make_version(avutil_version());
}

std::wstring get_avfilter_version()
{
	return make_version(avfilter_version());
}

std::wstring get_swscale_version()
{
	return make_version(swscale_version());
}

}}