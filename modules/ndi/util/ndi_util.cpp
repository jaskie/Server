/*
* Copyright 2017 Telewizja Polska
*
* This file is part of TVP's CasparCG fork.
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
* Author: Jerzy Jaœkiewicz, jurek@tvp.pl based on Robert Nagy, ronag89@gmail.com work
*/


#include "ndi_util.h"
#include <common/utility/string.h>
#include <common/log/log.h>
#include <core/video_format.h>
#include <core/mixer/audio/audio_util.h>
#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C"
{
#include <libavutil/imgutils.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif
#include <windows.h>

namespace caspar { namespace ndi {

NDIlib_video_frame_t * create_video_frame(const core::video_format_desc& format, const bool is_alpha)
{
	NDIlib_video_frame_t* frame = new NDIlib_video_frame_t();
	if (frame)
	{
		frame->xres = format.width;
		frame->yres = format.height;
		frame->FourCC = is_alpha ? NDIlib_FourCC_type_BGRA : NDIlib_FourCC_type_UYVY;
		frame->frame_rate_N = format.time_scale;
		frame->frame_rate_D = format.duration;
		frame->picture_aspect_ratio = static_cast<float>(format.square_width) / static_cast<float>(format.square_height);
		frame->frame_format_type = (format.field_mode == caspar::core::field_mode::progressive) ? NDIlib_frame_format_type_progressive : NDIlib_frame_format_type_interleaved;
		frame->timecode = NDIlib_send_timecode_synthesize;
		frame->p_data = nullptr;
		frame->line_stride_in_bytes = format.width * (is_alpha ? 4 : 2);
	}
	return frame;
}

NDIlib_audio_frame_interleaved_32f_t * create_weak_audio_frame(const core::channel_layout& layout, const int nb_samples, const int sample_rate)
{
	auto f = new NDIlib_audio_frame_interleaved_32f_t();
	if (f)
	{
		f->no_channels = layout.num_channels;
		f->no_samples = nb_samples;
		f->sample_rate = sample_rate;
		f->p_data = (float*)malloc(nb_samples * layout.num_channels * sizeof(float));
		f->timecode = NDIlib_send_timecode_synthesize;
	}
	return f;
}

std::shared_ptr<NDIlib_audio_frame_interleaved_32f_t> create_audio_frame(const core::channel_layout& layout, const int nb_samples, const int sample_rate)
{
	return std::shared_ptr<NDIlib_audio_frame_interleaved_32f_t>(
		create_weak_audio_frame(layout, nb_samples, sample_rate),
		[](NDIlib_audio_frame_interleaved_32f_t* f)
	{
		if (f->p_data)
			delete(f->p_data);
		delete(f);
	}
	);
}


NDIlib_v2* load_ndi()
{
#ifdef	_WIN64
	std::string ndi_lib("Processing.NDI.Lib.x64.dll");
#else	
	std::string ndi_lib("Processing.NDI.Lib.x86.dll");
#endif
	HMODULE h_lib = nullptr;
	h_lib = ::LoadLibraryA(ndi_lib.c_str());
	if (!h_lib)
	{
		char* env_path = ::getenv("NDI_RUNTIME_DIR_V2");
		if (env_path)
		{
			std::string ndi_runtime_v2(env_path);
			ndi_lib = ndi_runtime_v2 + '\\' + ndi_lib;
			h_lib = ::LoadLibraryA(ndi_lib.c_str());
		}
	}
	NDIlib_v2* (*ndi_lib_load)(void) = NULL;
	if (h_lib)
		*((FARPROC*)&ndi_lib_load) = ::GetProcAddress(h_lib, "NDIlib_v2_load");
	if (!ndi_lib_load)
	{	// Cannot run NDI. Most likely because the CPU is not sufficient (see SDK documentation).
		// you can check this directly with a call to NDIlib_is_supported_CPU()
		if (h_lib)
			::FreeLibrary(h_lib);
		CASPAR_LOG(info) << L"Newtek NDI runtime not found.";
		return nullptr;
	}
	return ndi_lib_load();
}



}}