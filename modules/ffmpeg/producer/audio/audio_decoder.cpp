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

#include "audio_decoder.h"

//#include "audio_resampler.h"

#include "../util/util.h"
#include "../../ffmpeg_error.h"

#include <core/video_format.h>
#include <core/mixer/audio/audio_util.h>

#include <tbb/cache_aligned_allocator.h>

#include <queue>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
    #include <libswresample/swresample.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg {
	
struct audio_decoder::implementation : boost::noncopyable
{	
	int															index_;
	const safe_ptr<AVCodecContext>								codec_context_;		
	const AVStream*												stream_;
	const core::video_format_desc								format_desc_;

	const std::shared_ptr<SwrContext>							swr_;
	//audio_resampler												resampler_;
	std::vector<uint8_t,  tbb::cache_aligned_allocator<uint8_t>>	buffer1_;


	std::queue<safe_ptr<AVPacket>>								packets_;
	tbb::atomic<int64_t>										packet_time_;
	core::channel_layout										channel_layout_;
	const int64_t												stream_start_pts_;

public:
	explicit implementation(const safe_ptr<AVFormatContext>& context, const core::video_format_desc& format_desc, const std::wstring& custom_channel_order) 
		: format_desc_(format_desc)	
		, codec_context_(open_codec(*context, AVMEDIA_TYPE_AUDIO, index_))
		/*, resampler_(codec_context_->channels,		codec_context_->channels,
					 format_desc.audio_sample_rate, codec_context_->sample_rate,
					 AV_SAMPLE_FMT_S32,				codec_context_->sample_fmt)
		*/
		, buffer1_(0)
		, channel_layout_(get_audio_channel_layout(*codec_context_, custom_channel_order))
		, swr_(alloc_resampler())
		, stream_(context->streams[index_])
		, stream_start_pts_(stream_->start_time)
	{
		THROW_ON_ERROR2(swr_init(swr_.get()), "[audio_decoder]");
		packet_time_ =  - (stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_);
		CASPAR_LOG(debug) << print() 
				<< " Selected channel layout " << channel_layout_.name;
	}

	void push(const std::shared_ptr<AVPacket>& packet)
	{			
		if(!packet)
			return;

		if(packet->stream_index == index_ || packet->data == nullptr)
			packets_.push(make_safe_ptr(packet));
	}	
	
	std::shared_ptr<core::audio_buffer> poll()
	{
		if(packets_.empty())
			return nullptr;
		auto packet = packets_.front();
		packets_.pop();
		if(packet->data == nullptr)
		{			
			if(codec_context_->codec->capabilities & CODEC_CAP_DELAY)
			{
				auto audio = decode(*packet);
				if(audio)
					return audio;
			}
			return nullptr;
		}
		return decode(*packet);
	}


	std::shared_ptr<SwrContext>	alloc_resampler()
	{
		std::shared_ptr<SwrContext>	resampler(
		swr_alloc_set_opts(
				nullptr,
				create_channel_layout_bitmask(codec_context_->channels),//get_ffmpeg_channel_layout(codec_context_.get()),
				AV_SAMPLE_FMT_S32,
				format_desc_.audio_sample_rate,
				create_channel_layout_bitmask(codec_context_->channels),//get_ffmpeg_channel_layout(codec_context_.get()),
				codec_context_->sample_fmt,
				codec_context_->sample_rate,
				0,
				nullptr),
		[](SwrContext* p){swr_free(&p);});
		return resampler;
	}

	std::shared_ptr<core::audio_buffer> decode(AVPacket& pkt)
	{		
		int got_frame = 0;
		safe_ptr<AVFrame> frame = create_frame();
		int ret = THROW_ON_ERROR2(avcodec_decode_audio4(codec_context_.get(), frame.get(), &got_frame, &pkt), "[audio_decoder]");
		if (ret>=0)
		{
		// There might be several frames in one packet.
		pkt.size -= ret;
		pkt.data += ret;
		if (got_frame)
			{
				int  data_size = av_samples_get_buffer_size(NULL, codec_context_->channels, frame->nb_samples, AV_SAMPLE_FMT_S32, 0);
				buffer1_.resize(data_size);
				const uint8_t **in	= const_cast<const uint8_t**>(frame->extended_data);
				uint8_t* out		= buffer1_.data();
				int n_samples = swr_convert(swr_.get(), 
											&out, 
											static_cast<int>(buffer1_.size()) / codec_context_->channels / av_get_bytes_per_sample(AV_SAMPLE_FMT_S32),
											in, 
											frame->nb_samples);
				if (n_samples > 0)
				{

					const auto samples = reinterpret_cast<uint32_t*>(buffer1_.data());
		
					int64_t frame_time_stamp = av_frame_get_best_effort_timestamp(frame.get());
					if (frame_time_stamp == AV_NOPTS_VALUE)
						packet_time_= std::numeric_limits<int64_t>().max();
					else
						packet_time_ = ((frame_time_stamp - (stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_)) * AV_TIME_BASE * stream_->time_base.num)/stream_->time_base.den;
					//CASPAR_LOG(trace) << print() << "Packet time: " << packet_time_/1000;
					return std::make_shared<core::audio_buffer>(samples, samples + n_samples*codec_context_->channels);
				}
			}
		}
		return empty_audio();
	}

	bool ready() const
	{
		return packets_.size() > 10;
	}

	bool empty() const
	{
		return packets_.size() == 0;
	}

	void clear()
	{
		while (!packets_.empty())
			packets_.pop();
		avcodec_flush_buffers(codec_context_.get());
		packet_time_ = 0;
	}
	
	std::wstring print() const
	{		
		return L"[audio-decoder] " + widen(codec_context_->codec->long_name);
	}
};

audio_decoder::audio_decoder(const safe_ptr<AVFormatContext>& context, const core::video_format_desc& format_desc, const std::wstring& custom_channel_order) : impl_(new implementation(context, format_desc, custom_channel_order)){}
void audio_decoder::push(const std::shared_ptr<AVPacket>& packet){impl_->push(packet);}
bool audio_decoder::ready() const{return impl_->ready();}
bool audio_decoder::empty() const{return impl_->empty();}
std::shared_ptr<core::audio_buffer> audio_decoder::poll(){return impl_->poll();}
const core::channel_layout& audio_decoder::channel_layout() const { return impl_->channel_layout_; }
std::wstring audio_decoder::print() const{return impl_->print();}
int64_t audio_decoder::packet_time() const {return impl_->packet_time_;}
void audio_decoder::clear(){impl_->clear();}


}}