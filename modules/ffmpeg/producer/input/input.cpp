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
#include "../../ffmpeg_params.h"
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
static const size_t MAX_BUFFER_SIZE     = 64 * 1000000;
static const int32_t FLUSH_PACKET_COUNT = 0x20;


namespace caspar { namespace ffmpeg {
		
struct input::implementation : boost::noncopyable
{		
	const safe_ptr<diagnostics::graph>							graph_;

	const safe_ptr<AVFormatContext>								format_context_; // Destroy this last
	const int													default_stream_index_;
	const AVStream*												default_stream_;
			
	const std::wstring											filename_;
	uint32_t													start_;		
	const uint32_t												length_;
	const bool													thumbnail_mode_;
	tbb::atomic<bool>											loop_;
	tbb::atomic<bool>											is_eof_;
	uint32_t													frame_number_;
	const int64_t												stream_start_time;
	int64_t														start_time_;
	int64_t														end_time_;
	const double												channel_fps_;
	
	tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>>	buffer_;
	tbb::atomic<size_t>											buffer_size_;
	int32_t														flush_packet_count_;
	executor													executor_;

	
	explicit implementation(const safe_ptr<diagnostics::graph> graph, const std::wstring& filename, FFMPEG_Resource resource_type, bool loop, uint32_t start, uint32_t length, bool thumbnail_mode, const ffmpeg_producer_params& vid_params, double fps)
		: graph_(graph)
		, format_context_(open_input(filename, resource_type, vid_params))		
		, default_stream_index_(av_find_default_stream_index(format_context_.get()))
		, default_stream_(format_context_->streams[default_stream_index_])
		, filename_(filename)
		, length_(length)
		, thumbnail_mode_(thumbnail_mode)
		, executor_(print())
		, end_time_(std::numeric_limits<int64_t>().max())
		, channel_fps_(fps)
		, stream_start_time(default_stream_->start_time)
	{
		if (thumbnail_mode_)
			executor_.invoke([]
			{
				disable_logging_for_thread();
			});

		is_eof_			= false;
		loop_			= loop;
		buffer_size_	= 0;
		graph_->set_color("seek", diagnostics::color(1.0f, 0.5f, 0.0f));	
		graph_->set_color("buffer-count", diagnostics::color(0.7f, 0.4f, 0.4f));
		graph_->set_color("buffer-size", diagnostics::color(1.0f, 1.0f, 0.0f));	
	}

	bool get_flush_paket(std::shared_ptr<AVPacket> &pkt)
	{
		if (flush_packet_count_-- >= 0 && is_eof_)
		{
			std::shared_ptr<AVPacket> flush_packet((AVPacket*)av_malloc(sizeof(AVPacket)), av_free_packet);
			av_init_packet(flush_packet.get());
			pkt = flush_packet;
			pkt->data = nullptr;
			pkt->size = 0;
			return true;
		}
		return false;
	}
	
	bool try_pop(std::shared_ptr<AVPacket>& packet)
	{
		auto result = buffer_.try_pop(packet);
		if(result)
		{
			if(packet)
				buffer_size_ -= packet->size;
			tick();
		}
		if (!result)
			result = get_flush_paket(packet);
		graph_->set_value("buffer-size", (static_cast<double>(buffer_size_)+0.001)/MAX_BUFFER_SIZE);
		graph_->set_value("buffer-count", (static_cast<double>(buffer_.size()+0.001)/MAX_BUFFER_COUNT));
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

	bool seek(uint32_t target)
	{
		/*if (!executor_.is_running())
			return wrap_as_future(false);
*/
		return executor_.begin_invoke([=]() -> bool
		{
			std::shared_ptr<AVPacket> packet;
			while(buffer_.try_pop(packet) && packet)
				buffer_size_ -= packet->size;

			queued_seek(target);

			return true;
		}, high_priority).get();
	}
	
	std::wstring print() const
	{
		return L"ffmpeg_input[" + filename_ + L")]";
	}
	
	bool full() const
	{
		return (buffer_size_ > MAX_BUFFER_SIZE || buffer_.size() > get_max_buffer_count()) && buffer_.size() > get_min_buffer_count();
	}

	void tick()
	{	
		if(is_eof_)
			return;
		
		executor_.begin_invoke([this]
		{			
			if(full())
				return;

			try
			{
				auto packet = create_packet();
		
				auto ret = av_read_frame(format_context_.get(), packet.get()); // packet is only valid until next call of av_read_frame. Use av_dup_packet to extend its life.	
		
				AVStream * packet_stream = format_context_->streams[packet->stream_index];
				int64_t packet_time = (packet.get() && packet->pts != AV_NOPTS_VALUE && packet_stream->time_base.den>0)  
					? ((packet->pts - (stream_start_time == AV_NOPTS_VALUE ? 0 : stream_start_time)) * AV_TIME_BASE * packet_stream->time_base.num)/packet_stream->time_base.den 
					: std::numeric_limits<int64_t>().max();
				is_eof_ = ret == AVERROR(EIO) || ret == AVERROR_EOF;
				if (is_eof_)
					CASPAR_LOG(trace) << print() << " Reached EOF.";			

				if(is_eof_ || (packet->stream_index == default_stream_index_ && packet_time > end_time_))
				{
					frame_number_ = start_;
					if(loop_)
					{
						queued_seek(start_);
						graph_->set_tag("seek");		
						CASPAR_LOG(trace) << print() << " Looping.";			
					}		
					//else
						//executor_.stop();
				}
				else
				{		
					THROW_ON_ERROR(ret, "av_read_frame", print());

					if(packet->stream_index == default_stream_index_ && packet_time >= start_time_)
						++frame_number_;

					THROW_ON_ERROR2(av_dup_packet(packet.get()), print());
				
					// Make sure that the packet is correctly deallocated even if size and data is modified during decoding.
					auto size = packet->size;
					auto data = packet->data;
			
					packet = safe_ptr<AVPacket>(packet.get(), [packet, size, data](AVPacket*)
					{
						packet->size = size;
						packet->data = data;				
					});

					buffer_.try_push(packet);
					buffer_size_ += packet->size;
				
					graph_->set_value("buffer-size", (static_cast<double>(buffer_size_)+0.001)/MAX_BUFFER_SIZE);
					graph_->set_value("buffer-count", (static_cast<double>(buffer_.size()+0.001)/MAX_BUFFER_COUNT));
				}	
		
				tick();		
			}
			catch(...)
			{
				if (!thumbnail_mode_)
					CASPAR_LOG_CURRENT_EXCEPTION();
				//executor_.stop();
			}
		});
	}	

	safe_ptr<AVFormatContext> open_input(const std::wstring resource_name, FFMPEG_Resource resource_type, const ffmpeg_producer_params& vid_params)
	{
		AVFormatContext* weak_context = nullptr;

		switch (resource_type) {
			case FFMPEG_FILE:
				THROW_ON_ERROR2(avformat_open_input(&weak_context, narrow(resource_name).c_str(), nullptr, nullptr), resource_name);
				break;
			case FFMPEG_DEVICE: {
				AVDictionary* format_options = NULL;
				for (auto it = vid_params.options.begin(); it != vid_params.options.end(); ++it)
				{
					av_dict_set(&format_options, (*it).name.c_str(), (*it).value.c_str(), 0);
				}
				AVInputFormat* input_format = av_find_input_format("dshow");
				THROW_ON_ERROR2(avformat_open_input(&weak_context, narrow(resource_name).c_str(), input_format, &format_options), resource_name);
				if (format_options != nullptr)
				{
					std::string unsupported_tokens = "";
					AVDictionaryEntry *t = NULL;
					while ((t = av_dict_get(format_options, "", t, AV_DICT_IGNORE_SUFFIX)) != nullptr)
					{
						if (!unsupported_tokens.empty())
							unsupported_tokens += ", ";
						unsupported_tokens += t->key;
					}
					avformat_close_input(&weak_context);
					BOOST_THROW_EXCEPTION(ffmpeg_error() << msg_info(unsupported_tokens));
				}
				av_dict_free(&format_options);
			} break;
			case FFMPEG_STREAM: {
				AVDictionary* format_options = NULL;
				for (auto it = vid_params.options.begin(); it != vid_params.options.end(); ++it)
				{
					av_dict_set(&format_options, (*it).name.c_str(), (*it).value.c_str(), 0);
				}
				THROW_ON_ERROR2(avformat_open_input(&weak_context, narrow(resource_name).c_str(), nullptr, &format_options), resource_name);
				if (format_options != nullptr)
				{
					std::string unsupported_tokens = "";
					AVDictionaryEntry *t = NULL;
					while ((t = av_dict_get(format_options, "", t, AV_DICT_IGNORE_SUFFIX)) != nullptr)
					{
						if (!unsupported_tokens.empty())
							unsupported_tokens += ", ";
						unsupported_tokens += t->key;
					}
					avformat_close_input(&weak_context);
					BOOST_THROW_EXCEPTION(ffmpeg_error() << msg_info(unsupported_tokens));
				}
				av_dict_free(&format_options);
			} break;
		};
		safe_ptr<AVFormatContext> context(weak_context, [](AVFormatContext* ctx){avformat_close_input(&ctx);});      
		THROW_ON_ERROR2(avformat_find_stream_info(weak_context, nullptr), resource_name);
		//fix_meta_data(&weak_context);
		return context;
	}

  void fix_meta_data(AVFormatContext& context)
  {
    auto video_index = av_find_best_stream(&context, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);

    if(video_index > -1)
    {
     auto video_stream   = context.streams[video_index];
      auto video_context  = context.streams[video_index]->codec;
            
      if(boost::filesystem2::path(context.filename).extension() == ".flv")
      {
        try
        {
          auto meta = read_flv_meta_info(context.filename);
          double fps = boost::lexical_cast<double>(meta["framerate"]);
          video_stream->nb_frames = static_cast<int64_t>(boost::lexical_cast<double>(meta["duration"])*fps);
        }
        catch(...){}
      }
      else
      {
        auto stream_time = video_stream->time_base;
        auto duration   = video_stream->duration;
        auto codec_time  = video_context->time_base;
        auto ticks     = video_context->ticks_per_frame;

        if(video_stream->nb_frames == 0)
          video_stream->nb_frames = (duration*stream_time.num*codec_time.den)/(stream_time.den*codec_time.num*ticks);  
      }
    }
  }
			
	void queued_seek(const uint32_t target)
	{  	
		if (!thumbnail_mode_)
			CASPAR_LOG(debug) << print() << " Seeking: " << target;
		start_ = target;
		frame_number_ = target;
		flush_packet_count_ = FLUSH_PACKET_COUNT;
		is_eof_ = false;
		if (default_stream_ && default_stream_->codec->codec_type == AVMEDIA_TYPE_VIDEO && default_stream_->avg_frame_rate.num > 0)
		{
				start_time_ = (AV_TIME_BASE * static_cast<int64_t>(start_) * default_stream_->avg_frame_rate.den)/default_stream_->avg_frame_rate.num;
				if (length_ != std::numeric_limits<uint32_t>().max())
					end_time_ = (AV_TIME_BASE * static_cast<int64_t>(start_+length_-1) * default_stream_->avg_frame_rate.den)/default_stream_->avg_frame_rate.num;
		}
		else  // in case no video == no frame rate
		{
				start_time_ = static_cast<int64_t>((AV_TIME_BASE * static_cast<int64_t>(start_))/channel_fps_);
				if (length_ != std::numeric_limits<uint32_t>().max())
					end_time_ = static_cast<int64_t>((AV_TIME_BASE * static_cast<int64_t>(start_+length_-1))/channel_fps_);
		}
		CASPAR_LOG(info) << "Requestet start time: " << start_time_;
		
		int64_t seek;
		bool seek_by_stream_time = default_stream_->avg_frame_rate.num != 0 && default_stream_->codec->codec_type == AVMEDIA_TYPE_VIDEO;
		if (seek_by_stream_time)
			seek = (static_cast<int64_t>(target) * default_stream_->time_base.den * default_stream_->avg_frame_rate.den)/(default_stream_->time_base.num * default_stream_->avg_frame_rate.num) 
			+ (stream_start_time ==		AV_NOPTS_VALUE ? 0 : stream_start_time) 
			+ default_stream_->first_dts;// - 1024; //don't know why?		
		else
			seek = static_cast<int64_t>(target / channel_fps_ * AV_TIME_BASE) + format_context_->start_time;
		THROW_ON_ERROR2(av_seek_frame(format_context_.get(), seek_by_stream_time ? default_stream_index_ : -1, seek, AVSEEK_FLAG_BACKWARD), "[input]"); // Again, why backward? but it just works.
		avformat_flush(format_context_.get());
		tick();
	}	

};

input::input(const safe_ptr<diagnostics::graph>& graph, const std::wstring& filename, FFMPEG_Resource resource_type, bool loop, uint32_t start, uint32_t length, bool thumbnail_mode, const ffmpeg_producer_params& vid_params, double fps) 
	: impl_(new implementation(graph, filename, resource_type, loop, start, length, thumbnail_mode, vid_params, fps)){}
bool input::eof() const {return !impl_->is_eof_;}
bool input::empty() const {return impl_->buffer_size_ == 0;}
bool input::try_pop(std::shared_ptr<AVPacket>& packet){return impl_->try_pop(packet);}
safe_ptr<AVFormatContext> input::context(){return impl_->format_context_;}
void input::loop(bool value){impl_->loop_ = value;}
bool input::loop() const{return impl_->loop_;}
bool input::seek(uint32_t target){return impl_->seek(target);}
}}
