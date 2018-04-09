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

#include "../../stdafx.h"

#include "video_decoder.h"

#include "../util/util.h"

#include "../../ffmpeg_error.h"

#include <core/producer/frame/frame_transform.h>
#include <core/producer/frame/frame_factory.h>

#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/filesystem.hpp>

#include <queue>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libavutil/imgutils.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg {
	
struct video_decoder::implementation : boost::noncopyable
{
	input 									input_;
	const safe_ptr<AVCodecContext>			codec_context_;
	const AVCodec*							codec_;
	int										stream_index_;
	const AVStream*							stream_;
	const uint32_t							nb_frames_;
	const size_t							width_;
	const size_t							height_;
	bool									is_progressive_;
	const int64_t							stream_start_pts_;
	tbb::atomic<int64_t>					seek_pts_;
	tbb::atomic<bool>						invert_field_order_;
	tbb::atomic<uint32_t>					frame_decoded_;
public:
	explicit implementation(input input, bool invert_field_order)
		: input_(input)
		, codec_context_(input.open_video_codec(stream_index_))
		, codec_(codec_context_->codec)
		, width_(codec_context_->width)
		, height_(codec_context_->height)
		, stream_(input_.format_context()->streams[stream_index_])
		, stream_start_pts_(stream_->start_time)
		, nb_frames_(static_cast<uint32_t>(calc_nb_frames(stream_)))		
	{
		invert_field_order_ = invert_field_order;
		seek_pts_ = 0;
		frame_decoded_ = 0;
		CASPAR_LOG(trace) << "Codec: " << codec_->long_name;
	}

	int64_t calc_nb_frames(const AVStream* stream)
	{
		if (stream->nb_frames > 0)
			return stream->nb_frames;
		else
			if (stream->duration == AV_NOPTS_VALUE)
				return 0;
			else
			{
				AVRational r_frame_rate = av_stream_get_r_frame_rate(stream);
				return (stream->duration * stream->time_base.num * r_frame_rate.num) / (stream->time_base.den * r_frame_rate.den);
			}				
	}

	std::shared_ptr<AVFrame> poll()
	{		
		std::shared_ptr<AVFrame> video;
		std::shared_ptr<AVPacket> packet;
		while (!video && input_.try_pop_video(packet))
			video = decode(packet);
		if (!video && !input_.eof())
			CASPAR_LOG(trace) << print() << L" Received empty frame";
		return video;
	}

	std::shared_ptr<AVFrame> decode(std::shared_ptr<AVPacket> pkt)
	{
		std::shared_ptr<AVFrame> decoded_frame = create_frame();

		int got_picture_ptr = 0;
		int bytes_consumed = avcodec_decode_video2(codec_context_.get(), decoded_frame.get(), &got_picture_ptr, pkt.get());
		
		if(got_picture_ptr == 0 || bytes_consumed < 0)	
			return nullptr;

		is_progressive_ = !decoded_frame->interlaced_frame;
		if (invert_field_order_)
			decoded_frame->top_field_first = (!decoded_frame->top_field_first & 0x1);

		if(decoded_frame->repeat_pict > 0)
			CASPAR_LOG(warning) << "[video_decoder] Field repeat_pict not implemented.";
		int64_t frame_time_stamp = av_frame_get_best_effort_timestamp(decoded_frame.get());
		if (frame_time_stamp < seek_pts_)
			return nullptr;
		frame_decoded_++;
		return decoded_frame;
	}

	void seek(uint64_t time, uint32_t frame)
	{
		avcodec_flush_buffers(codec_context_.get());
		seek_pts_ = stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_
			+ (time * stream_->time_base.den / (AV_TIME_BASE * stream_->time_base.num));
		frame_decoded_ = frame;
	}

	void invert_field_order (bool invert)
	{
		invert_field_order_ = invert;
	}

	uint32_t nb_frames()
	{
		return nb_frames_ == 0 ? frame_decoded_ : nb_frames_;
	}

	std::wstring print() const
	{		
		return L"[video-decoder] " + widen(codec_context_->codec->long_name);
	}
};

video_decoder::video_decoder(input input, bool invert_field_order) : impl_(new implementation(input, invert_field_order)){}
std::shared_ptr<AVFrame> video_decoder::poll(){return impl_->poll();}
size_t video_decoder::width() const{return impl_->width_;}
size_t video_decoder::height() const{return impl_->height_;}
uint32_t video_decoder::nb_frames() const{return impl_->nb_frames();}
bool	video_decoder::is_progressive() const{return impl_->is_progressive_;}
std::wstring video_decoder::print() const{return impl_->print();}
void video_decoder::seek(uint64_t time, uint32_t frame) { impl_->seek(time, frame);}
void video_decoder::invert_field_order(bool invert) {impl_-> invert_field_order(invert);}
}}