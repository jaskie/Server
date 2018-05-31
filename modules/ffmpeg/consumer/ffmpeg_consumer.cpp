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
			const std::string							file_name_;
			const std::string							video_codec_;
			const std::string							audio_codec_;
			const std::string							output_metadata_;
			const std::string							audio_metadata_;
			const std::string							video_metadata_;
			const std::string							options_;
			const std::string							pixel_format_;
			const bool									is_mxf_;
			const bool									is_narrow_;
			const bool									is_stream_;
			const int									audio_bitrate_;
			const int									video_bitrate_;
			const std::string							file_timecode_;
			
			output_params(
				std::string filename, 
				std::string audio_codec, 
				std::string video_codec, 
				std::string output_metadata,
				std::string audio_metadata,
				std::string video_metadata,
				std::string options,
				std::string pixel_format,
				const bool is_stream,
				const bool is_narrow, 
				const int a_rate, 
				const int v_rate, 
				std::string file_tc)
				: video_codec_(std::move(video_codec))
				, audio_codec_(std::move(audio_codec))
				, output_metadata_(std::move(output_metadata))
				, audio_metadata_(std::move(audio_metadata))
				, video_metadata_(std::move(video_metadata))
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
		
		struct ffmpeg_consumer : boost::noncopyable
		{
			AVDictionary *							options_;
			const output_params						output_params_;
			const core::video_format_desc			format_desc_;

			const safe_ptr<diagnostics::graph>		graph_;

			std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext *)>>	format_context_;
			std::unique_ptr<AVStream, std::function<void(AVStream  *)>>					audio_st_;
			std::unique_ptr<AVStream, std::function<void(AVStream  *)>>					video_st_;

			std::unique_ptr<SwrContext, std::function<void(SwrContext *)>>				swr_;
			std::unique_ptr<SwsContext, std::function<void(SwsContext *)>>				sws_;

			byte_vector								audio_bufers_[AV_NUM_DATA_POINTERS];
			byte_vector								key_picture_buf_;
			byte_vector								picture_buf_;

			tbb::atomic<int64_t>					out_frame_number_;
			int64_t									out_audio_sample_number_;

			bool									key_only_;
			bool									audio_is_planar;
			tbb::atomic<int64_t>					current_encoding_delay_;
			boost::timer							frame_timer_;
			executor								encode_executor_;

		public:
			ffmpeg_consumer
			(
				const core::video_format_desc& format_desc,
				output_params params,
				bool key_only
			)
				: encode_executor_(print())
				, out_audio_sample_number_(0)
				, output_params_(std::move(params))
				, format_desc_(format_desc)
				, key_only_(key_only)
				, options_(read_parameters(params.options_))
			{
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
						format = av_guess_format("mpegts", NULL, NULL);
					if (!format && output_params_.is_mxf_ && format_desc.format == core::video_format::pal)
						format = av_guess_format("mxf_d10", output_params_.file_name_.c_str(), NULL);
					if (!format)
						format = av_guess_format(NULL, output_params_.file_name_.c_str(), NULL);
					if (!format)
						BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not guess output format."));
					AVCodec * video_codec = NULL;
					AVCodec * audio_codec = NULL;
					if (!output_params_.video_codec_.empty())
						video_codec = avcodec_find_encoder_by_name(output_params_.video_codec_.c_str());
					if (!output_params_.audio_codec_.empty())
						audio_codec = avcodec_find_encoder_by_name(output_params_.audio_codec_.c_str());
					if (!video_codec)
						video_codec = avcodec_find_encoder(format->video_codec);
					if (!audio_codec)
						audio_codec = avcodec_find_encoder(format->audio_codec);
					if (!video_codec)
						video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
					if (!audio_codec)
						audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);

					format_context_ = std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext *)>>(alloc_output_params_context(output_params_.file_name_, format), ([](AVFormatContext * ctx)
					{
						if (!(ctx->oformat->flags & AVFMT_NOFILE))
							LOG_ON_ERROR2(avio_close(ctx->pb), "[ffmpeg_consumer]"); // Close the output ffmpeg.
						avformat_free_context(ctx);
					}));

					//  Add the audio and video streams using the default format codecs	and initialize the codecs.


					auto stream_deleter = [](AVStream * stream) {
						avcodec_free_context(&stream->codec);
					};

					video_st_ = std::unique_ptr<AVStream, std::function<void(AVStream *)>>(add_video_stream(video_codec, format), stream_deleter);
					if (!key_only_ && audio_codec)
						audio_st_ = std::unique_ptr<AVStream, std::function<void(AVStream *)>>(add_audio_stream(audio_codec, format), stream_deleter);

					LOG_ON_ERROR2(av_dict_set(&video_st_->metadata, "timecode", output_params_.file_timecode_.c_str(), AV_DICT_DONT_OVERWRITE), "[ffmpeg_consumer]");

					av_dump_format(format_context_.get(), 0, output_params_.file_name_.c_str(), 1);
					
					// Open the output

					if (!(format_context_->oformat->flags & AVFMT_NOFILE))
						THROW_ON_ERROR2(avio_open2(&format_context_->pb, output_params_.file_name_.c_str(), AVIO_FLAG_WRITE | AVIO_FLAG_NONBLOCK, NULL, &options_), "[ffmpeg_consumer]");
					format_context_->metadata = read_parameters(params.output_metadata_);

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
					if ((video_st_ && (video_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY))
						|| (!key_only_ && (audio_st_ && (audio_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY))))
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

			AVStream* add_video_stream(AVCodec * encoder, AVOutputFormat * format)
			{

				if (!encoder)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Codec not found."));

				AVStream * st = avformat_new_stream(format_context_.get(), encoder);
				if (!st)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate video-stream.") << boost::errinfo_api_function("avformat_new_stream"));

				st->id = 0;
				st->time_base = av_make_q(format_desc_.duration, format_desc_.time_scale);
				st->metadata = read_parameters(output_params_.video_metadata_);
				AVCodecContext * c = st->codec;

				c->refcounted_frames = 0;
				c->codec_id = encoder->id;
				c->codec_type = AVMEDIA_TYPE_VIDEO;
				c->width = format_desc_.width;
				c->height = format_desc_.height;
				c->gop_size = 25;
				c->time_base = st->time_base;
				c->flags |= format_desc_.field_mode == core::field_mode::progressive ? 0 : (AV_CODEC_FLAG_INTERLACED_ME | AV_CODEC_FLAG_INTERLACED_DCT);
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
					c->bit_rate = format_desc_.height * 14 * 1000; // about 8Mbps for SD, 14 for HD
					LOG_ON_ERROR2(av_opt_set(c->priv_data, "preset", "veryfast", NULL), "[ffmpeg_consumer]");
				}
				else if (c->codec_id == AV_CODEC_ID_QTRLE)
				{
					c->pix_fmt = AV_PIX_FMT_ARGB;
				}
				else if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
				{
					if (output_params_.is_mxf_)
					{
						c->pix_fmt = AV_PIX_FMT_YUV422P;
						c->bit_rate = 50 * 1000000;
						if (format_desc_.format == core::video_format::pal)
						{
							// IMX50 encoding parameters
							c->bit_rate = 50 * 1000000;
							c->height = 608;
							c->rc_max_rate = c->bit_rate;
							c->rc_min_rate = c->bit_rate;
							c->rc_buffer_size = 2000000;
							c->rc_initial_buffer_occupancy = 2000000;
							//c->rc_buffer_aggressivity = 0.25; TODO: amend this
							c->gop_size = 1;
						}
					}
				}

				if (output_params_.video_bitrate_ != 0)
					c->bit_rate = output_params_.video_bitrate_ * 1024;
				if (!output_params_.pixel_format_.empty())
					c->pix_fmt = av_get_pix_fmt(output_params_.pixel_format_.c_str());

				c->max_b_frames = 0; // b-frames not supported.

				AVRational sample_aspect_ratio;
				switch (format_desc_.format) {
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
					c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
				c->sample_aspect_ratio = sample_aspect_ratio;

				if (tbb_avcodec_open(c, encoder, &options_, true) < 0)
				{
					CASPAR_LOG(debug) << print() << L" Multithreaded avcodec_open2 failed";
					c->thread_count = 1;
					THROW_ON_ERROR2(avcodec_open2(c, encoder, &options_), "[ffmpeg_consumer]");
				}

				picture_buf_.resize(av_image_get_buffer_size(c->pix_fmt, c->width, c->height, 16));

				return st;
			}

			AVStream * add_audio_stream(AVCodec * encoder, AVOutputFormat * format)
			{
				if (!encoder)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("codec not found") << boost::errinfo_api_function("avcodec_find_encoder"));

				auto st = avformat_new_stream(format_context_.get(), encoder);
				if (!st)
					BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate audio-stream") << boost::errinfo_api_function("avformat_new_stream"));
				st->id = 1;
				st->metadata = read_parameters(output_params_.audio_metadata_);

				AVCodecContext * c = st->codec;
				c->refcounted_frames = 0;
				c->codec_id = encoder->id;
				c->codec_type = AVMEDIA_TYPE_AUDIO;
				c->sample_rate = format_desc_.audio_sample_rate;
				c->channels = 2;
				c->channel_layout = av_get_default_channel_layout(c->channels);
				c->profile = FF_PROFILE_UNKNOWN;
				c->sample_fmt = encoder->sample_fmts[0];
				if (encoder->id == AV_CODEC_ID_FLV1)
					c->sample_rate = 44100;

				if (encoder->id == AV_CODEC_ID_AAC)
				{
					c->sample_fmt = AV_SAMPLE_FMT_FLTP;
					c->profile = FF_PROFILE_AAC_MAIN;
					c->bit_rate = 160 * 1024;
				}
				if (output_params_.is_mxf_)
				{
					c->channels = 4;
					c->channel_layout = AV_CH_LAYOUT_4POINT0;
					c->sample_fmt = AV_SAMPLE_FMT_S16;
					c->bit_rate_tolerance = 0;
				}

				if (format->flags & AVFMT_GLOBALHEADER)
					c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

				if (output_params_.audio_bitrate_ != 0)
					c->bit_rate = output_params_.audio_bitrate_ * 1024;

				audio_is_planar = av_sample_fmt_is_planar(c->sample_fmt) != 0;

				THROW_ON_ERROR2(tbb_avcodec_open(c, encoder, &options_, true), "[ffmpeg_consumer]");

				return st;
			}

			std::shared_ptr<AVFrame> convert_video(core::read_frame& frame, AVCodecContext* c)
			{
				if (!sws_)
				{
					sws_ = std::unique_ptr<SwsContext, std::function<void(SwsContext *)>>(
						sws_getContext(format_desc_.width, format_desc_.height, AV_PIX_FMT_BGRA, format_desc_.width, format_desc_.height, c->pix_fmt, 0, nullptr, nullptr, NULL),
						[](SwsContext * ctx) { sws_freeContext(ctx); });
					if (!sws_)
						BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Cannot initialize the conversion context"));
				}
				std::unique_ptr<AVFrame, std::function<void(AVFrame *)>> in_frame(av_frame_alloc(), [](AVFrame *frame) { av_frame_free(&frame); });
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

				std::shared_ptr<AVFrame> out_frame(av_frame_alloc(), [](AVFrame* frame) { av_frame_free(&frame); });
				bool is_imx50_pal = output_params_.is_mxf_ && format_desc_.format == core::video_format::pal;

				av_image_fill_arrays(out_frame->data, out_frame->linesize, picture_buf_.data(), c->pix_fmt, c->width, c->height, 16);

				uint8_t *out_data[AV_NUM_DATA_POINTERS];
				for (uint32_t i = 0; i < AV_NUM_DATA_POINTERS; i++)
					out_data[i] = reinterpret_cast<uint8_t*>( out_frame->data[i] + ((is_imx50_pal && out_frame->data[i]) ? 32 * out_frame->linesize[i] : 0));
				sws_scale(sws_.get(), in_frame->data, in_frame->linesize, 0, format_desc_.height, out_data, out_frame->linesize);
				out_frame->height = c->height;
				out_frame->width = c->width;
				out_frame->format = c->pix_fmt;

				return out_frame;
			}

			void encode_video_frame(core::read_frame& frame)
			{
				AVCodecContext * codec_context = video_st_->codec;

				auto av_frame = convert_video(frame, codec_context);
				av_frame->interlaced_frame = format_desc_.field_mode != core::field_mode::progressive;
				av_frame->top_field_first = format_desc_.field_mode == core::field_mode::upper;
				av_frame->pts = out_frame_number_++;

				std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });
				av_init_packet(pkt.get());
				int got_packet;

				THROW_ON_ERROR2(avcodec_encode_video2(codec_context, pkt.get(), av_frame.get(), &got_packet), "[video_encoder]");

				if (got_packet == 0)
					return;

				if (pkt->pts != AV_NOPTS_VALUE)
					pkt->pts = av_rescale_q(pkt->pts, codec_context->time_base, video_st_->time_base);
				if (pkt->dts != AV_NOPTS_VALUE)
					pkt->dts = av_rescale_q(pkt->dts, codec_context->time_base, video_st_->time_base);

				if (codec_context->coded_frame->key_frame)
					pkt->flags |= AV_PKT_FLAG_KEY;

				pkt->stream_index = video_st_->index;
				THROW_ON_ERROR2(av_interleaved_write_frame(format_context_.get(), pkt.get()), "[video_encoder]");
			}

			void resample_audio(core::read_frame& frame, AVCodecContext* ctx)
			{
				if (!swr_)
				{
					uint64_t out_channel_layout = av_get_default_channel_layout(ctx->channels);
					uint64_t in_channel_layout = create_channel_layout_bitmask(frame.num_channels());
					swr_ = std::unique_ptr<SwrContext, std::function<void(SwrContext *)>>(
						swr_alloc_set_opts(nullptr,
							out_channel_layout,
							ctx->sample_fmt,
							ctx->sample_rate,
							in_channel_layout,
							AV_SAMPLE_FMT_S32,
							format_desc_.audio_sample_rate,
							0, nullptr),
						[](SwrContext * ctx) {swr_free(&ctx); });
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

			void encode_audio_buffer(bool is_last_frame)
			{
				AVCodecContext * enc = audio_st_->codec;
				size_t input_audio_size = enc->frame_size == 0 || is_last_frame ?
					audio_bufers_[0].size() :
					enc->frame_size * av_get_bytes_per_sample(enc->sample_fmt) * enc->channels;
				if (!input_audio_size)
					return;
				int frame_size = input_audio_size / (av_get_bytes_per_sample(enc->sample_fmt) * enc->channels);
				while (audio_bufers_[0].size() >= input_audio_size)
				{
					std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> pkt(av_packet_alloc(), [](AVPacket *p) { av_packet_free(&p); });
					std::unique_ptr<AVFrame, std::function<void(AVFrame *)>> in_frame(av_frame_alloc(), [](AVFrame *frame) { av_frame_free(&frame); });
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
					if (pkt->pts != AV_NOPTS_VALUE)
						pkt->pts = av_rescale_q(pkt->pts, enc->time_base, audio_st_->time_base);
					if (pkt->dts != AV_NOPTS_VALUE)
						pkt->dts = av_rescale_q(pkt->dts, enc->time_base, audio_st_->time_base);
					pkt->stream_index = audio_st_->index;
					THROW_ON_ERROR2(av_interleaved_write_frame(format_context_.get(), pkt.get()), "[audio_encoder]");
				}
			}

			void encode_audio_frame(core::read_frame& frame)
			{
				resample_audio(frame, audio_st_->codec);
				encode_audio_buffer(false);
			}

			void send(const safe_ptr<core::read_frame>& frame)
			{
				encode_executor_.begin_invoke([=] {
					frame_timer_.restart();

					encode_video_frame(*frame);

					if (!key_only_)
						encode_audio_frame(*frame);

					graph_->set_value("frame-time", frame_timer_.elapsed()*format_desc_.fps*0.5);
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
				bool audio_flushed = key_only_ || !audio_st_ || (audio_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY) == 0;
				bool video_flushed = !video_st_ || (video_st_->codec->codec->capabilities & AV_CODEC_CAP_DELAY) == 0;
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
				auto stream = video ? video_st_.get() : audio_st_.get();
				int got_packet;
				if (video)
					THROW_ON_ERROR2(avcodec_encode_video2(stream->codec, pkt.get(), NULL, &got_packet), "[flush_video]");
				else
					THROW_ON_ERROR2(avcodec_encode_audio2(stream->codec, pkt.get(), NULL, &got_packet), "[flush_audio]");

				if (got_packet == 0)
					return true;

				if (pkt->pts != AV_NOPTS_VALUE)
					pkt->pts = av_rescale_q(pkt->pts, stream->codec->time_base, stream->time_base);
				if (pkt->dts != AV_NOPTS_VALUE)
					pkt->dts = av_rescale_q(pkt->dts, stream->codec->time_base, stream->time_base);

				if (video && stream->codec->coded_frame->key_frame)
					pkt->flags |= AV_PKT_FLAG_KEY;

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
			auto file_tc = params.get(L"IN", L"00:00:00:00");
			auto file_path_is_complete = boost::filesystem2::path(narrow(filename)).is_complete();
			auto op = output_params(
				narrow(file_path_is_complete ? filename : env::media_folder() + filename),
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
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
			auto pixel_format = params.get_original(L"PIXEL_FORMAT");
			auto arate = params.get(L"ARATE", 0);
			auto vrate = params.get(L"VRATE", 0);
			auto file_path_is_complete = boost::filesystem2::path(narrow(filename)).is_complete();
			auto op = output_params(
				narrow(file_path_is_complete ? filename : env::media_folder() + filename),
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
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
			auto file_path_is_complete = boost::filesystem2::path(filename).is_complete();
			auto separate_key = params.has(L"SEPARATE_KEY");
			auto is_stream = params[0] == L"STREAM";
			auto acodec = params.get_original(L"ACODEC");
			auto vcodec = params.get_original(L"VCODEC");
			auto options = params.get_original(L"OPTIONS");
			auto output_metadata = params.get_original(L"OUTPUT_METADATA");
			auto audio_metadata = params.get_original(L"AUDIO_METADATA");
			auto video_metadata = params.get_original(L"VIDEO_METADATA");
			auto pixel_format = params.get_original(L"PIXEL_FORMAT");
			auto arate = params.get(L"ARATE", 0);
			auto vrate = params.get(L"VRATE", 0);
			auto narrow_aspect_ratio = params.get(L"NARROW", false);
			output_params op(
				file_path_is_complete ? filename : narrow(env::media_folder()) + filename,
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
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
			bool file_path_is_complete = boost::filesystem2::path(filename).is_complete();
			auto vcodec = ptree.get(L"vcodec", L"libx264");
			auto acodec = ptree.get(L"acodec", L"aac");
			auto separate_key = ptree.get(L"separate-key", false);
			auto vrate = ptree.get(L"vrate", 0);
			auto arate = ptree.get(L"arate", 0);
			auto options = ptree.get(L"options", L"");
			auto pixel_format = ptree.get(L"pixel-format", L"");
			auto output_metadata = ptree.get(L"output-metadata", L"");
			auto audio_metadata = ptree.get(L"audio-metadata", L"");
			auto video_metadata = ptree.get(L"video-metadata", L"");
			output_params op(
				file_path_is_complete ? filename : narrow(env::media_folder()) + filename,
				narrow(acodec),
				narrow(vcodec),
				narrow(output_metadata),
				narrow(audio_metadata),
				narrow(video_metadata),
				narrow(options),
				narrow(pixel_format),
				true,
				!ptree.get(L"narrow", false),
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
