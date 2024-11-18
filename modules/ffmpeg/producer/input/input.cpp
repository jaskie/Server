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

#include "input.h"

#include "../util/util.h"
#include "../util/flv.h"
#include "../../ffmpeg_error.h"
#include "../../ffmpeg.h"
#include "../../tbb_avcodec.h"

#include <core/video_format.h>

#include <common/diagnostics/graph.h>
#include <common/concurrency/executor.h>
#include <common/concurrency/future_util.h>
#include <common/exception/exceptions.h>
#include <common/exception/win32_exception.h>

#include <tbb/concurrent_queue.h>
#include <tbb/atomic.h>
#include <tbb/recursive_mutex.h>

#include <boost/rational.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

static const size_t MAX_BUFFER_COUNT    = 500;
static const size_t MIN_BUFFER_COUNT    = 50;

namespace caspar { namespace ffmpeg {
		
struct input::implementation : boost::noncopyable
{		
	const safe_ptr<diagnostics::graph>							graph_;

	const safe_ptr<AVFormatContext>								format_context_; // Destroy this last
			
	const std::wstring											filename_;
	tbb::atomic<bool>											is_eof_;
	tbb::atomic<int>											video_stream_index_;
	tbb::atomic<int>											audio_stream_index_;
	tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>>	audio_buffer_;
	tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>>	video_buffer_;
	executor													executor_;


	
	explicit implementation(const safe_ptr<diagnostics::graph> graph, 
		const std::wstring& filename
		)
		: graph_(graph)
		, filename_(filename)
		, format_context_(open_input(filename))
		, executor_(print())
	{
		is_eof_			= false;
		video_buffer_.set_capacity(MAX_BUFFER_COUNT);
		audio_buffer_.set_capacity(MAX_BUFFER_COUNT);
		video_stream_index_ = -1;
		audio_stream_index_ = -1;
		graph_->set_color("audio-buffer-count", diagnostics::color(0.7f, 0.4f, 0.4f));
		graph_->set_color("video-buffer-count", diagnostics::color(1.0f, 1.0f, 0.0f));
	}

	safe_ptr<AVCodecContext> open_audio_codec(AVStream** stream)
	{
		const AVCodec* decoder;
		int index = THROW_ON_ERROR2(av_find_best_stream(format_context_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0), print());
		if (!decoder)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(narrow(print()) + " Audio decoder not found."));
		AVCodecContext *ctx = avcodec_alloc_context3(decoder);
		if (!ctx)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(narrow(print()) + " Audio codec context not created."));
		avcodec_parameters_to_context(ctx, format_context_->streams[index]->codecpar);
		THROW_ON_ERROR2(avcodec_open2(ctx, decoder, NULL), print());
		audio_stream_index_ = index;
		*stream = format_context_->streams[index];
		return safe_ptr<AVCodecContext>(ctx, [](AVCodecContext* c) { avcodec_free_context(&c); });
	}
	
	safe_ptr<AVCodecContext> open_video_codec(AVStream** stream)
	{
		const AVCodec *decoder;
		int index = THROW_ON_ERROR2(av_find_best_stream(format_context_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0), print());
		if (!decoder)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(narrow(print()) + " Video decoder not found."));
		AVCodecContext* ctx = avcodec_alloc_context3(decoder);
		if (!ctx)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(narrow(print()) + " Video codec context not created."));
		avcodec_parameters_to_context(ctx, format_context_->streams[index]->codecpar);
		if (tbb_avcodec_open(ctx, decoder, NULL) < 0)
		{
			CASPAR_LOG(debug) << print() << L" Multithreaded avcodec_open2 failed";
			THROW_ON_ERROR2(avcodec_open2(ctx, decoder, NULL), print());
		}
		video_stream_index_ = index;
		*stream = format_context_->streams[index];
		return safe_ptr<AVCodecContext>(ctx, [](AVCodecContext* c) { avcodec_free_context(&c); });
	}

	void try_pop_audio(std::shared_ptr<AVPacket>& packet)
	{	
		bool result = false;
		for (int i = 0; i < 32 && !result; ++i)
		{
			result = audio_buffer_.try_pop(packet);
			if (!result)
				if (is_eof_)
					return;
				else
				{
					boost::this_thread::sleep(boost::posix_time::milliseconds(10));
					result = audio_buffer_.try_pop(packet);
				}
		}
		if(result)
			tick();
		graph_->set_value("audio-buffer-count", (static_cast<double>(audio_buffer_.size())+0.001)/MAX_BUFFER_COUNT);
	}

	void try_pop_video(std::shared_ptr<AVPacket>& packet)
	{
		bool result = false;
		for (int i = 0; i < 32 && !result; ++i)
		{
			result = video_buffer_.try_pop(packet);
			if (!result)
				if (is_eof_)
					return;
				else
				{
					boost::this_thread::sleep(boost::posix_time::milliseconds(10));
					result = video_buffer_.try_pop(packet);
				}
		}
		if(result)
			tick();
		graph_->set_value("video-buffer-count", (static_cast<double>(video_buffer_.size()) + 0.001)/MAX_BUFFER_COUNT);
	}

	std::wstring print() const
	{
		return L"ffmpeg_input[" + filename_ + L")]";
	}
	
	bool full() const
	{
		return (audio_stream_index_ == -1 || audio_buffer_.size() > MIN_BUFFER_COUNT)
			&& (video_stream_index_ == -1 || video_buffer_.size() > MIN_BUFFER_COUNT);
	}

	bool is_eof() const
	{
		return is_eof_;
	}

	void tick()
	{	
		if(is_eof_)
			return;
		executor_.begin_invoke([this]
		{			
			while (!is_eof_ && !full())
			{
				try
				{
					auto packet = create_packet();
					auto ret = av_read_frame(format_context_.get(), packet.get()); 
					is_eof_ = ret == AVERROR(EIO) || ret == AVERROR_EOF;
					if (is_eof_)
						CASPAR_LOG(trace) << print() << " Reached EOF.";
					else
					{
						THROW_ON_ERROR(ret, "av_read_frame", print());
						if (packet->stream_index == video_stream_index_ && packet->size > 0)
						{
							if (!video_buffer_.try_push(packet))
							{
								video_buffer_.clear();
								video_buffer_.push(packet);
								CASPAR_LOG(warning) << print() << " Video packet queue cleared due to overflow.";
							}
							graph_->set_value("video-buffer-count", (static_cast<double>(video_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
						}
						if (packet->stream_index == audio_stream_index_ && packet->size > 0)
						{
							if (!audio_buffer_.try_push(packet))
							{
								audio_buffer_.clear();
								audio_buffer_.push(packet);
								CASPAR_LOG(warning) << print() << " Audio packet queue cleared due to overflow.";
							}
							graph_->set_value("audio-buffer-count", (static_cast<double>(audio_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
						}
					}
				}
				catch (...)
				{
					CASPAR_LOG_CURRENT_EXCEPTION();
				}
			}
		});
	}	

	safe_ptr<AVFormatContext> open_input(const std::wstring resource_name)
	{
		AVFormatContext* weak_context = nullptr;
		THROW_ON_ERROR2(avformat_open_input(&weak_context, narrow(resource_name).c_str(), nullptr, nullptr), resource_name);
		safe_ptr<AVFormatContext> context(weak_context, [](AVFormatContext* ctx){avformat_close_input(&ctx);});      
		THROW_ON_ERROR2(avformat_find_stream_info(weak_context, nullptr), resource_name);
		return context;
	}

	bool seek(int64_t target_time)
	{
		return executor_.invoke([this, target_time]() -> bool
		{
			audio_buffer_.clear();
			video_buffer_.clear();
			LOG_ON_ERROR2(avformat_flush(format_context_.get()), "FFMpeg input avformat_flush");
			graph_->set_value("audio-buffer-count", (static_cast<double>(audio_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
			graph_->set_value("video-buffer-count", (static_cast<double>(video_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
			CASPAR_LOG(trace) << print() << " Seeking: " << target_time / 1000 << " ms";
			is_eof_ = false;
			int ret = av_seek_frame(format_context_.get(), -1, target_time - AV_TIME_BASE, AVSEEK_FLAG_BACKWARD);
			if (ret < 0)
				CASPAR_LOG(error) << print() << " Seek failed";
			tick();
			return ret >= 0;
		}, high_priority);
	}
};

input::input(const safe_ptr<diagnostics::graph> graph, const std::wstring& filename)
	: impl_(new implementation(graph, filename)){}
bool input::eof() const { return impl_->is_eof(); }
void input::try_pop_audio(std::shared_ptr<AVPacket>& packet) { impl_->try_pop_audio(packet); }
void input::try_pop_video(std::shared_ptr<AVPacket>& packet) { impl_->try_pop_video(packet); }
safe_ptr<AVFormatContext> input::format_context(){return impl_->format_context_;}
bool input::seek(int64_t target_time) { return impl_->seek(target_time); }
void input::tick() { impl_->tick(); }
safe_ptr<AVCodecContext> input::open_audio_codec(AVStream** stream) { return impl_->open_audio_codec(stream);}
safe_ptr<AVCodecContext> input::open_video_codec(AVStream** stream) { return impl_->open_video_codec(stream); }

}}
