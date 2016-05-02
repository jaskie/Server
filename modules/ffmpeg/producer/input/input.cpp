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

static const size_t MAX_BUFFER_COUNT    = 100;
static const size_t MAX_BUFFER_COUNT_RT = 3;
static const size_t MIN_BUFFER_COUNT    = 50;
static const int32_t FLUSH_AV_PACKET_COUNT = 0x50;


namespace caspar { namespace ffmpeg {
		
struct input::implementation : boost::noncopyable
{		
	const safe_ptr<diagnostics::graph>							graph_;

	const safe_ptr<AVFormatContext>								format_context_; // Destroy this last
			
	const std::wstring											filename_;
	const bool													thumbnail_mode_;
	tbb::atomic<bool>											is_eof_;
	tbb::atomic<int32_t>										flush_av_packet_count_;
	tbb::atomic<int>											video_stream_index_;
	tbb::atomic<int>											audio_stream_index_;
	tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>>	audio_buffer_;
	tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>>	video_buffer_;
	executor													executor_;


	
	explicit implementation(const safe_ptr<diagnostics::graph> graph, 
		const std::wstring& filename, 
		bool thumbnail_mode
		)
		: graph_(graph)
		, filename_(filename)
		, format_context_(open_input(filename))
		, thumbnail_mode_(thumbnail_mode)
		, executor_(print())
	{
		if (thumbnail_mode_)
			executor_.invoke([]
			{
				disable_logging_for_thread();
			});
		is_eof_			= false;
		video_stream_index_ = -1;
		audio_stream_index_ = -1;
		graph_->set_color("audio-buffer-count", diagnostics::color(0.7f, 0.4f, 0.4f));
		graph_->set_color("video-buffer-count", diagnostics::color(1.0f, 1.0f, 0.0f));	
	}

	safe_ptr<AVCodecContext> open_audio_codec(int& index)
	{
		auto ret = open_codec(format_context_, AVMEDIA_TYPE_AUDIO, index);
		audio_stream_index_ = index;
		return ret;
	}
	
	safe_ptr<AVCodecContext> open_video_codec(int& index)
	{
		auto ret = open_codec(format_context_, AVMEDIA_TYPE_VIDEO, index);
		video_stream_index_ = index;
		return ret;
	}

	std::shared_ptr<AVPacket> get_flush_av_packet()
	{
		if (flush_av_packet_count_-- >= 0 && is_eof_)
		{
			std::shared_ptr<AVPacket> packet = create_packet();
			packet->data = nullptr;
			packet->size = 0;
			return packet;
		}
		return nullptr;
	}
	
	bool try_pop_audio(std::shared_ptr<AVPacket>& packet)
	{
		auto result = audio_buffer_.try_pop(packet);
		if(result)
			tick();
		else
			result = packet = get_flush_av_packet();
		graph_->set_value("audio-buffer-count", (static_cast<double>(audio_buffer_.size())+0.001)/MAX_BUFFER_COUNT);
		return result;
	}

	bool try_pop_video(std::shared_ptr<AVPacket>& packet)
	{
		auto result = video_buffer_.try_pop(packet);
		if (result)
			tick();
		else
			result = packet = get_flush_av_packet();
		graph_->set_value("video-buffer-count", (static_cast<double>(video_buffer_.size()) + 0.001)/MAX_BUFFER_COUNT);
		return result;
	}


	std::ptrdiff_t get_max_buffer_count() const
	{
		return thumbnail_mode_ ? 1 : MAX_BUFFER_COUNT;
	}

	std::ptrdiff_t get_min_buffer_count() const
	{
		return thumbnail_mode_ ? 0 : MIN_BUFFER_COUNT;
	}

	std::wstring print() const
	{
		return L"ffmpeg_input[" + filename_ + L")]";
	}
	
	bool full() const
	{
		return audio_buffer_.size() > get_min_buffer_count()
			&& video_buffer_.size() > get_min_buffer_count();
	}

	bool is_eof() const
	{
		return is_eof_ && flush_av_packet_count_ <= 0;
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
						;// CASPAR_LOG(trace) << print() << " Reached EOF.";
					else
					{
						THROW_ON_ERROR(ret, "av_read_frame", print());
						if (packet->stream_index == video_stream_index_)
						{
							THROW_ON_ERROR2(av_dup_packet(packet.get()), print());
							video_buffer_.try_push(packet);
							graph_->set_value("video-buffer-count", (static_cast<double>(video_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
						}
						if (packet->stream_index == audio_stream_index_)
						{
							THROW_ON_ERROR2(av_dup_packet(packet.get()), print());
							audio_buffer_.try_push(packet);
							graph_->set_value("audio-buffer-count", (static_cast<double>(audio_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
						}
					}
				}
				catch (...)
				{
					if (!thumbnail_mode_)
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
		return executor_.begin_invoke([=]() -> bool
		{
			std::shared_ptr<AVPacket> packet;
			audio_buffer_.clear();
			video_buffer_.clear();
			graph_->set_value("audio-buffer-count", (static_cast<double>(audio_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
			graph_->set_value("video-buffer-count", (static_cast<double>(video_buffer_.size()) + 0.001) / MAX_BUFFER_COUNT);
			queued_seek(target_time);
			return true;
		}, high_priority).get();
	}

	void queued_seek(int64_t target)
	{  	
		/*if (!thumbnail_mode_)
			CASPAR_LOG(debug) << print() << " Seeking: " << target / 1000;*/
		flush_av_packet_count_ = FLUSH_AV_PACKET_COUNT;
		is_eof_ = false;
		THROW_ON_ERROR2(av_seek_frame(format_context_.get(), -1, target, AVSEEK_FLAG_BACKWARD), "[input]");
		avformat_flush(format_context_.get());
		tick();
	}	
	
};

input::input(const safe_ptr<diagnostics::graph> graph, const std::wstring& filename, bool thumbnail_mode)
	: impl_(new implementation(graph, filename, thumbnail_mode)){}
bool input::eof() const {return impl_->is_eof();}
bool input::try_pop_audio(std::shared_ptr<AVPacket>& packet){return impl_->try_pop_audio(packet);}
bool input::try_pop_video(std::shared_ptr<AVPacket>& packet) { return impl_->try_pop_video(packet); }
safe_ptr<AVFormatContext> input::format_context(){return impl_->format_context_;}
bool input::seek(int64_t target_time){return impl_->seek(target_time);}
safe_ptr<AVCodecContext> input::open_audio_codec(int& index) { return impl_->open_audio_codec(index);}
safe_ptr<AVCodecContext> input::open_video_codec(int& index) { return impl_->open_video_codec(index); }

}}
