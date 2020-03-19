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

#include <core/mixer/audio/audio_mixer.h>

#include <common/memory/safe_ptr.h>

#include <boost/noncopyable.hpp>

#include "..\input\input.h"

struct AVPacket;
struct AVFormatContext;

namespace caspar { 
			
namespace core {

struct video_format_desc;
struct channel_layout;

}

namespace ffmpeg {
	
class audio_decoder : boost::noncopyable
{
public:
	explicit audio_decoder(input input, caspar::core::video_format_desc format,  const std::wstring& custom_channel_order);
	std::shared_ptr<core::audio_buffer> poll();
	const core::channel_layout& channel_layout() const;
	std::wstring print() const;
	void seek(uint64_t time);
	int64_t duration() const;
	int64_t time() const;
	bool eof() const;

private:
	struct implementation;
	safe_ptr<implementation> impl_;
};

}}