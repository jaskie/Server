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

#include "../../StdAfx.h"

#include "frame_muxer.h"

#include "../filter/filter.h"
#include "../util/util.h"

#include <core/producer/frame_producer.h>
#include <core/producer/frame/basic_frame.h>
#include <core/producer/frame/frame_transform.h>
#include <core/producer/frame/pixel_format.h>
#include <core/producer/frame/frame_factory.h>
#include <core/mixer/write_frame.h>
#include <core/mixer/audio/audio_util.h>

#include <common/env.h>
#include <common/exception/exceptions.h>
#include <common/log/log.h>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include <boost/foreach.hpp>
#include <boost/range/algorithm_ext/push_back.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>

#include <deque>
#include <queue>
#include <vector>

using namespace caspar::core;

namespace caspar { namespace ffmpeg {

struct frame_muxer::implementation : boost::noncopyable
{	
	std::queue<std::queue<safe_ptr<write_frame>>>	video_streams_;
	std::queue<core::audio_buffer>					audio_streams_;
	std::queue<safe_ptr<basic_frame>>				frame_buffer_;
	const boost::rational<int>						in_fps_;
	const boost::rational<int>						in_timebase_;
	const video_format_desc							format_desc_;
	bool											auto_transcode_;
	bool											auto_deinterlace_;
	
	std::vector<size_t>								audio_cadence_;
			
	safe_ptr<core::frame_factory>					frame_factory_;
	
	std::shared_ptr<filter>							filter_;
	const std::string								filter_str_;
	const bool										thumbnail_mode_;
	bool											force_deinterlacing_;
	const core::channel_layout						audio_channel_layout_;
		
	implementation(
			boost::rational<int> in_fps,
			boost::rational<int> in_timebase,
			const safe_ptr<core::frame_factory>& frame_factory,
			const std::string& filter_str,
			bool thumbnail_mode,
			const core::channel_layout& audio_channel_layout
			)
		: in_fps_(in_fps)
		, in_timebase_(in_timebase)
		, format_desc_(frame_factory->get_video_format_desc())
		, auto_transcode_(env::properties().get(L"configuration.auto-transcode", true))
		, auto_deinterlace_(env::properties().get(L"configuration.auto-deinterlace", true))
		, audio_cadence_(format_desc_.audio_cadence)
		, frame_factory_(frame_factory)
		, filter_str_(filter_str)
		, thumbnail_mode_(thumbnail_mode)
		, force_deinterlacing_(false)
		, audio_channel_layout_(audio_channel_layout)
	{
		video_streams_.push(std::queue<safe_ptr<write_frame>>());
		audio_streams_.push(core::audio_buffer());
		// Note: Uses 1 step rotated cadence for 1001 modes (1602, 1602, 1601, 1602, 1601)
		// This cadence fills the audio mixer most optimally.
		boost::range::rotate(audio_cadence_, std::end(audio_cadence_)-1);
	}



	void push(const std::shared_ptr<AVFrame>& video_frame, int hints, int timecode)
	{		
		if(!video_frame)
			return;
		bool need_update_filter = false;

		if (!filter_ || (video_frame->data[0] && filter_->is_frame_format_changed(video_frame)))
		{
			CASPAR_LOG(debug) << L"[frame_muxer] Frame format has changed. Resetting filter mode.";
			need_update_filter = true;
		}
				
		if (video_frame == flush_video())
		{	
			video_streams_.push(std::queue<safe_ptr<write_frame>>());
			CASPAR_LOG(trace) << "Muxer::push flush video";
		}
		else if(video_frame == empty_video())
		{
			video_streams_.back().push(make_safe<core::write_frame>(this, audio_channel_layout_));
			CASPAR_LOG(trace) << "Muxer::push empty video";
		}
		else
		{
			video_frame->display_picture_number = timecode;
			bool deinterlace_hint = (hints & core::frame_producer::DEINTERLACE_HINT) != 0;
			if(auto_deinterlace_ && force_deinterlacing_ != deinterlace_hint)
			{
				force_deinterlacing_ = deinterlace_hint;
				need_update_filter = true;
			}

			if(hints & core::frame_producer::ALPHA_HINT)
				video_frame->format = make_alpha_format(video_frame->format);
		
			if(video_frame->format == CASPAR_PIX_FMT_LUMA) // CASPAR_PIX_FMT_LUMA is not valid for filter, change it to GRAY8
				video_frame->format = AV_PIX_FMT_GRAY8;

			if(!filter_ || need_update_filter)
				update_filter(video_frame, force_deinterlacing_);

			filter_->push(video_frame);

			BOOST_FOREACH(auto& av_frame, filter_->poll_all())
			{
				if(video_frame->format == AV_PIX_FMT_GRAY8 && video_frame->format == CASPAR_PIX_FMT_LUMA)
					av_frame->format = video_frame->format;
				video_streams_.back().push(make_write_frame(this, av_frame, frame_factory_, hints, audio_channel_layout_));
			}
		}

		if(video_streams_.back().size() > 32)
			BOOST_THROW_EXCEPTION(invalid_operation() << source_info("frame_muxer") << msg_info("video-stream overflow. This can be caused by incorrect frame-rate. Check clip meta-data."));
	}

	void push(const std::shared_ptr<core::audio_buffer>& audio)
	{
		if(!audio)	
			return;

		if(audio == flush_audio())
		{
			audio_streams_.push(core::audio_buffer());
		}
		else if(audio == empty_audio())
		{
			boost::range::push_back(audio_streams_.back(), core::audio_buffer(audio_cadence_.front() * audio_channel_layout_.num_channels, 0));
		}
		else
		{
			boost::range::push_back(audio_streams_.back(), *audio);
		}

		if(audio_streams_.back().size() > 32*audio_cadence_.front() * audio_channel_layout_.num_channels)
			BOOST_THROW_EXCEPTION(invalid_operation() << source_info("frame_muxer") << msg_info("audio-stream overflow. This can be caused by incorrect frame-rate. Check clip meta-data."));
	}
	
	bool video_ready() const
	{		
		return video_streams_.size() > 1 || (video_streams_.size() >= audio_streams_.size() && video_ready2());
	}
	
	bool audio_ready() const
	{
		return audio_streams_.size() > 1 || (audio_streams_.size() >= video_streams_.size() && audio_ready2());
	}

	bool video_ready2() const
	{
		return video_streams_.front().size() >= 1;
	}
	
	bool audio_ready2() const
	{
		return audio_streams_.front().size() >= audio_cadence_.front() * audio_channel_layout_.num_channels;
	}
		
	std::shared_ptr<basic_frame> poll()
	{
		if (!frame_buffer_.empty())
		{
			auto frame = frame_buffer_.front();
			frame_buffer_.pop();
			return frame;
		}

		if (video_streams_.size() > 1 && audio_streams_.size() > 1 && (!video_ready2() || !audio_ready2()))
		{
			if (!video_streams_.front().empty() || !audio_streams_.front().empty())
				CASPAR_LOG(trace) << "Truncating: " << video_streams_.front().size() << L" video-frames, " << audio_streams_.front().size() << L" audio-samples.";

			video_streams_.pop();
			audio_streams_.pop();
		}

		if (!video_ready2() || !audio_ready2())
			return nullptr;

		auto frame1 = pop_video();
		frame1->audio_data() = pop_audio();
		frame_buffer_.push(frame1);
		return frame_buffer_.empty() ? nullptr : poll();
	}
	
	safe_ptr<core::write_frame> pop_video()
	{
		auto frame = video_streams_.front().front();
		video_streams_.front().pop();		
		return frame;
	}

	core::audio_buffer pop_audio()
	{
		CASPAR_VERIFY(audio_streams_.front().size() >= audio_cadence_.front() * audio_channel_layout_.num_channels);

		auto begin = audio_streams_.front().begin();
		auto end   = begin + (audio_cadence_.front() * audio_channel_layout_.num_channels);

		core::audio_buffer samples(begin, end);
		audio_streams_.front().erase(begin, end);
		
		boost::range::rotate(audio_cadence_, std::begin(audio_cadence_)+1);

		return samples;
	}
				
	void update_filter(const std::shared_ptr<AVFrame>& frame, bool force_deinterlace)
	{
		std::string filter_str = narrow(filter_str_);


		auto frame_mode = get_mode(*frame);
		int fixed_height = frame->height;
		if (fixed_height == 608 && frame->width == 720) // fix for IMX frames with VBI lines
		{
			filter_str = append_filter(filter_str, "crop=720:576:0:32");
			fixed_height = 576;
		}

		if (force_deinterlace)
			filter_str = append_filter(filter_str, "yadif");

		auto filtered_fps = in_fps_;

		if (filter_str_.empty())
		{
			if (format_desc_.field_mode != field_mode::progressive && frame_mode != field_mode::progressive 
				&& (format_desc_.width > static_cast<uint32_t>(frame->width) || format_desc_.height > static_cast<uint32_t>(fixed_height)))
			{
				filter_str = append_filter(filter_str, "bwdif");
				filter_str = append_filter(filter_str, (boost::format("scale=w=%1%:h=%2%") % format_desc_.width %format_desc_.height).str());
				filter_str = append_filter(filter_str, format_desc_.field_mode == field_mode::lower ? "interlace=scan=bff" : "interlace=scan=tff");
			}
			else
				if (format_desc_.width != static_cast<uint32_t>(frame->width) || format_desc_.height != static_cast<uint32_t>(fixed_height))
					filter_str = append_filter(filter_str, (boost::format("scale=w=%1%:h=%2%:interl=1") %format_desc_.width %format_desc_.height).str());

			if (format_desc_.field_mode == field_mode::progressive && frame_mode != field_mode::progressive)
			{
				filter_str = append_filter(filter_str, "bwdif");
				filtered_fps *= 2;
			}

			if (format_desc_.field_mode != field_mode::progressive && frame_mode == field_mode::progressive && filtered_fps >= boost::rational<int>(format_desc_.time_scale * 2, format_desc_.duration))
			{
				filter_str = append_filter(filter_str, format_desc_.field_mode == field_mode::lower ? "interlace=scan=bff" : "interlace=scan=tff");
				filtered_fps /= 2;
			}

			if (filtered_fps != boost::rational<int>(format_desc_.time_scale, format_desc_.duration))
				filter_str = append_filter(filter_str, (boost::format("fps=fps=%1%/%2%") % format_desc_.time_scale %format_desc_.duration).str());

		}
		auto out_pix_fmts = std::vector<AVPixelFormat>();
		out_pix_fmts.push_back(AV_PIX_FMT_BGRA);
		
		filter_.reset (new filter(
			frame->width,
			frame->height,
			av_make_q(in_timebase_.numerator(), in_timebase_.denominator()),
			av_make_q(in_fps_.numerator(), in_fps_.denominator()),
			frame->sample_aspect_ratio,
			static_cast<AVPixelFormat>(frame->format),
			out_pix_fmts,
			filter_str));

			CASPAR_LOG(debug) << L"[frame_muxer] " << print_mode(frame->width, fixed_height, in_fps_, frame->interlaced_frame > 0);
	}
	
	void clear()
	{
		while(!video_streams_.empty())
			video_streams_.pop();
		audio_streams_.back().clear();
		while (!audio_streams_.empty())
			audio_streams_.pop();	
		while(!frame_buffer_.empty())
			frame_buffer_.pop();
		if (filter_)
			filter_->clear();
		video_streams_.push(std::queue<safe_ptr<write_frame>>());
		audio_streams_.push(core::audio_buffer());
	}

	void flush()
	{
		filter_->flush();
		auto frame = filter_->last_input_frame();
		BOOST_FOREACH(auto & av_frame, filter_->poll_all())
		{
			if (frame->format == AV_PIX_FMT_GRAY8 && frame->format == CASPAR_PIX_FMT_LUMA)
				av_frame->format = frame->format;
			video_streams_.back().push(make_write_frame(this, av_frame, frame_factory_, 0, audio_channel_layout_));
		}
		push(empty_audio());
	}
};

frame_muxer::frame_muxer(
		boost::rational<int> in_fps,
		boost::rational<int> in_timebase,
		const safe_ptr<core::frame_factory>& frame_factory,
		bool thumbnail_mode,
		const core::channel_layout& audio_channel_layout,
		const std::string& filter)
	: impl_(new implementation(in_fps, in_timebase, frame_factory, filter, thumbnail_mode, audio_channel_layout)){}
void frame_muxer::push(const std::shared_ptr<AVFrame>& video_frame, int hints, int frame_timecode){impl_->push(video_frame, hints, frame_timecode);}
void frame_muxer::push(const std::shared_ptr<core::audio_buffer>& audio_samples){return impl_->push(audio_samples);}
void frame_muxer::flush() { impl_->flush(); }
void frame_muxer::clear(){return impl_->clear();}
std::shared_ptr<basic_frame> frame_muxer::poll(){return impl_->poll();}
bool frame_muxer::video_ready() const{return impl_->video_ready();}
bool frame_muxer::audio_ready() const{return impl_->audio_ready();}

}}