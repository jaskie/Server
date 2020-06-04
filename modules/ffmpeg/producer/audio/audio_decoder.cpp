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
	static const int BUFFER_SIZE = 480000 * 2;
	input 														input_;
	const safe_ptr<AVCodecContext>								codec_context_;
	AVStream*													stream_;
	caspar::core::video_format_desc								format_;
	const std::shared_ptr<SwrContext>							swr_;
	core::channel_layout										channel_layout_;
	const int64_t												stream_start_pts_;
	const int64_t												duration_;
	tbb::atomic<int64_t>										time_;
	tbb::atomic<int64_t>										seek_pts_;
	tbb::atomic<bool>											eof_;
	std::vector<int32_t,  tbb::cache_aligned_allocator<int32_t>> buffer_;

public:
	explicit implementation(input input, caspar::core::video_format_desc format, const std::wstring& custom_channel_order)
		: input_(input)
		, codec_context_(input.open_audio_codec(&stream_))
		, format_(format)
		, channel_layout_(get_audio_channel_layout(*codec_context_, custom_channel_order))
		, swr_(alloc_resampler())
		, stream_start_pts_(stream_->start_time == AV_NOPTS_VALUE ? 0 : stream_->start_time)
		, buffer_(BUFFER_SIZE)
		, duration_(calc_duration(stream_))
	{
		seek_pts_ = 0;
		time_ = AV_NOPTS_VALUE;
		eof_ = false;
		THROW_ON_ERROR2(swr_init(swr_.get()), "[audio_decoder]");
		CASPAR_LOG(debug) << print() 
				<< " Selected channel layout " << channel_layout_.name;
	}
	

	int64_t calc_duration(const AVStream* stream)
	{
		if (stream->duration == AV_NOPTS_VALUE)
			return 0;
		else
			return  av_rescale(stream->duration, stream->time_base.num * AV_TIME_BASE, stream->time_base.den);
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

	std::shared_ptr<core::audio_buffer> poll()
	{
		std::shared_ptr<AVPacket> packet;
		input_.try_pop_audio(packet);
		if (packet || (input_.eof() && !eof_))
			avcodec_send_packet(codec_context_.get(), packet.get());
		auto frame = create_frame();
		int ret = avcodec_receive_frame(codec_context_.get(), frame.get());
		if (ret == AVERROR_EOF)
			eof_ = true;
		if (ret < 0)
			return nullptr;
		if (frame->pts == AV_NOPTS_VALUE)
			frame->pts = frame->best_effort_timestamp;
		if (frame->pts != AV_NOPTS_VALUE)
			frame->pts -= stream_start_pts_;
		if (frame->pts < seek_pts_)
			return nullptr;
		time_ = av_rescale(frame->pts * AV_TIME_BASE, stream_->time_base.num, stream_->time_base.den);
		const uint8_t** in = const_cast<const uint8_t**>(frame->extended_data);
		uint8_t* out[] = { reinterpret_cast<uint8_t*>(buffer_.data()) };
		int n_samples = swr_convert(swr_.get(),
			out,
			BUFFER_SIZE / codec_context_->channels,
			in,
			frame->nb_samples);
		if (n_samples > 0)
		{
			const auto samples = reinterpret_cast<uint32_t*>(*out);
			return std::make_shared<core::audio_buffer>(samples, samples + n_samples * codec_context_->channels);
		}
		return nullptr;
	}

	void flush_resampler()
	{
		uint8_t* out[] =  { reinterpret_cast<uint8_t*>(buffer_.data()) };
		while (swr_convert(swr_.get(),
				out,
				BUFFER_SIZE / codec_context_->channels,
				NULL,
				0)>0) {};
	}
	
	void seek(uint64_t time)
	{
		avcodec_flush_buffers(codec_context_.get());
		flush_resampler();
		eof_ = false;
		time_ = AV_NOPTS_VALUE;
		seek_pts_ = stream_start_pts_ == AV_NOPTS_VALUE ? 0 : stream_start_pts_ + (time * stream_->time_base.den / (AV_TIME_BASE * stream_->time_base.num));
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
int64_t audio_decoder::duration() const { return impl_->duration_; }
int64_t audio_decoder::time() const { return impl_->time_; }
bool audio_decoder::eof() const { return impl_->eof_; }
}

}