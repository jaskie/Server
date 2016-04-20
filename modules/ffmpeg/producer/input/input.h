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

#include <memory>
#include <string>
#include <cstdint>

#include <boost/noncopyable.hpp>
#include <boost/thread/future.hpp>

struct AVFormatContext;
struct AVPacket;

namespace caspar {

namespace diagnostics {

class graph;

}
	 
namespace ffmpeg {

class input
{
public:
	explicit input(const safe_ptr<diagnostics::graph> graph, const std::wstring& filename, bool thumbnail_mode);
	safe_ptr<AVCodecContext> open_audio_codec(int& index);
	safe_ptr<AVCodecContext> open_video_codec(int& index);

	bool try_pop_audio(std::shared_ptr<AVPacket>& packet);
	bool try_pop_video(std::shared_ptr<AVPacket>& packet);
	bool eof() const;

	bool seek(int64_t target_time);
	safe_ptr<AVFormatContext> format_context();

private:
	struct implementation;
	std::shared_ptr<implementation> impl_;
};

	
}}
