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
 
#include "../StdAfx.h"

#include "../ffmpeg_error.h"
#include "../tbb_avcodec.h"

#include "ffmpeg_consumer.h"

#include <core/parameters/parameters.h>
#include <core/mixer/read_frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/consumer/frame_consumer.h>
#include <core/video_format.h>

#include <common/concurrency/executor.h>
#include <common/concurrency/future_util.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/utility/string.h>
#include <common/memory/memshfl.h>

#include <boost/algorithm/string.hpp>
#include <boost/timer.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/cache_aligned_allocator.h>
#include <tbb/parallel_invoke.h>
#include <tbb/atomic.h>

#include <boost/range/algorithm.hpp>
#include <boost/range/algorithm_ext.hpp>
#include <boost/lexical_cast.hpp>

#include <string>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavutil/opt.h>
	#include <libavutil/pixdesc.h>
	#include <libavutil/parseutils.h>
	#include <libavutil/imgutils.h>
	#include <libswresample/swresample.h>
	#include <libavutil/audio_fifo.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace ffmpeg {
	
int av_opt_set(void *obj, const char *name, const char *val, int search_flags)
{
	AVClass* av_class = *(AVClass**)obj;

	if((strcmp(name, "pix_fmt") == 0 || strcmp(name, "pixel_format") == 0) && strcmp(av_class->class_name, "AVCodecContext") == 0)
	{
		AVCodecContext* c = (AVCodecContext*)obj;		
		auto pix_fmt = av_get_pix_fmt(val);
		if(pix_fmt == AV_PIX_FMT_NONE)
			return -1;		
		c->pix_fmt = pix_fmt;
		return 0;
	}
	if((strcmp(name, "r") == 0 || strcmp(name, "frame_rate") == 0) && strcmp(av_class->class_name, "AVCodecContext") == 0)
	{
		AVCodecContext* c = (AVCodecContext*)obj;	

		if(c->codec_type != AVMEDIA_TYPE_VIDEO)
			return -1;

		AVRational rate;
		int ret = av_parse_video_rate(&rate, val);
		if(ret < 0)
			return ret;

		c->time_base.num = rate.den;
		c->time_base.den = rate.num;
		return 0;
	}

	return ::av_opt_set(obj, name, val, search_flags);
}

AVFormatContext * alloc_output_format_context(const std::string& filename, AVOutputFormat * output_format)
{
	AVFormatContext * ctx = NULL;
	if (avformat_alloc_output_context2(&ctx, output_format, NULL, filename.c_str()) >= 0)
		return ctx;
	else
		return nullptr;
}

static const std::string			MXF = ".MXF";

struct output_format
{
	int									width;
	int									height;
	AVCodecID							vcodec;
	AVCodecID							acodec;
	caspar::core::video_format::type    video_format;
	AVRational							sample_aspect_ratio;
	AVOutputFormat*						format;
	bool								is_mxf;

	output_format(const core::video_format_desc& format_desc, const std::string& filename)
		: format(av_guess_format(NULL, filename.c_str(), NULL))
		, width(format_desc.width)
		, height(format_desc.height)
		, vcodec(AV_CODEC_ID_NONE)
		, acodec(AV_CODEC_ID_NONE)
		, video_format(format_desc.format)
		, is_mxf(std::equal(MXF.rbegin(), MXF.rend(), filename.rbegin()))
	{
		if (format == NULL)
			BOOST_THROW_EXCEPTION(caspar_exception()
				<< msg_info(filename + " not a supported file for recording"));
		if (is_mxf)
			format = av_guess_format("mxf_d10", filename.c_str(), NULL);

		if (vcodec == AV_CODEC_ID_NONE)
			vcodec = format->video_codec;

		if (acodec == AV_CODEC_ID_NONE)
			acodec = format->audio_codec;

		if (vcodec == AV_CODEC_ID_NONE)
			vcodec = AV_CODEC_ID_H264;

		if (acodec == AV_CODEC_ID_NONE)
			acodec = AV_CODEC_ID_PCM_S16LE;

		switch (video_format) {
		case caspar::core::video_format::pal:
			sample_aspect_ratio = av_make_q(64, 45);
			break;
		case caspar::core::video_format::ntsc:
			sample_aspect_ratio = av_make_q(32, 27);
			break;
		default:
			sample_aspect_ratio = av_make_q(1, 1);
			break;
		}

	}
	
	bool set_opt(const std::string& name, const std::string& value)
	{
		//if(name == "target")
		//{ 
		//	enum { PAL, NTSC, FILM, UNKNOWN } norm = UNKNOWN;
		//	
		//	if(name.find("pal-") != std::string::npos)
		//		norm = PAL;
		//	else if(name.find("ntsc-") != std::string::npos)
		//		norm = NTSC;

		//	if(norm == UNKNOWN)
		//		BOOST_THROW_EXCEPTION(invalid_argument() << arg_name_info("target"));
		//	
		//	if (name.find("-dv") != std::string::npos) 
		//	{
		//		set_opt("f", "dv");
		//		set_opt("s", norm == PAL ? "720x576" : "720x480");
		//		//set_opt("pix_fmt", name.find("-dv50") != std::string::npos ? "yuv422p" : norm == PAL ? "yuv420p" : "yuv411p");
		//		//set_opt("ar", "48000");
		//		//set_opt("ac", "2");
		//	} 
		//}
		if(name == "f")
		{
			format = av_guess_format(value.c_str(), nullptr, nullptr);

			if(format == nullptr)
				BOOST_THROW_EXCEPTION(invalid_argument() << arg_name_info("f"));

			return true;
		}
		else if(name == "vcodec")
		{
			auto c = avcodec_find_encoder_by_name(value.c_str());
			if(c == nullptr)
				BOOST_THROW_EXCEPTION(invalid_argument() << arg_name_info("vcodec"));

			vcodec = avcodec_find_encoder_by_name(value.c_str())->id;
			return true;

		}
		else if(name == "acodec")
		{
			auto c = avcodec_find_encoder_by_name(value.c_str());
			if(c == nullptr)
				BOOST_THROW_EXCEPTION(invalid_argument() << arg_name_info("acodec"));

			acodec = avcodec_find_encoder_by_name(value.c_str())->id;

			return true;
		}
		else if(name == "s")
		{
			if(av_parse_video_size(&width, &height, value.c_str()) < 0)
				BOOST_THROW_EXCEPTION(invalid_argument() << arg_name_info("s"));
			
			return true;
		}

		return false;
	}
};

typedef std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>>	byte_vector;

struct ffmpeg_consumer : boost::noncopyable
{
	const std::string						filename_;

	output_format							output_format_;
	const std::shared_ptr<AVFormatContext>	format_context_;
	const core::video_format_desc			format_desc_;

	const safe_ptr<diagnostics::graph>		graph_;

	executor								encode_executor_;

	std::shared_ptr<AVStream>				audio_st_;
	std::shared_ptr<AVStream>				video_st_;
	std::shared_ptr<AVAudioFifo>			audio_fifo_;

	byte_vector								audio_bufers_[AV_NUM_DATA_POINTERS];
	byte_vector								key_picture_buf_;
	byte_vector								picture_buf_;
	std::shared_ptr<SwrContext>				swr_;
	std::shared_ptr<SwsContext>				sws_;

	int64_t									in_frame_number_;
	int64_t									out_frame_number_;
	int64_t									out_audio_sample_number_;

	bool									key_only_;
	bool									audio_is_planar;
	tbb::atomic<int64_t>					current_encoding_delay_;

public:
	ffmpeg_consumer(const std::string& filename, const core::video_format_desc& format_desc, bool key_only, bool frag_mov)
		: filename_(filename)
		, format_context_(alloc_output_format_context(filename, output_format_.format), avformat_free_context)
		, format_desc_(format_desc)
		, encode_executor_(print())
		, in_frame_number_(0)
		, out_frame_number_(0)
		, out_audio_sample_number_(0)
		, output_format_(format_desc, filename)
		, key_only_(key_only)
	{
		current_encoding_delay_ = 0;
		// TODO: Ask stakeholders about case where file already exists.
		boost::filesystem2::remove(boost::filesystem2::wpath(env::media_folder() + widen(filename))); // Delete the file if it exists

		graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);

		encode_executor_.set_capacity(8);

		encode_executor_.begin_invoke([this, frag_mov] {
			//strcpy_s(format_context_->filename, filename_.c_str());

			//  Add the audio and video streams using the default format codecs	and initialize the codecs.
			video_st_ = add_video_stream();
			if (!key_only_)
				audio_st_ = add_audio_stream();

			av_dump_format(format_context_.get(), 0, filename_.c_str(), 1);
			
			// Open the output ffmpeg
			THROW_ON_ERROR2(avio_open(&format_context_->pb, filename_.c_str(), AVIO_FLAG_WRITE | AVIO_FLAG_NONBLOCK), "[ffmpeg_consumer]");
			AVDictionary * dict;
			if (frag_mov && !std::strcmp(output_format_.format->name, "mov"))
				av_dict_set(&dict, "movflags", "frag_keyframe", 1);
			THROW_ON_ERROR2(avformat_write_header(format_context_.get(), &dict), "[ffmpeg_consumer]");
			CASPAR_LOG(info) << print() << L" Successfully Initialized.";
		});
	}

	~ffmpeg_consumer()
	{
		encode_executor_.stop();
		encode_executor_.join();
		LOG_ON_ERROR2(av_write_trailer(format_context_.get()), "[ffmpeg_consumer]");
		if (!key_only_)
			audio_st_.reset();
		video_st_.reset();
		LOG_ON_ERROR2(avio_close(format_context_->pb), "[ffmpeg_consumer]"); // Close the output ffmpeg.

		CASPAR_LOG(info) << print() << L" Successfully Uninitialized.";
	}

	std::wstring print() const
	{
		return L"ffmpeg[" + widen(filename_) + L"]";
	}

	std::shared_ptr<AVStream> add_video_stream()
	{
		if (output_format_.vcodec == AV_CODEC_ID_NONE)
			return nullptr;

		AVCodec * encoder = avcodec_find_encoder(output_format_.vcodec);
		if (!encoder)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Codec not found."));

		AVStream * st = avformat_new_stream(format_context_.get(), encoder);
		if (!st)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate video-stream.") << boost::errinfo_api_function("av_new_stream"));
		st->id = 0;
		st->time_base = av_make_q(format_desc_.duration, format_desc_.time_scale);

		AVCodecContext * c = st->codec;

		c->codec_id = output_format_.vcodec;
		c->codec_type = AVMEDIA_TYPE_VIDEO;
		c->width = output_format_.width;
		c->height = output_format_.height;
		c->gop_size = 25;
		c->time_base = st->time_base;
		c->flags |= format_desc_.field_mode == core::field_mode::progressive ? 0 : (CODEC_FLAG_INTERLACED_ME | CODEC_FLAG_INTERLACED_DCT);
		if (c->pix_fmt == AV_PIX_FMT_NONE)
			c->pix_fmt = AV_PIX_FMT_YUV420P;

		if (c->codec_id == AV_CODEC_ID_PRORES)
		{
			c->bit_rate = c->width < 1280 ? 63 * 1000000 : 220 * 1000000;
			c->pix_fmt = AV_PIX_FMT_YUV422P10;
		}
		else if (c->codec_id == AV_CODEC_ID_DNXHD)
		{
			if (c->width < 1280 || c->height < 720)
				BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Unsupported video dimensions."));

			c->bit_rate = 220 * 1000000;
			c->pix_fmt = AV_PIX_FMT_YUV422P;
		}
		else if (c->codec_id == AV_CODEC_ID_DVVIDEO)
		{
			c->width = c->height == 1280 ? 960 : c->width;

			if (format_desc_.format == core::video_format::ntsc)
				c->pix_fmt = AV_PIX_FMT_YUV411P;
			else if (format_desc_.format == core::video_format::pal)
				c->pix_fmt = AV_PIX_FMT_YUV420P;
			else // dv50
				c->pix_fmt = AV_PIX_FMT_YUV422P;

			if (format_desc_.duration == 1001)
				c->width = c->height == 1080 ? 1280 : c->width;
			else
				c->width = c->height == 1080 ? 1440 : c->width;
		}
		else if (c->codec_id == AV_CODEC_ID_H264)
		{
			c->pix_fmt = AV_PIX_FMT_YUV420P;
			c->bit_rate = output_format_.height * 14 * 1000; // about 8Mbps for SD, 14 for HD
			::av_opt_set(c, "preset", "veryfast", NULL);
		}
		else if (c->codec_id == AV_CODEC_ID_QTRLE)
		{
			c->pix_fmt = AV_PIX_FMT_ARGB;
		}
		else if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
		{
			if (output_format_.is_mxf && format_desc_.format == core::video_format::pal)
			{
				// IMX50 encoding parameters
				c->pix_fmt = AV_PIX_FMT_YUV422P;
				c->bit_rate = 50 * 1000000;
				c->rc_max_rate = c->bit_rate;
				c->rc_min_rate = c->bit_rate;
				c->rc_buffer_size = 2000000;
				c->rc_initial_buffer_occupancy = 2000000;
				c->rc_buffer_aggressivity = 0.25;
				c->gop_size = 1;
			}
			else
			{
				c->pix_fmt = AV_PIX_FMT_YUV422P;
				c->bit_rate = 15 * 1000000;
			}
		}

		c->max_b_frames = 0; // b-frames not supported.

		if (output_format_.format->flags & AVFMT_GLOBALHEADER)
			c->flags |= CODEC_FLAG_GLOBAL_HEADER;
		c->sample_aspect_ratio = output_format_.sample_aspect_ratio;

		c->thread_count = boost::thread::hardware_concurrency();
		if (avcodec_open2(c, encoder, NULL) < 0)
		{
			CASPAR_LOG(debug) << print() << L" Multithreaded avcodec_open2 failed";
			c->thread_count = 1;
			THROW_ON_ERROR2(avcodec_open2(c, encoder, NULL), "[ffmpeg_consumer]");
		}

		return std::shared_ptr<AVStream>(st, [](AVStream* st)
		{
			LOG_ON_ERROR2(avcodec_close(st->codec), "[ffmpeg_consumer]");
		});
	}

	std::shared_ptr<AVStream> add_audio_stream()
	{
		if (output_format_.acodec == AV_CODEC_ID_NONE)
			return nullptr;

		AVCodec * encoder = avcodec_find_encoder(output_format_.acodec);
		if (!encoder)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("codec not found") << boost::errinfo_api_function("avcodec_find_encoder"));

		auto st = avformat_new_stream(format_context_.get(), encoder);
		if (!st)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate audio-stream") << boost::errinfo_api_function("av_new_stream"));
		st->id = 1;


		AVCodecContext * c = st->codec;
		c->codec_id = output_format_.acodec;
		c->codec_type = AVMEDIA_TYPE_AUDIO;
		c->sample_rate = format_desc_.audio_sample_rate; 
		c->channels = 2;
		c->channel_layout = av_get_default_channel_layout(c->channels);
		c->profile = FF_PROFILE_UNKNOWN;
		c->sample_fmt = encoder->sample_fmts[0];
		if (output_format_.vcodec == AV_CODEC_ID_FLV1)
			c->sample_rate = 44100;

		if (output_format_.acodec == AV_CODEC_ID_AAC)
		{
			c->sample_fmt = AV_SAMPLE_FMT_FLTP;
			c->profile = FF_PROFILE_AAC_MAIN;
			c->bit_rate = 160 * 1024;
		}
		if (output_format_.is_mxf)
		{
			c->channels = 4;
			c->channel_layout = AV_CH_LAYOUT_4POINT0;
			c->sample_fmt = AV_SAMPLE_FMT_S16;
			c->bit_rate_tolerance = 0;
		}

		if (output_format_.format->flags & AVFMT_GLOBALHEADER)
			c->flags |= CODEC_FLAG_GLOBAL_HEADER;
		
		audio_is_planar = av_sample_fmt_is_planar(c->sample_fmt) != 0;
		audio_fifo_.reset(av_audio_fifo_alloc(c->sample_fmt, c->channels, 1), av_audio_fifo_free);

		THROW_ON_ERROR2(avcodec_open2(c, encoder, nullptr), "[ffmpeg_consumer]");

		return std::shared_ptr<AVStream>(st, [](AVStream* st)
		{
			LOG_ON_ERROR2(avcodec_close(st->codec), "[ffmpeg_consumer]");;
		});
	}

	std::shared_ptr<AVFrame> convert_video(core::read_frame& frame, AVCodecContext* c)
	{
		if (!sws_)
		{
			sws_.reset(sws_getContext(format_desc_.width, format_desc_.height, AV_PIX_FMT_BGRA, c->width, c->height, c->pix_fmt, SWS_BICUBIC, nullptr, nullptr, NULL), sws_freeContext);
			if (sws_ == nullptr)
				BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Cannot initialize the conversion context"));
		}
		std::shared_ptr<AVFrame> in_frame(av_frame_alloc(), [](AVFrame* frame) {av_frame_free(&frame); });
		auto in_picture = reinterpret_cast<AVPicture*>(in_frame.get());

		if (key_only_)
		{
			key_picture_buf_.resize(frame.image_data().size());
			in_picture->linesize[0] = format_desc_.width * 4; //AV_PIX_FMT_BGRA
			in_picture->data[0] = key_picture_buf_.data();

			fast_memshfl(in_picture->data[0], frame.image_data().begin(), frame.image_data().size(), 0x0F0F0F0F, 0x0B0B0B0B, 0x07070707, 0x03030303);
		}
		else
		{
			avpicture_fill(in_picture, const_cast<uint8_t*>(frame.image_data().begin()), AV_PIX_FMT_BGRA, format_desc_.width, format_desc_.height);
		}

		std::shared_ptr<AVFrame> out_frame(av_frame_alloc(), [](AVFrame* frame) {av_frame_free(&frame); });
		picture_buf_.resize(av_image_get_buffer_size(c->pix_fmt, c->width, c->height, 16));
		av_image_fill_arrays(out_frame->data, out_frame->linesize, picture_buf_.data(), c->pix_fmt, c->width, c->height, 16);

		sws_scale(sws_.get(), in_frame->data, in_frame->linesize, 0, format_desc_.height, out_frame->data, out_frame->linesize);
		out_frame->width = format_desc_.width;
		out_frame->height = format_desc_.height;
		out_frame->format = c->pix_fmt;

		return out_frame;
	}

	void encode_video_frame(core::read_frame& frame)
	{
		AVCodecContext * codec_context = video_st_->codec;

		auto in_time = static_cast<double>(in_frame_number_) / format_desc_.fps;
		auto out_time = static_cast<double>(out_frame_number_) / (static_cast<double>(codec_context->time_base.den) / static_cast<double>(codec_context->time_base.num));

		in_frame_number_++;

		if (out_time - in_time > 0.01)
			return;

		auto av_frame = convert_video(frame, codec_context);
		av_frame->interlaced_frame = format_desc_.field_mode != core::field_mode::progressive;
		av_frame->top_field_first = format_desc_.field_mode == core::field_mode::upper;
		av_frame->pts = out_frame_number_++;

		std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });

		av_init_packet(pkt.get());
		int got_packet;

		THROW_ON_ERROR2(avcodec_encode_video2(codec_context, pkt.get(), av_frame.get(), &got_packet), "[audio_encoder]");

		if (got_packet == 0)
			return;

		if (pkt->pts != AV_NOPTS_VALUE)
			pkt->pts = av_rescale_q(pkt->pts, codec_context->time_base, video_st_->time_base);
		if (pkt->dts != AV_NOPTS_VALUE)
			pkt->dts = av_rescale_q(pkt->dts, codec_context->time_base, video_st_->time_base);

		if (codec_context->coded_frame->key_frame)
			pkt->flags |= AV_PKT_FLAG_KEY;

		pkt->stream_index = video_st_->index;

		av_interleaved_write_frame(format_context_.get(), pkt.get());
	}
	
	void resample_audio(core::read_frame& frame, AVCodecContext* ctx)
	{
		if (!swr_)
		{
			uint64_t out_channel_layout = av_get_default_channel_layout(ctx->channels);
			uint64_t in_channel_layout = create_channel_layout_bitmask(frame.num_channels());
			swr_.reset(swr_alloc_set_opts(nullptr,
				out_channel_layout,
				ctx->sample_fmt,
				ctx->sample_rate,
				in_channel_layout,
				AV_SAMPLE_FMT_S32,
				format_desc_.audio_sample_rate,
				0, nullptr), [](SwrContext* ctx) { swr_free(&ctx); });

			if (!swr_)
				BOOST_THROW_EXCEPTION(caspar_exception()
					<< msg_info("Cannot alloc audio resampler"));

			THROW_ON_ERROR2(swr_init(swr_.get()), "[audio_encoder]");
		}
		byte_vector out_buffers[AV_NUM_DATA_POINTERS];
		const int in_samples_count = frame.audio_data().size() / frame.num_channels();
		const int out_samples_count = static_cast<int>(av_rescale_rnd(in_samples_count, ctx->sample_rate, format_desc_.audio_sample_rate, AV_ROUND_UP));
		if (audio_is_planar)
			for (char i = 0; i < ctx->channels; i++)
				out_buffers[i].resize(out_samples_count * av_get_bytes_per_sample(AV_SAMPLE_FMT_S32));
		else
			out_buffers[0].resize(out_samples_count * av_get_bytes_per_sample(AV_SAMPLE_FMT_S32) *ctx->channels);

		const uint8_t* in[] = { reinterpret_cast<const uint8_t*>(frame.audio_data().begin()) };
		uint8_t*       out[AV_NUM_DATA_POINTERS];
		for (char i = 0; i < AV_NUM_DATA_POINTERS; i++)
			out[i] = out_buffers[i].data();

		int converted_sample_count = swr_convert(swr_.get(),
			out, out_samples_count,
			in, in_samples_count);
		if (audio_is_planar)
			for (char i = 0; i < ctx->channels; i++)
			{
				out_buffers[i].resize(converted_sample_count * av_get_bytes_per_sample(ctx->sample_fmt));
				boost::range::push_back(audio_bufers_[i], out_buffers[i]);
			}
		else
		{
			out_buffers[0].resize(converted_sample_count * av_get_bytes_per_sample(ctx->sample_fmt) * ctx->channels);
			boost::range::push_back(audio_bufers_[0], out_buffers[0]);
		}
	}

	std::int64_t create_channel_layout_bitmask(int num_channels)
	{
		if (num_channels > 63)
			BOOST_THROW_EXCEPTION(caspar_exception("FFMpeg cannot handle more than 63 audio channels"));
		const auto ALL_63_CHANNELS = 0x7FFFFFFFFFFFFFFFULL;
		auto to_shift = 63 - num_channels;
		auto result = ALL_63_CHANNELS >> to_shift;
		return static_cast<std::int64_t>(result);
	}
	
	void encode_audio_frame(core::read_frame& frame)
	{			
		AVCodecContext * enc = audio_st_->codec;
		resample_audio(frame, enc);
		size_t input_audio_size = enc->frame_size == 0 ? 
			audio_bufers_[0].size() :
			enc->frame_size * av_get_bytes_per_sample(enc->sample_fmt) * enc->channels;
		int frame_size = input_audio_size / (av_get_bytes_per_sample(enc->sample_fmt) * enc->channels);
		while (audio_bufers_[0].size() >= input_audio_size)
		{
			std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });
			std::shared_ptr<AVFrame> in_frame(av_frame_alloc(), [](AVFrame* frame) {av_frame_free(&frame); });
			in_frame->nb_samples = frame_size;
			in_frame->pts = out_audio_sample_number_;
			out_audio_sample_number_ += frame_size;
			uint8_t* out_buffers[AV_NUM_DATA_POINTERS];
			for (char i = 0; i < AV_NUM_DATA_POINTERS; i++)
				out_buffers[i] = audio_bufers_[i].data();
			THROW_ON_ERROR2(avcodec_fill_audio_frame(in_frame.get(), enc->channels, enc->sample_fmt, (const uint8_t *)out_buffers[0], input_audio_size, 0), "[audio_encoder]");
			if (audio_is_planar)
				for (char i = 0; i < enc->channels; i++)
					in_frame->data[i] = audio_bufers_[i].data();
			int got_packet;
			THROW_ON_ERROR2(avcodec_encode_audio2(enc, pkt.get(), in_frame.get(), &got_packet), "[audio_encoder]");
			if (audio_is_planar)
				for (char i = 0; i < enc->channels; i++)
					audio_bufers_[i].erase(audio_bufers_[i].begin(), audio_bufers_[i].begin() + (enc->frame_size * av_get_bytes_per_sample(enc->sample_fmt)));
			else
				audio_bufers_[0].erase(audio_bufers_[0].begin(), audio_bufers_[0].begin() + input_audio_size);
			if (!got_packet)
				return;
			pkt->stream_index = audio_st_->index;
			THROW_ON_ERROR2(av_interleaved_write_frame(format_context_.get(), pkt.get()), "[audio_encoder]");
		}
	}
		 
	void send(const safe_ptr<core::read_frame>& frame)
	{
		encode_executor_.begin_invoke([=]
		{		
			boost::timer frame_timer;

			encode_video_frame(*frame);

			if (!key_only_)
				encode_audio_frame(*frame);

			graph_->set_value("frame-time", frame_timer.elapsed()*format_desc_.fps*0.5);
			current_encoding_delay_ = frame->get_age_millis();
		});
	}

	bool ready_for_frame()
	{
		return encode_executor_.size() < encode_executor_.capacity();
	}

	void mark_dropped()
	{
		graph_->set_tag("dropped-frame");

		// TODO: adjust PTS accordingly to make dropped frames contribute
		//       to the total playing time
	}
};

struct ffmpeg_consumer_proxy : public core::frame_consumer
{
	const std::wstring				filename_;
	const bool						separate_key_;
	const bool						frag_mov_;
	core::video_format_desc			format_desc_;

	std::unique_ptr<ffmpeg_consumer> consumer_;
	std::unique_ptr<ffmpeg_consumer> key_only_consumer_;

public:

	ffmpeg_consumer_proxy(const std::wstring& filename, bool separate_key_, bool frag_mov)
		: filename_(filename)
		, separate_key_(separate_key_)
		, frag_mov_(frag_mov)
	{
	}
	
	virtual void initialize(const core::video_format_desc& format_desc, int)
	{
	
		format_desc_ = format_desc;
		consumer_.reset();
		key_only_consumer_.reset();
		consumer_.reset(new ffmpeg_consumer(
			narrow(filename_),
			format_desc_,
			false,
			frag_mov_));

		if (separate_key_)
		{
			boost::filesystem::wpath fill_file(filename_);
			auto without_extension = fill_file.stem();
			auto key_file = env::media_folder() + without_extension + L"_A" + fill_file.extension();

			key_only_consumer_.reset(new ffmpeg_consumer(
				narrow(key_file),
				format_desc_,
				true, 
				frag_mov_));
		}
	}

	virtual int64_t presentation_frame_age_millis() const override
	{
		return consumer_ ? consumer_->current_encoding_delay_ : 0;
	}
	
	virtual boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame) override
	{
		bool ready_for_frame = consumer_->ready_for_frame();

		if (ready_for_frame && separate_key_)
			ready_for_frame = ready_for_frame && key_only_consumer_->ready_for_frame();

		if (ready_for_frame)
		{
			consumer_->send(frame);

			if (separate_key_)
				key_only_consumer_->send(frame);
		}
		else
		{
			consumer_->mark_dropped();

			if (separate_key_)
				key_only_consumer_->mark_dropped();
		}
		return caspar::wrap_as_future(true);
	}
	
	virtual std::wstring print() const override
	{
		return consumer_ ? consumer_->print() : L"[ffmpeg_consumer]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"ffmpeg-consumer");
		info.add(L"filename", filename_);
		info.add(L"separate_key", separate_key_);
		return info;
	}
		
	virtual bool has_synchronization_clock() const override
	{
		return false;
	}

	virtual size_t buffer_depth() const override
	{
		return 1;
	}

	virtual int index() const override
	{
		return 200;
	}

};	

safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params)
{
	if(params.size() < 1 || (params[0] != L"FILE" && params[0] != L"STREAM"))
		return core::frame_consumer::empty();

	auto params2 = params;
	
	auto filename	= (params2.size() > 1 ? params2[1] : L"");
	bool separate_key = params2.remove_if_exists(L"SEPARATE_KEY");
	bool frag_mov = params2.remove_if_exists(L"FRAGMENT_MOOV_ATOM");

	
	if (params2.size() >= 3)
	{
		for (auto opt_it = params2.begin() + 2; opt_it != params2.end();)
		{
			auto name  = narrow(boost::trim_copy(boost::to_lower_copy(*opt_it++))).substr(1);

			if (opt_it == params2.end())
				break;

			auto value = narrow(boost::trim_copy(boost::to_lower_copy(*opt_it++)));
				
			if (value == "h264")
				value = "libx264";
			else if (value == "dvcpro")
				value = "dvvideo";
		}
	}
		
	return make_safe<ffmpeg_consumer_proxy>(env::media_folder() + filename, separate_key, frag_mov);
}

safe_ptr<core::frame_consumer> create_consumer(const boost::property_tree::wptree& ptree)
{
	auto filename		= ptree.get<std::wstring>(L"path");
	auto codec			= ptree.get(L"vcodec", L"libx264");
	auto separate_key	= ptree.get(L"separate-key", false);
	bool frag_mov		= ptree.get(L"fragment-mov-atom", false);

	
	return make_safe<ffmpeg_consumer_proxy>(env::media_folder() + filename, separate_key, frag_mov);
}

}}
