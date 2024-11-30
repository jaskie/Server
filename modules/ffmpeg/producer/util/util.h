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

#include <core/video_format.h>
#include <core/producer/frame/pixel_format.h>
#include <core/mixer/audio/audio_mixer.h>

#include <boost/rational.hpp>
#include <boost/optional.hpp>

#include <tuple>

#include <cstdint>

enum PixelFormat;
struct AVFrame;
struct AVFormatContext;
struct AVPacket;
struct AVRational;
struct AVCodecContext;

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

namespace caspar {

namespace core {

struct pixel_format_desc;
class write_frame;
struct frame_factory;
struct channel_layout;
}

namespace ffmpeg {

std::shared_ptr<core::audio_buffer> flush_audio();
std::shared_ptr<core::audio_buffer> empty_audio();
std::shared_ptr<AVFrame>			flush_video();
std::shared_ptr<AVFrame>			empty_video();

// Utils

static const int CASPAR_PIX_FMT_LUMA = 10; // Just hijack some unual pixel format.

core::field_mode::type		get_mode(const AVFrame& frame);
int							make_alpha_format(int format); // NOTE: Be careful about CASPAR_PIX_FMT_LUMA, change it to PIX_FMT_GRAY8 if you want to use the frame inside some ffmpeg function.
safe_ptr<core::write_frame> make_write_frame(const void* tag, const safe_ptr<AVFrame>& decoded_frame, const safe_ptr<core::frame_factory>& frame_factory, int hints, const core::channel_layout& audio_channel_layout);

safe_ptr<AVPacket> create_packet();
std::shared_ptr<AVFrame> create_frame();

std::wstring print_mode(size_t width, size_t height, boost::rational<int> fps, bool interlaced);

std::wstring probe_stem(const std::wstring stem, const std::vector<std::wstring>& invalid_exts);
std::wstring probe_stem(const std::wstring stem);
bool is_valid_file(const std::wstring filename, const std::vector<std::wstring>& invalid_exts);
bool is_valid_file(const std::wstring filename);
bool try_get_duration(const std::wstring filename, std::int64_t& duration, boost::rational<std::int64_t>& time_base);
int64_t ffmpeg_time_from_frame_number(int32_t frame_number, int fps_num, int fps_den);
int64_t frame_number_from_ffmpeg_time(int64_t time, int fps_num, int fps_den);
std::vector<int> parse_list(const std::string& list);

core::channel_layout get_audio_channel_layout(const AVCodecContext& context, const std::wstring& custom_channel_order);

struct frame_time {
	frame_time(int frame_number)
		: FrameNumber(frame_number)
	{ }
	const int FrameNumber;
};

void av_buffer_free(void* opaque, uint8_t* data);

}}