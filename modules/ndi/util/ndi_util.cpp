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
#include <core/video_format.h>
#include <core/mixer/audio/audio_util.h>

namespace caspar { namespace ndi {

NDIlib_video_frame_t * create_weak_video_frame(core::video_format_desc format)
{
	NDIlib_video_frame_t* frame = new NDIlib_video_frame_t();
	if (frame)
	{
		frame->xres = format.width;
		frame->yres = format.height;
		frame->FourCC = NDIlib_FourCC_type_BGRA;
		frame->frame_rate_N = format.time_scale;
		frame->frame_rate_D = format.duration;
		frame->picture_aspect_ratio = static_cast<float>(format.square_width) / static_cast<float>(format.square_height);
		frame->frame_format_type = (format.field_mode == caspar::core::field_mode::progressive) ? NDIlib_frame_format_type_progressive : NDIlib_frame_format_type_interleaved;
		frame->timecode = NDIlib_send_timecode_synthesize;
		frame->p_data = (uint8_t*)malloc(format.width * format.height * 4);
		frame->line_stride_in_bytes = format.width * 4;
	}
	return frame;
}

std::shared_ptr<NDIlib_video_frame_t> create_video_frame(core::video_format_desc format)
{
	return std::shared_ptr<NDIlib_video_frame_t>(create_weak_video_frame(format), [](NDIlib_video_frame_t* f) 
	{
		if (f)
			delete(f->p_data);
		delete(f);
	});
}

NDIlib_audio_frame_t * create_weak_audio_frame(core::channel_layout layout) 
{
	return new NDIlib_audio_frame_t();		 
}

std::shared_ptr<NDIlib_audio_frame_t> create_audio_frame(core::channel_layout layout)
{
	return std::shared_ptr<NDIlib_audio_frame_t>(
		create_weak_audio_frame(layout),
		[](NDIlib_audio_frame_t* f)
		{
			delete(f);
		}
	);
	// 48000, 2, 1920, NULL, 0 
}



}}