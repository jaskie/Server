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
#include "../ffmpeg.h"

#include "ffmpeg_consumer.h"

#include <core/parameters/parameters.h>
#include <core/mixer/read_frame.h>
#include <core/mixer/audio/audio_util.h>
#include <core/consumer/frame_consumer.h>
#include <core/video_format.h>
#include <core/recorder.h>

#include <common/concurrency/executor.h>
#include <common/concurrency/future_util.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/memory/memshfl.h>

#include <boost/algorithm/string.hpp>
#include <boost/timer.hpp>
#include <boost/property_tree/ptree.hpp>
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#include <boost/crc.hpp>
#pragma warning(pop)

#include <tbb/cache_aligned_allocator.h>
#include <tbb/parallel_invoke.h>

#include <boost/range/algorithm_ext.hpp>
#include <boost/lexical_cast.hpp>

#include <string>

namespace caspar {
	namespace ffmpeg {

		AVFormatContext * alloc_output_params_context(const std::string filename, AVOutputFormat * output_params)
		{
			AVFormatContext * ctx = nullptr;
			if (avformat_alloc_output_context2(&ctx, output_params, NULL, filename.c_str()) >= 0)
				return ctx;
			else
				return nullptr;
		}

		int crc16(const std::string& str)
		{
			boost::crc_16_type result;
			result.process_bytes(str.data(), str.length());
			return result.checksum();
		}

		static const std::string			MXF = ".MXF";

		struct output_params
		{
			const core::video_format_desc				format_;
			const std::string							file_name_;
			const std::string							video_codec_;
			const std::string							audio_codec_;
			const std::string							output_metadata_;
			const std::string							audio_metadata_;
			const std::string							video_metadata_;
			const int									audio_stream_id_;
			const int									video_stream_id_;
			const std::string							options_;
			const std::string							pixel_format_;
			const bool									is_mxf_;
			const bool									is_narrow_;
			const bool									is_stream_;
			const int									audio_bitrate_;
			const int									video_bitrate_;
			const std::string							file_timecode_;
			
			output_params(
				const core::video_format_desc& format,
				const std::string& filename, 
				const std::string& audio_codec, 
				const std::string& video_codec, 
				const std::string& output_metadata,
				const std::string& audio_metadata,
				const std::string& video_metadata,
				const int audio_stream_id,
				const int video_stream_id,
				const std::string& options,
				const std::string& pixel_format,
				const bool is_stream,
				const bool is_narrow, 
				const int a_rate, 
				const int v_rate, 
				const std::string& file_tc)
				: format_(format)
				, video_codec_(std::move(video_codec))
				, audio_codec_(std::move(audio_codec))
				, output_metadata_(std::move(output_metadata))
				, audio_metadata_(std::move(audio_metadata))
				, video_metadata_(std::move(video_metadata))
				, audio_stream_id_(audio_stream_id)
				, video_stream_id_(video_stream_id)
				, options_(std::move(options))
				, pixel_format_(std::move(pixel_format))
				, is_narrow_(is_narrow)
				, is_stream_(is_stream)
				, is_mxf_(std::equal(MXF.rbegin(), MXF.rend(), boost::to_upper_copy(filename).rbegin()))
				, audio_bitrate_(a_rate)
				, video_bitrate_(v_rate)
				, file_name_(std::move(filename))
				, file_timecode_(std::move(file_tc))
			{ }
			
		};

		AVDictionary * read_parameters(const std::string& options) {
			AVDictionary* result = NULL;
			LOG_ON_ERROR2(av_dict_parse_string(&result, options.c_str(), "=", ",", 0), L"Parameters unrecognized");
			return result;
		}

		typedef std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>>	byte_vector;
		typedef std::unique_ptr<SwsContext, std::function<void(SwsContext *)>> SwsContextPtr;
		typedef std::unique_ptr<SwrContext, std::function<void(SwrContext *)>> SwrContextPtr;
		typedef std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext *)>> AVFormatContextPtr;
		typedef std::unique_ptr<AVCodecContext, std::function<void(AVCodecContext *)>> AVCodecContextPtr;
		
		struct ffmpeg_consumer : boost::noncopyable
		{
			AVDictionary *							options_;
			const output_params						output_params_;
			const core::video_format_desc			channel_format_desc_;
			const core::video_format_desc			output_format_desc_;

			const safe_ptr<diagnostics::graph>		graph_;

			AVFormatContextPtr						format_context_;
			AVStream *								audio_stream_;
			AVStream *								video_stream_;
			AVCodecContextPtr						audio_codec_ctx_;
			AVCodecContextPtr						video_codec_ctx_;

			SwrContextPtr							swr_;
			SwsContextPtr							sws_;
			int										last_frame_no_channels_;

			byte_vector								audio_bufers_[AV_NUM_DATA_POINTERS];
			byte_vector								key_picture_buf_;
			byte_vector								picture_buf_;

			tbb::atomic<int64_t>					out_frame_number_;
			int64_t									out_audio_sample_number_;

			const bool								key_only_;
			bool									audio_is_planar_;
			const bool								is_imx50_pal_;
			const bool								deinterlace_;
			tbb::atomic<int64_t>					current_encoding_delay_;
			boost::timer							frame_timer_;
			executor								encode_executor_;

		public:
			ffmpeg_consumer
			(
				const core::video_format_desc& channel_format_desc,
				output_params params,
				bool key_only
			)
				: encode_executor_(print())
				, out_audio_sample_number_(0)
				, output_params_(std::move(params))
				, channel_format_desc_(channel_format_desc)
				, output_format_desc_(params.format_.format == caspar::core::video_format::invalid ? channel_format_desc : params.format_)
				, key_only_(key_only)
				, options_(read_parameters(params.options_))
				, audio_stream_(nullptr)
				, video_stream_(nullptr)
				, is_imx50_pal_(output_params_.is_mxf_ && output_format_desc_.format == core::video_format::pal)
				, deinterlace_(output_format_desc_.field_mode != channel_format_desc.field_mode)
			{

				if (deinterlace_) //TODO
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Deinterlacing not yet supported."));

				current_encoding_delay_ = 0;
				out_frame_number_ = 0;

				if (boost::filesystem::exists(output_params_.file_name_))
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("File already exists: " + params.file_name_));

				graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
				graph_->set_color("dropped-frame", diagnostics::color(1.0f, 0.1f, 0.1f));
				graph_->set_text(print());
				diagnostics::register_graph(graph_);

				encode_executor_.set_capacity(16);

				try
				{
					AVOutputFormat * format = NULL;
					if (output_params_.is_stream_)
					{
						if (output_params_.file_name_.find("rtmp://") == 0)
							format = av_guess_format("flv", NULL, NULL);
						else
							format = av_guess_format("mpegts", NULL, NULL);
					}
					if (!format && is_imx50_pal_)
						format = av_guess_format("mxf_d10", output_params_.file_name_.c_str(), NULL);
					if (!format)
						format = av_guess_format(NULL, output_params_.file_name_.c_str(), NULL);
					if (!format)
						BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not guess output format."));
			
					format_context_ = AVFormatContextPtr(alloc_output_params_context(output_params_.file_name_, format), [](AVFormatContext * ctx)
					{
						if (!(ctx->oformat->flags & AVFMT_NOFILE))
							LOG_ON_ERROR2(avio_close(ctx->pb), "[ffmpeg_consumer]"); 
						avformat_free_context(ctx);
					});
					
					AVCodec * video_codec = NULL;
					AVCodec * audio_codec = NULL;
					video_codec = output_params_.video_codec_.empty()
						? output_params_.is_mxf_ ? avcodec_find_encoder_by_name("mpeg2video") : avcodec_find_encoder_by_name("libx264")
						: avcodec_find_encoder_by_name(output_params_.video_codec_.c_str());
					audio_codec = output_params_.audio_codec_.empty()
						? output_params_.is_mxf_ ? avcodec_find_encoder_by_name("pcm_s16le") : avcodec_find_encoder_by_name("aac")
						: avcodec_find_encoder_by_name(output_params_.audio_codec_.c_str());

					video_stream_ = add_video_stream(video_codec, format);

					if (!key_only)
						audio_stream_ = add_audio_stream(audio_codec, format);

					LOG_ON_ERROR2(av_dict_set(&video_stream_->metadata, "timecode", output_params_.file_timecode_.c_str(), AV_DICT_DONT_OVERWRITE), "[ffmpeg_consumer]");
					
					// Open the output
					format_context_->metadata = read_parameters(params.output_metadata_);
					if (format_context_->max_delay <= 0)
						format_context_->max_delay = AV_TIME_BASE * 7 / 10;
					format_context_->flags = AVFMT_FLAG_FLUSH_PACKETS | format_context_->flags;

					av_dump_format(format_context_.get(), 0, output_params_.file_name_.c_str(), 1);

					if (!(format_context_->oformat->flags & AVFMT_NOFILE))
						THROW_ON_ERROR2(avio_open2(&format_context_->pb, output_params_.file_name_.c_str(), AVIO_FLAG_WRITE, NULL, &options_), "[ffmpeg_consumer]");

					THROW_ON_ERROR2(avformat_write_header(format_context_.get(), &options_), "[ffmpeg_consumer]");

					char * unused_options;
					if (options_
						&& av_dict_count(options_) > 0
						&& av_dict_get_string(options_, &unused_options, '=', ',') >= 0)
					{
						CASPAR_LOG(warning) << print() << L" Unrecognized FFMpeg options: " << widen(std::string(unused_options));
						if (unused_options)
							delete(unused_options);
						av_dict_free(&options_);
					}
					create_sws();
				}
				catch (...)
				{
					boost::filesystem2::remove(output_params_.file_name_); // Delete the file if exists and consumer not fully initialized
					throw;
				}
				CASPAR_LOG(info) << print() << L" Successfully Initialized.";
			}

			~ffmpeg_consumer()
			{
				encode_executor_.begin_invoke([this] {
					if ((video_codec_ctx_->codec->capabilities & AV_CODEC_CAP_DELAY)
						|| (!key_only_ && audio_codec_ctx_ && (audio_codec_ctx_->codec->capabilities & AV_CODEC_CAP_DELAY)))
						flush_encoders();
					avio_flush(format_context_->pb);
					LOG_ON_ERROR2(av_write_trailer(format_context_.get()), "[ffmpeg_consumer]");
				});
				if (options_)
					av_dict_free(&options_);
				CASPAR_LOG(info) << print() << L" Successfully Uninitialized.";
			}

			std::wstring print() const
			{
				return L"ffmpeg_consumer[" + widen(output_params_.file_name_) + L"]:" + boost::lexical_cast<std::wstring>(out_frame_number_);
			}

			void create_sws()
			{
				if (channel_format_desc_.field_mode == caspar::core::field_mode::progressive)
				{
					sws_ = SwsContextPtr(
						sws_getContext(channel_format_desc_.width, channel_format_desc_.height, AV_PIX_FMT_BGRA, output_format_desc_.width, output_format_desc_.height, video_codec_ctx_->pix_fmt, 0, nullptr, nullptr, NULL),
						[](SwsContext * ctx) { sws_freeContext(ctx); });
				}
				else
				{
					sws_ = SwsContextPtr(
						sws_getContext(channel_format_desc_.width, channel_format_desc_.height / 2, AV_PIX_FMT_BGRA, output_format_desc_.width, output_format_desc_.height/2, video_codec_ctx_->pix_fmt, 0, nullptr, nullptr, NULL),
						[](SwsContext * ctx) { sws_freeContext(ctx); });
				}
				if (!sws_)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Cannot initialize the conversion context"));
			}

			AVStream* add_video_stream(AVCodec * encoder, AVOutputFormat * format)
			{

				if (!encoder)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Codec not found."));

				video_codec_ctx_ = AVCodecContextPtr(avcodec_alloc_context3(encoder), [](AVCodecContext * ctx) { avcodec_free_context(&ctx); });

				video_codec_ctx_->refcounted_frames = 0;
				video_codec_ctx_->codec_id = encoder->id;
				video_codec_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
				video_codec_ctx_->width = output_format_desc_.width;
				video_codec_ctx_->height = output_format_desc_.height;
				video_codec_ctx_->time_base = av_make_q(output_format_desc_.duration, output_format_desc_.time_scale);
				video_codec_ctx_->framerate = av_inv_q(video_codec_ctx_->time_base);
				video_codec_ctx_->flags |= output_format_desc_.field_mode == core::field_mode::progressive ? 0 : (AV_CODEC_FLAG_INTERLACED_ME | AV_CODEC_FLAG_INTERLACED_DCT);
				if (video_codec_ctx_->pix_fmt == AV_PIX_FMT_NONE)
					video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

				if (video_codec_ctx_->codec_id == AV_CODEC_ID_PRORES)
				{
					video_codec_ctx_->bit_rate = video_codec_ctx_->width < 1280 ? 63 * 1000000 : 220 * 1000000;
					video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV422P10;
				}
				else if (video_codec_ctx_->codec_id == AV_CODEC_ID_DNXHD)
				{
					if (video_codec_ctx_->width < 1280 || video_codec_ctx_->height < 720)
						BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Unsupported video dimensions."));
					video_codec_ctx_->bit_rate = 220 * 1000000;
					video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV422P;
				}
				else if (video_codec_ctx_->codec_id == AV_CODEC_ID_DVVIDEO)
				{
					video_codec_ctx_->width = video_codec_ctx_->height == 1280 ? 960 : video_codec_ctx_->width;

					if (output_format_desc_.format == core::video_format::ntsc)
						video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV411P;
					else if (output_format_desc_.format == core::video_format::pal)
						video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
					else // dv50
						video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV422P;

					if (output_format_desc_.duration == 1001)
						video_codec_ctx_->width = video_codec_ctx_->height == 1080 ? 1280 : video_codec_ctx_->width;
					else
						video_codec_ctx_->width = video_codec_ctx_->height == 1080 ? 1440 : video_codec_ctx_->width;
				}
				else if (video_codec_ctx_->codec_id == AV_CODEC_ID_H264)
				{
					video_codec_ctx_->bit_rate = output_format_desc_.height * 14 * 1000; // about 8Mbps for SD, 14 for HD
					if (strcmp(encoder->name, "libx264") == 0)
						LOG_ON_ERROR2(av_opt_set(video_codec_ctx_->priv_data, "preset", "veryfast", NULL), "[ffmpeg_consumer]");
				}
				else if (video_codec_ctx_->codec_id == AV_CODEC_ID_QTRLE)
				{
					video_codec_ctx_->pix_fmt = AV_PIX_FMT_ARGB;
				}
				else if (video_codec_ctx_->codec_id == AV_CODEC_ID_MPEG2VIDEO)
				{
					if (output_params_.is_mxf_)
					{
						video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV422P;
						video_codec_ctx_->bit_rate = 50 * 1000000;
						if (output_format_desc_.format == core::video_format::pal)
						{
							// IMX50 encoding parameters
							video_codec_ctx_->bit_rate = 50 * 1000000;
							video_codec_ctx_->height = 608;
							video_codec_ctx_->rc_max_rate = video_codec_ctx_->bit_rate;
							video_codec_ctx_->rc_min_rate = video_codec_ctx_->bit_rate;
							video_codec_ctx_->rc_buffer_size = 2000000;
							video_codec_ctx_->rc_initial_buffer_occupancy = 2000000;
							video_codec_ctx_->gop_size = 1;
						}
					}
				}

				if (output_params_.video_bitrate_ != 0)
					video_codec_ctx_->bit_rate = output_params_.video_bitrate_ * 1000;
				if (!output_params_.pixel_format_.empty())
					video_codec_ctx_->pix_fmt = av_get_pix_fmt(output_params_.pixel_format_.c_str());

				video_codec_ctx_->max_b_frames = 0; // b-frames not supported.

				AVRational sample_aspect_ratio;
				switch (output_format_desc_.format) {
				case caspar::core::video_format::pal:
					sample_aspect_ratio = output_params_.is_narrow_ ? av_make_q(16, 15) : av_make_q(64, 45);
					break;
				case caspar::core::video_format::ntsc:
					sample_aspect_ratio = output_params_.is_narrow_ ? av_make_q(8, 9) : av_make_q(32, 27);
					break;
				default:
					sample_aspect_ratio = av_make_q(1, 1);
					break;
				}
				
				if (format->flags & AVFMT_GLOBALHEADER)
					video_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
				video_codec_ctx_->sample_aspect_ratio = sample_aspect_ratio;

				if (tbb_avcodec_open(video_codec_ctx_.get(), encoder, &options_, true) < 0)
				{
					CASPAR_LOG(debug) << print() << L" Multithreaded avcodec_open2 failed";
					video_codec_ctx_->thread_count = 1;
					THROW_ON_ERROR2(avcodec_open2(video_codec_ctx_.get(), encoder, &options_), "[ffmpeg_consumer]");
				}

				AVStream * st = avformat_new_stream(format_context_.get(), NULL);
				if (!st)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate video-stream.") << boost::errinfo_api_function("avformat_new_stream"));

				THROW_ON_ERROR2(avcodec_parameters_from_context(st->codecpar, video_codec_ctx_.get()), "[ffmpeg_consumer]");

				st->time_base = video_codec_ctx_->time_base;
				st->avg_frame_rate = video_codec_ctx_->framerate;
				st->metadata = read_parameters(output_params_.video_metadata_);
				st->id = output_params_.video_stream_id_;

				picture_buf_.resize(av_image_get_buffer_size(video_codec_ctx_->pix_fmt, video_codec_ctx_->width, video_codec_ctx_->height, 16));

				return st;
			}

			AVStream * add_audio_stream(AVCodec * encoder, AVOutputFormat * format)
			{
				if (!encoder)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("codec not found") << boost::errinfo_api_function("avcodec_find_encoder"));

				audio_codec_ctx_ = AVCodecContextPtr(avcodec_alloc_context3(encoder), [](AVCodecContext * ctx) {avcodec_free_context(&ctx); });

				audio_codec_ctx_->refcounted_frames = 0;
				audio_codec_ctx_->codec_id = encoder->id;
				audio_codec_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
				audio_codec_ctx_->sample_rate = output_format_desc_.audio_sample_rate;
				audio_codec_ctx_->channels = 2;
				audio_codec_ctx_->channel_layout = av_get_default_channel_layout(audio_codec_ctx_->channels);
				audio_codec_ctx_->profile = FF_PROFILE_UNKNOWN;
				audio_codec_ctx_->sample_fmt = encoder->sample_fmts[0];
				if (encoder->id == AV_CODEC_ID_FLV1)
					audio_codec_ctx_->sample_rate = 44100;
				
				if (encoder->id == AV_CODEC_ID_AAC)
				{
					audio_codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
					audio_codec_ctx_->profile = FF_PROFILE_AAC_MAIN;
					audio_codec_ctx_->bit_rate = 160 * 1024;
				}
				if (output_params_.is_mxf_)
				{
					audio_codec_ctx_->channels = 4;
					audio_codec_ctx_->channel_layout = AV_CH_LAYOUT_4POINT0;
					audio_codec_ctx_->sample_fmt = AV_SAMPLE_FMT_S16;
					audio_codec_ctx_->bit_rate_tolerance = 0;
				}

				if (format->flags & AVFMT_GLOBALHEADER)
					audio_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

				if (output_params_.audio_bitrate_ != 0)
					audio_codec_ctx_->bit_rate = output_params_.audio_bitrate_ * 1000;

				audio_is_planar_ = av_sample_fmt_is_planar(audio_codec_ctx_->sample_fmt) != 0;

				THROW_ON_ERROR2(tbb_avcodec_open(audio_codec_ctx_.get(), encoder, &options_, true), "[ffmpeg_consumer]");

				auto st = avformat_new_stream(format_context_.get(), NULL);
				if (!st)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate audio-stream") << boost::errinfo_api_function("avformat_new_stream"));

				THROW_ON_ERROR2(avcodec_parameters_from_context(st->codecpar, audio_codec_ctx_.get()), "[ffmpeg_consumer]");

				st->metadata = read_parameters(output_params_.audio_metadata_);
				st->id = output_params_.audio_stream_id_;
				
				return st;
			}

			std::shared_ptr<AVFrame> convert_video(core::read_frame& frame)
			{

				std::unique_ptr<AVFrame, std::function<void(AVFrame *)>> in_frame(av_frame_alloc(), [](AVFrame *frame) { av_frame_free(&frame); });
				auto in_picture = reinterpret_cast<AVPicture*>(in_frame.get());

				if (key_only_)
				{
					key_picture_buf_.resize(frame.image_data().size());
					in_picture->linesize[0] = output_format_desc_.width * 4; //AV_PIX_FMT_BGRA
					in_picture->data[0] = key_picture_buf_.data();
					fast_memshfl(in_picture->data[0], frame.image_data().begin(), frame.image_data().size(), 0x0F0F0F0F, 0x0B0B0B0B, 0x07070707, 0x03030303);
				}
				else
				{
					avpicture_fill(in_picture, const_cast<uint8_t*>(frame.image_data().begin()), AV_PIX_FMT_BGRA, channel_format_desc_.width, channel_format_desc_.height);
				}

				std::shared_ptr<AVFrame> out_frame(av_frame_alloc(), [](AVFrame* frame) { av_frame_free(&frame); });

				av_image_fill_arrays(out_frame->data, out_frame->linesize, picture_buf_.data(), video_codec_ctx_->pix_fmt, video_codec_ctx_->width, video_codec_ctx_->height, 16);

				if (channel_format_desc_.field_mode == caspar::core::field_mode::progressive)
				{
					sws_scale(sws_.get(), in_frame->data, in_frame->linesize, 0, channel_format_desc_.height, out_frame->data, out_frame->linesize);
				}
				else
				{
					uint8_t * in_data_lower[4];
					int in_stride[4];
					uint8_t * out_data_upper[4];
					uint8_t * out_data_lower[4];
					int out_stride[4];
					for (uint32_t i = 0; i < 4; i++)
					{
						in_data_lower[i] = in_frame->data[i] + in_frame->linesize[i];
						out_data_upper[i] = out_frame->data[i] + ((is_imx50_pal_ && out_frame->data[i]) ? 32 * out_frame->linesize[i] : 0);
						out_data_lower[i] = out_frame->data[i] + (out_frame->data[i] ? ((is_imx50_pal_ ? 33 : 1) * out_frame->linesize[i]) : 0);
						in_stride[i] = in_frame->linesize[i] * 2;
						out_stride[i] = out_frame->linesize[i] * 2;
					}
					sws_scale(sws_.get(), in_frame->data, in_stride, 0, channel_format_desc_.height / 2, out_data_upper, out_stride);
					sws_scale(sws_.get(), in_data_lower, in_stride, 0, channel_format_desc_.height / 2, out_data_lower, out_stride);
				}
				out_frame->height = video_codec_ctx_->height;
				out_frame->width = video_codec_ctx_->width;
				out_frame->format = video_codec_ctx_->pix_fmt;
				out_frame->interlaced_frame = output_format_desc_.field_mode != core::field_mode::progressive;
				out_frame->top_field_first = output_format_desc_.field_mode == core::field_mode::upper;
				out_frame->pts = out_frame_number_++;
				return out_frame;
			}

			void encode_video_frame(core::read_frame& frame)
			{
				auto av_frame = convert_video(frame);
				AVPacket pkt = { 0 };
				av_init_packet(&pkt);
				int got_packet;
				THROW_ON_ERROR2(avcodec_encode_video2(video_codec_ctx_.get(), &pkt, av_frame.get(), &got_packet), "[video_encoder]");
				if (got_packet == 0)
					return;

				av_packet_rescale_ts(&pkt, video_codec_ctx_->time_base, video_stream_->time_base);
				pkt.stream_index = video_stream_->index;
				THROW_ON_ERROR2(av_interleaved_write_frame(format_context_.get(), &pkt), "[video_encoder]");
			}

			void resample_audio(core::read_frame& frame)
			{
				if (!swr_ || last_frame_no_channels_ != frame.num_channels())
				{
					uint64_t out_channel_layout = av_get_default_channel_layout(audio_codec_ctx_->channels);
					uint64_t in_channel_layout = create_channel_layout_bitmask(frame.num_channels());
					swr_ = SwrContextPtr(
						swr_alloc_set_opts(nullptr,
							out_channel_layout,
							audio_codec_ctx_->sample_fmt,
							audio_codec_ctx_->sample_rate,
							in_channel_layout,
							AV_SAMPLE_FMT_S32,
							channel_format_desc_.audio_sample_rate,
							0, nullptr),
						[](SwrContext * ctx) {swr_free(&ctx); });
					if (!swr_)
						BOOST_THROW_EXCEPTION(caspar_exception()
							<< msg_info("Cannot alloc audio resampler"));
					THROW_ON_ERROR2(swr_init(swr_.get()), "[audio_encoder]");
					last_frame_no_channels_ = frame.num_channels();
				}
				byte_vector out_buffers[AV_NUM_DATA_POINTERS];
				const int in_samples_count = frame.audio_data().size() / frame.num_channels();
				const int out_samples_count = static_cast<int>(av_rescale_rnd(in_samples_count, audio_codec_ctx_->sample_rate, channel_format_desc_.audio_sample_rate, AV_ROUND_UP));
				if (audio_is_planar_)
					for (char i = 0; i < audio_codec_ctx_->channels; i++)
						out_buffers[i].resize(out_samples_count * av_get_bytes_per_sample(AV_SAMPLE_FMT_S32));
				else
					out_buffers[0].resize(out_samples_count * av_get_bytes_per_sample(AV_SAMPLE_FMT_S32) *audio_codec_ctx_->channels);

				const uint8_t* in[] = { reinterpret_cast<const uint8_t*>(frame.audio_data().begin()) };
				uint8_t*       out[AV_NUM_DATA_POINTERS];
				for (char i = 0; i < AV_NUM_DATA_POINTERS; i++)
					out[i] = out_buffers[i].data();

				int converted_sample_count = swr_convert(swr_.get(),
					out, out_samples_count,
					in, in_samples_count);
				if (audio_is_planar_)
					for (char i = 0; i < audio_codec_ctx_->channels; i++)
					{
						out_buffers[i].resize(converted_sample_count * av_get_bytes_per_sample(audio_codec_ctx_->sample_fmt));
						boost::range::push_back(audio_bufers_[i], out_buffers[i]);
					}
				else
				{
					out_buffers[0].resize(converted_sample_count * av_get_bytes_per_sample(audio_codec_ctx_->sample_fmt) * audio_codec_ctx_->channels);
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

			void encode_audio_buffer(bool is_last_frame)
			{
				size_t input_audio_size = audio_codec_ctx_->frame_size == 0 || is_last_frame ?
					audio_bufers_[0].size() :
					audio_codec_ctx_->frame_size * av_get_bytes_per_sample(audio_codec_ctx_->sample_fmt) * audio_codec_ctx_->channels;
				if (!input_audio_size)
					return;
				int frame_size = input_audio_size / (av_get_bytes_per_sample(audio_codec_ctx_->sample_fmt) * audio_codec_ctx_->channels);
				while (audio_bufers_[0].size() >= input_audio_size)
				{
					AVPacket pkt = { 0 };
					av_init_packet(&pkt);
					std::unique_ptr<AVFrame, std::function<void(AVFrame *)>> in_frame(av_frame_alloc(), [](AVFrame *frame) { av_frame_free(&frame); });
					in_frame->nb_samples = frame_size;
					in_frame->pts = out_audio_sample_number_;
					out_audio_sample_number_ += frame_size;
					uint8_t* out_buffers[AV_NUM_DATA_POINTERS];
					for (char i = 0; i < AV_NUM_DATA_POINTERS; i++)
						out_buffers[i] = audio_bufers_[i].data();
					THROW_ON_ERROR2(avcodec_fill_audio_frame(in_frame.get(), audio_codec_ctx_->channels, audio_codec_ctx_->sample_fmt, (const uint8_t *)out_buffers[0], input_audio_size, 0), "[audio_encoder]");
					if (audio_is_planar_)
						for (char i = 0; i < audio_codec_ctx_->channels; i++)
							in_frame->data[i] = audio_bufers_[i].data();
					int got_packet;
					THROW_ON_ERROR2(avcodec_encode_audio2(audio_codec_ctx_.get(), &pkt, in_frame.get(), &got_packet), "[audio_encoder]");
					if (audio_is_planar_)
						for (char i = 0; i < audio_codec_ctx_->channels; i++)
							audio_bufers_[i].erase(audio_bufers_[i].begin(), audio_bufers_[i].begin() + (audio_codec_ctx_->frame_size * av_get_bytes_per_sample(audio_codec_ctx_->sample_fmt)));
					else
						audio_bufers_[0].erase(audio_bufers_[0].begin(), audio_bufers_[0].begin() + input_audio_size);
					if (!got_packet)
						return;
					av_packet_rescale_ts(&pkt, audio_codec_ctx_->time_base, audio_stream_->time_base);
					pkt.stream_index = audio_stream_->index;
					THROW_ON_ERROR2(av_interleaved_write_frame(format_context_.get(), &pkt), "[audio_encoder]");
				}
			}

			void encode_audio_frame(core::read_frame& frame)
			{
				resample_audio(frame);
				encode_audio_buffer(false);
			}

			void send(const safe_ptr<core::read_frame>& frame)
			{
				encode_executor_.begin_invoke([=] {
					frame_timer_.restart();

					encode_video_frame(*frame);

					if (!key_only_)
						encode_audio_frame(*frame);

					graph_->set_value("frame-time", frame_timer_.elapsed()*channel_format_desc_.fps*0.5);
					graph_->set_text(print());
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

			void flush_encoders()
			{
				encode_audio_buffer(true); // encode remaining buffer data
				bool audio_flushed = key_only_ || !audio_codec_ctx_ || (audio_codec_ctx_->codec->capabilities & AV_CODEC_CAP_DELAY) == 0;
				bool video_flushed = !video_codec_ctx_ || (video_codec_ctx_->codec->capabilities & AV_CODEC_CAP_DELAY) == 0;
				while (!(audio_flushed && video_flushed))
				{
					audio_flushed |= flush_stream(false);
					video_flushed |= flush_stream(true);
				} 
			}


			bool flush_stream(bool video)
			{
				std::shared_ptr<AVPacket> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });

				av_init_packet(pkt.get());
				
				auto stream = video ? video_stream_ : audio_stream_;
				if (!stream)
					return true;
				
				int got_packet;
				if (video)
					THROW_ON_ERROR2(avcodec_encode_video2(video_codec_ctx_.get(), pkt.get(), NULL, &got_packet), "[flush_video]");
				else
					THROW_ON_ERROR2(avcodec_encode_audio2(audio_codec_ctx_.get(), pkt.get(), NULL, &got_packet), "[flush_audio]");

				if (got_packet == 0)
					return true;

				if (pkt->pts != AV_NOPTS_VALUE)
					pkt->pts = av_rescale_q(pkt->pts, (video ? video_codec_ctx_ : audio_codec_ctx_)->time_base, stream->time_base);
				if (pkt->dts != AV_NOPTS_VALUE)
					pkt->dts = av_rescale_q(pkt->dts, (video ? video_codec_ctx_ : audio_codec_ctx_)->time_base, stream->time_base);

				pkt->stream_index = stream->index;
				THROW_ON_ERROR2(av_interleaved_write_frame(format_context_.get(), pkt.get()), "[flush_stream]");
				return false;
			}

		};

		struct ffmpeg_consumer_proxy : public core::frame_consumer
		{
			const output_params				output_params_;
			const int						index_;
			const bool						separate_key_;
			const int						tc_in_;
			const int						tc_out_;
			core::recorder* const			recorder_;
			bool							recording_;
			tbb::atomic<unsigned int>		frames_left_;
			std::unique_ptr<ffmpeg_consumer> consumer_;
			std::unique_ptr<ffmpeg_consumer> key_only_consumer_;
		public:

			ffmpeg_consumer_proxy(
				output_params output_params,
				const bool separate_key,
				core::recorder* const recorder = nullptr, 
				const int tc_in = 0, 
				const int tc_out = std::numeric_limits<int>().max(), 
				const unsigned int frame_limit = std::numeric_limits<unsigned int>().max())
				: output_params_(std::move(output_params))
				, separate_key_(separate_key)
				, index_(FFMPEG_CONSUMER_BASE_INDEX + crc16(boost::to_lower_copy(output_params.file_name_)))
				, tc_in_(tc_in)
				, tc_out_(tc_out)
				, recorder_(recorder)
				, recording_(tc_out == std::numeric_limits<int>().max())
			{
				frames_left_ = frame_limit;
			}

			virtual void initialize(const core::video_format_desc& format_desc, int)
			{
				consumer_.reset(new ffmpeg_consumer(
					format_desc,
					output_params_,
					false
				));
				if (separate_key_)
				{
					boost::filesystem::path fill_file(output_params_.file_name_);
					auto without_extension = fill_file.stem();
					auto key_file = narrow(env::media_folder()) + without_extension + "_A" + fill_file.extension();
					key_only_consumer_.reset(new ffmpeg_consumer(
						format_desc,
						output_params_,
						true
					));
				}
				else
					key_only_consumer_.reset();
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
					if (recorder_)
					{
						if (tc_out_ != std::numeric_limits<int>().max())
						{
							int timecode = frame->get_timecode();
							if (timecode == std::numeric_limits<int>().max())  // the frame does not contain timecode information
								timecode = recorder_->GetTimecode();
							if (!recording_ && timecode >= tc_in_)
								recording_ = true;
						}
						if (recording_)
						{
							if (frames_left_)
							{
								consumer_->send(frame);
								if (separate_key_)
									key_only_consumer_->send(frame);
							}
							else
								recording_ = false;
							recorder_->frame_captured(frames_left_--);
						}
					}
					else
					{
						consumer_->send(frame);
						if (separate_key_)
							key_only_consumer_->send(frame);
					}
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
				info.add(L"type", L"ffmpeg_consumer");
				info.add(L"filename", widen(output_params_.file_name_));
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
				return index_;
			}

			void set_frame_limit(unsigned int frame_limit)
			{
				frames_left_ = frame_limit;
			}

		};

		safe_ptr<core::frame_consumer> create_capture_consumer(const std::wstring filename, const core::parameters& params, const int tc_in, const int tc_out, bool narrow_aspect_ratio, core::recorder* const recorder)
		{
			auto acodec = params.get_original(L"ACODEC");
			auto vcodec = params.get_original(L"VCODEC");
			auto options = params.get_original(L"OPTIONS");
			auto output_metadata = params.get_original(L"OUTPUT_METADATA");
			auto audio_metadata = params.get_original(L"AUDIO_METADATA");
			auto video_metadata = params.get_original(L"VIDEO_METADATA");
			auto pixel_format = params.get_original(L"PIXEL_FORMAT");
			auto arate = params.get(L"ARATE", 0);
			auto vrate = params.get(L"VRATE", 0);
			auto audio_stream_id = params.get(L"AUDIO_STREAM_ID", 1);
			auto video_stream_id = params.get(L"VIDEO_STREAM_ID", 0);
			auto file_tc = params.get(L"IN", L"00:00:00:00");
			auto file_path_is_complete = boost::filesystem2::path(narrow(filename)).is_complete();
			auto format = core::video_format_desc::get(params.get(L"FORMAT", L"INVALID"));
			auto op = output_params(
				format,
				narrow(file_path_is_complete ? filename : env::media_folder() + filename),
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
				audio_stream_id,
				video_stream_id,
				narrow(options),
				narrow(pixel_format),
				false,
				narrow_aspect_ratio,
				arate,
				vrate,
				narrow(file_tc));
			return make_safe<ffmpeg_consumer_proxy>(op, false, recorder, tc_in, tc_out, static_cast<unsigned int>(tc_out - tc_in));
		}

		safe_ptr<core::frame_consumer> create_manual_record_consumer(const std::wstring filename, const core::parameters& params, const unsigned int frame_limit, bool narrow_aspect_ratio, core::recorder* const recorder)
		{
			auto acodec = params.get_original(L"ACODEC");
			auto vcodec = params.get_original(L"VCODEC");
			auto options = params.get_original(L"OPTIONS");
			auto output_metadata = params.get_original(L"OUTPUT_METADATA");
			auto audio_metadata = params.get_original(L"AUDIO_METADATA");
			auto video_metadata = params.get_original(L"VIDEO_METADATA");
			auto audio_stream_id = params.get(L"AUDIO_STREAM_ID", 1);
			auto video_stream_id = params.get(L"VIDEO_STREAM_ID", 0);
			auto pixel_format = params.get_original(L"PIXEL_FORMAT");
			auto arate = params.get(L"ARATE", 0);
			auto vrate = params.get(L"VRATE", 0);
			auto file_path_is_complete = boost::filesystem2::path(narrow(filename)).is_complete();
			auto format = core::video_format_desc::get(params.get(L"FORMAT", L"INVALID"));
			auto op = output_params(
				format,
				narrow(file_path_is_complete ? filename : env::media_folder() + filename),
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
				audio_stream_id,
				video_stream_id,
				narrow(options),
				narrow(pixel_format),
				false,
				narrow_aspect_ratio,
				arate,
				vrate,
				std::string("00:00:00:00"));
			return make_safe<ffmpeg_consumer_proxy>(op, false, recorder, 0, std::numeric_limits<int>().max(), frame_limit);
		}


		safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params)
		{
			if (params.size() < 1 || (params[0] != L"FILE" && params[0] != L"STREAM"))
				return core::frame_consumer::empty();
			auto filename = params.size() > 1 ? narrow(params.at_original(1)) : "";
			auto is_stream = params[0] == L"STREAM";
			auto file_path_is_complete = is_stream || boost::filesystem2::path(filename).is_complete();
			auto separate_key = params.has(L"SEPARATE_KEY");
			auto acodec = params.get_original(L"ACODEC");
			auto vcodec = params.get_original(L"VCODEC");
			auto options = params.get_original(L"OPTIONS");
			auto output_metadata = params.get_original(L"OUTPUT_METADATA");
			auto audio_metadata = params.get_original(L"AUDIO_METADATA");
			auto video_metadata = params.get_original(L"VIDEO_METADATA");
			auto audio_stream_id = params.get(L"AUDIO_STREAM_ID", 1);
			auto video_stream_id = params.get(L"VIDEO_STREAM_ID", 0);
			auto pixel_format = params.get_original(L"PIXEL_FORMAT");
			auto arate = params.get(L"ARATE", 0);
			auto vrate = params.get(L"VRATE", 0);
			auto narrow_aspect_ratio = params.get(L"NARROW", false);
			auto format = core::video_format_desc::get(params.get(L"FORMAT", L"INVALID"));
			output_params op(
				format,
				file_path_is_complete ? filename : narrow(env::media_folder()) + filename,
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
				audio_stream_id,
				video_stream_id,
				narrow(options),
				narrow(pixel_format),
				is_stream,
				narrow_aspect_ratio,
				arate,
				vrate,
				std::string("00:00:00:00"));
			return make_safe<ffmpeg_consumer_proxy>(op, separate_key);
		}

		safe_ptr<core::frame_consumer> create_consumer(const boost::property_tree::wptree& ptree)
		{
			auto filename = narrow(ptree.get<std::wstring>(L"path"));
			auto vcodec = ptree.get(L"vcodec", L"");
			auto acodec = ptree.get(L"acodec", L"");
			auto separate_key = ptree.get(L"separate-key", false);
			auto vrate = ptree.get(L"vrate", 0);
			auto arate = ptree.get(L"arate", 0);
			auto options = ptree.get(L"options", L"");
			auto pixel_format = ptree.get(L"pixel-format", L"");
			auto output_metadata = ptree.get(L"output-metadata", L"");
			auto audio_metadata = ptree.get(L"audio-metadata", L"");
			auto video_metadata = ptree.get(L"video-metadata", L"");
			auto audio_stream_id = ptree.get(L"audio_stream_id", 1);
			auto video_stream_id = ptree.get(L"video_stream_id", 0);
			auto format = core::video_format_desc::get(ptree.get(L"format", L"INVALID"));
			output_params op(
				format,
				filename,
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
				audio_stream_id,
				video_stream_id,
				narrow(options),
				narrow(pixel_format),
				true,
				ptree.get(L"narrow", false),
				arate,
				vrate,
				std::string("00:00:00:00"));
			return make_safe<ffmpeg_consumer_proxy>(op, separate_key);
		}

		void set_frame_limit(const safe_ptr<core::frame_consumer>& consumer, unsigned int frame_limit)
		{
			auto ffmpeg_consumer = dynamic_cast<ffmpeg_consumer_proxy*>(consumer.get());
			if (ffmpeg_consumer)
				ffmpeg_consumer->set_frame_limit(frame_limit);
		}

	}
}
