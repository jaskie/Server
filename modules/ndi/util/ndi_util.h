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


#pragma once

#include <Processing.NDI.Lib.h>
#include <common/exception/exceptions.h>

namespace caspar {

namespace core {
	struct video_format_desc;
	struct channel_layout;
}

namespace ndi {
	NDIlib_video_frame_t* create_video_frame(const core::video_format_desc& format, const bool is_alpha);
	std::shared_ptr<NDIlib_audio_frame_interleaved_32f_t> create_audio_frame(const core::channel_layout& layout, const int nb_samples, const int sample_rate);
	NDIlib_v2* load_ndi();

} }