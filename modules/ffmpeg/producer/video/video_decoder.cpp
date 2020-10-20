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
	AVStream*								stream_;
	const int64_t							duration_;
	const size_t							width_;
	const size_t							height_;
	bool									is_progressive_;
	const int64_t							stream_start_pts_;
	tbb::atomic<int64_t>					seek_pts_;
	tbb::atomic<bool>						invert_field_order_;
	tbb::atomic<bool>						eof_;
	tbb::atomic<int64_t>					time_;
public:
	explicit implementation(input input, bool invert_field_order)
		: input_(input)
		, codec_context_(input.open_video_codec(&stream_))
		, codec_(codec_context_->codec)
		, width_(codec_context_->width)
		, height_(codec_context_->height)
		, stream_start_pts_(stream_->start_time == AV_NOPTS_VALUE ? 0 : stream_->start_time)
		, duration_(calc_duration(stream_))
	{
		invert_field_order_ = invert_field_order;
		eof_ = false;
		time_ = AV_NOPTS_VALUE;
		seek_pts_ = 0;
		CASPAR_LOG(trace) << "Codec: " << codec_->long_name;
	}

	int64_t calc_duration(const AVStream* stream)
	{
		if (stream->duration == AV_NOPTS_VALUE)
			return 0;
		else
			return  av_rescale(stream->duration, stream->time_base.num * AV_TIME_BASE, stream->time_base.den);
	}

	std::shared_ptr<AVFrame> poll()
	{
		while (!eof_)
		{
			std::shared_ptr<AVPacket> packet;
			input_.try_pop_video(packet);
			if (packet || (input_.eof() && !eof_))
				avcodec_send_packet(codec_context_.get(), packet.get());
			std::shared_ptr<AVFrame> decoded_frame = create_frame();
			int ret = avcodec_receive_frame(codec_context_.get(), decoded_frame.get());
			switch (ret)
			{
			case 0:
				is_progressive_ = !decoded_frame->interlaced_frame;

				if (invert_field_order_)
					decoded_frame->top_field_first = (!decoded_frame->top_field_first & 0x1);
				if (decoded_frame->pts == AV_NOPTS_VALUE)
					decoded_frame->pts = decoded_frame->best_effort_timestamp;
				if (decoded_frame->pts != AV_NOPTS_VALUE)
					decoded_frame->pts -= stream_start_pts_;

				if (decoded_frame->pts < seek_pts_)
					continue;

				if (decoded_frame->repeat_pict > 0)
					CASPAR_LOG(warning) << "[video_decoder] Field repeat_pict not implemented.";
				time_ = av_rescale(decoded_frame->pts * AV_TIME_BASE, stream_->time_base.num, stream_->time_base.den);
				return decoded_frame;
			case AVERROR_EOF:
				eof_ = true;
				break;
			case AVERROR(EAGAIN):
				if (input_.eof())
					eof_ = true;
				break;
			case AVERROR(EINVAL):
				BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("codec not opened"));
				return nullptr;
			}
		}
		return nullptr;
	}

	void seek(int64_t time)
	{
		avcodec_flush_buffers(codec_context_.get());
		seek_pts_ = stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_
			+ (time * stream_->time_base.den / (AV_TIME_BASE * stream_->time_base.num));
		eof_ = false;
		time_ = AV_NOPTS_VALUE;
	}

	void invert_field_order (bool invert)
	{
		invert_field_order_ = invert;
	}

	boost::rational<int> frame_rate() const
	{
		return boost::rational<int>(stream_->r_frame_rate.num, stream_->r_frame_rate.den);
	}

	boost::rational<int> time_base() const
	{
		return boost::rational<int>(stream_->time_base.num, stream_->time_base.den);
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
int64_t video_decoder::duration() const {return impl_->duration_;}
bool	video_decoder::is_progressive() const{return impl_->is_progressive_;}
std::wstring video_decoder::print() const{return impl_->print();}
void video_decoder::seek(int64_t time) { impl_->seek(time);}
void video_decoder::invert_field_order(bool invert) {impl_-> invert_field_order(invert);}
boost::rational<int> video_decoder::frame_rate() const { return impl_->frame_rate(); };
boost::rational<int> video_decoder::time_base() const { return impl_->time_base(); };
int64_t video_decoder::time() const { return impl_->time_; }
bool video_decoder::eof() const { return impl_->eof_; }
}}