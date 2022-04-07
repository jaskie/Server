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

#pragma once

#include <common/memory/safe_ptr.h>

#include <core/mixer/audio/audio_mixer.h>

#include <boost/noncopyable.hpp>
#include <boost/rational.hpp>

#include <vector>

struct AVFrame;

namespace caspar { 
	
namespace core {

class write_frame;
class basic_frame;
struct frame_factory;
struct channel_layout;

}

namespace ffmpeg {

class frame_muxer : boost::noncopyable
{
public:
	frame_muxer(
			boost::rational<int> in_fps,
			boost::rational<int> in_timebase,
			const safe_ptr<core::frame_factory>& frame_factory,
			const core::channel_layout& audio_channel_layout,
			const std::string& filter = "");
	
	void push(const std::shared_ptr<AVFrame>& video_frame, int hints = 0, int frame_timecode = std::numeric_limits<unsigned int>().max());
	void push(const std::shared_ptr<core::audio_buffer>& audio_samples);
	void clear();
	void flush();

	bool video_ready() const;
	bool audio_ready() const;
	
	std::shared_ptr<core::basic_frame> poll();

private:
	struct implementation;
	safe_ptr<implementation> impl_;
};

}}