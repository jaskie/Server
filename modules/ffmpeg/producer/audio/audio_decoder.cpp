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
	input 														input_;
	const safe_ptr<AVCodecContext>								codec_context_;
	int															stream_index_;
	const AVStream*												stream_;
	caspar::core::video_format_desc								format_;
	const std::shared_ptr<SwrContext>							swr_;
	std::vector<uint8_t,  tbb::cache_aligned_allocator<uint8_t>>	buffer1_;
	core::channel_layout										channel_layout_;
	const int64_t												stream_start_pts_;
	tbb::atomic<int64_t>										seek_pts_;

public:
	explicit implementation(input input, caspar::core::video_format_desc format, const std::wstring& custom_channel_order)
		: input_(input)
		, codec_context_(input.open_audio_codec(stream_index_))
		, buffer1_(0)
		, format_(format)
		, channel_layout_(get_audio_channel_layout(*codec_context_, custom_channel_order))
		, swr_(alloc_resampler())
		, stream_(input_.format_context()->streams[stream_index_])
		, stream_start_pts_(stream_->start_time)
	{
		seek_pts_ = 0;
		THROW_ON_ERROR2(swr_init(swr_.get()), "[audio_decoder]");
		CASPAR_LOG(debug) << print() 
				<< " Selected channel layout " << channel_layout_.name;
	}
	
	std::shared_ptr<core::audio_buffer> poll()
	{
		std::shared_ptr<core::audio_buffer> audio = nullptr;
		std::shared_ptr<AVPacket> packet = nullptr;
		while (!audio && input_.try_pop_audio(packet))
			audio = decode(packet);
		return audio;
	}


	std::shared_ptr<SwrContext>	alloc_resampler()
	{
		std::shared_ptr<SwrContext>	resampler(
		swr_alloc_set_opts(
				nullptr,
				create_channel_layout_bitmask(codec_context_->channels),//get_ffmpeg_channel_layout(codec_context_.get()),
				AV_SAMPLE_FMT_S32,
				format_.audio_sample_rate,
				create_channel_layout_bitmask(codec_context_->channels),//get_ffmpeg_channel_layout(codec_context_.get()),
				codec_context_->sample_fmt,
				codec_context_->sample_rate,
				0,
				nullptr),
		[](SwrContext* p){swr_free(&p);});
		return resampler;
	}

	std::shared_ptr<core::audio_buffer> decode(std::shared_ptr<AVPacket> pkt)
	{
		int got_frame = 0;
		safe_ptr<AVFrame> frame = create_frame();
		int ret = THROW_ON_ERROR2(avcodec_decode_audio4(codec_context_.get(), frame.get(), &got_frame, pkt.get()), "[audio_decoder]");
		int64_t frame_time_stamp = av_frame_get_best_effort_timestamp(frame.get());
		if (ret >= 0 && got_frame && frame_time_stamp >= seek_pts_)
		{
			int  data_size = av_samples_get_buffer_size(NULL, codec_context_->channels, frame->nb_samples, AV_SAMPLE_FMT_S32, 0);
			buffer1_.resize(data_size);
			const uint8_t **in = const_cast<const uint8_t**>(frame->extended_data);
			uint8_t* out = buffer1_.data();
			int n_samples = swr_convert(swr_.get(),
				&out,
				static_cast<int>(buffer1_.size()) / codec_context_->channels / av_get_bytes_per_sample(AV_SAMPLE_FMT_S32),
				in,
				frame->nb_samples);
			if (n_samples > 0)
			{
				const auto samples = reinterpret_cast<uint32_t*>(buffer1_.data());
				return std::make_shared<core::audio_buffer>(samples, samples + n_samples*codec_context_->channels);
			}
		}
		return nullptr;
	}


	void seek(uint64_t time)
	{
		avcodec_flush_buffers(codec_context_.get());
		seek_pts_ = stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_
			+ (time * stream_->time_base.den / (AV_TIME_BASE * stream_->time_base.num));
	}
	
	std::wstring print() const
	{		
		return L"[audio-decoder] " + widen(codec_context_->codec->long_name);
	}
};

audio_decoder::audio_decoder(input input, caspar::core::video_format_desc format, const std::wstring& custom_channel_order) : impl_(new implementation(input, format, custom_channel_order)){}
std::shared_ptr<core::audio_buffer> audio_decoder::poll(){return impl_->poll();}
const core::channel_layout& audio_decoder::channel_layout() const { return impl_->channel_layout_; }
std::wstring audio_decoder::print() const{return impl_->print();}
void audio_decoder::seek(uint64_t time) {impl_->seek(time);}

}}