/*
* Copyright 2017 Telewizja Polska
*
* This file is part of TVP's CasparCG fork.
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
* Author: Jerzy Jaœkiewicz, jurek@tvp.pl based on Robert Nagy, ronag89@gmail.com work
*/

#include "ndi_consumer.h"
#include "../util/ndi_util.h"
#include "../ndi.h"

#include <common/exception/win32_exception.h>
#include <common/env.h>
#include <common/concurrency/future_util.h>
#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>
#include <common/memory/memclr.h>
#include <common/memory/memcpy.h>

#include <core/parameters/parameters.h>
#include <core/consumer/frame_consumer.h>
#include <core/mixer/read_frame.h>
#include <core/video_format.h>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread.hpp>
#include <boost/timer.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/crc.hpp>
extern "C"
{
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar {
	namespace ndi {

		int crc16(const std::string& str)
		{
			boost::crc_16_type result;
			result.process_bytes(str.data(), str.length());
			return result.checksum();
		}

		NDIlib_send_instance_t create_ndi_send(const NDIlib_v2* ndi_lib, const std::string ndi_name, const std::string groups)
		{
			if (!ndi_lib)
				BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(" NDI library not available"));
			NDIlib_send_create_t NDI_send_create_desc = { ndi_name.c_str(), groups.c_str(), true, false };
			return ndi_lib->NDIlib_send_create(&NDI_send_create_desc);
		}

		SwrContext* create_swr(const core::video_format_desc format_desc, const core::channel_layout out_layout, const int input_channel_count)
		{
			const auto ALL_63_CHANNELS = 0x7FFFFFFFFFFFFFFFULL;
			auto to_shift = 63 - input_channel_count;
			auto in_channel_layout = ALL_63_CHANNELS >> to_shift;
			to_shift = 63 - out_layout.num_channels;
			auto out_channel_layout = ALL_63_CHANNELS >> to_shift;
			auto swr = swr_alloc_set_opts(nullptr,
				out_channel_layout,
				AV_SAMPLE_FMT_FLT,
				format_desc.audio_sample_rate,
				in_channel_layout,
				AV_SAMPLE_FMT_S32,
				format_desc.audio_sample_rate,
				0, nullptr);
			if (!swr)
				BOOST_THROW_EXCEPTION(caspar_exception()
					<< msg_info("Cannot alloc audio resampler"));
			if (swr_init(swr) < 0)
				BOOST_THROW_EXCEPTION(caspar_exception()
					<< msg_info("Cannot initialize audio resampler"));
			return swr;
		}

		struct ndi_consumer : public boost::noncopyable
		{
			const core::video_format_desc									format_desc_;
			const core::channel_layout										channel_layout_;
			const std::wstring												ndi_name_;
			const bool														is_alpha_;
			const bool														is_blocking_;
			const NDIlib_v2*												ndi_lib_;
			const NDIlib_send_instance_t									ndi_send_;
			std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>>     send_frame_buffer_;
			executor														executor_;
			std::unique_ptr<SwrContext, std::function<void(SwrContext*)>>	swr_;
			std::unique_ptr<SwsContext, std::function<void(SwsContext*)>>	sws_;
			safe_ptr<diagnostics::graph>									graph_;
			tbb::atomic<int64_t>											current_encoding_delay_;
			boost::timer													audio_send_timer_;
			boost::timer													video_send_timer_;
			boost::timer													tick_timer_;
			boost::timer													frame_convert_timer_;

		public:

			// frame_consumer

			ndi_consumer(const core::video_format_desc& format_desc, const core::channel_layout& channel_layout, const std::string& ndi_name, const std::string& groups, const bool is_alpha, const bool is_blocking)
				: channel_layout_(channel_layout)
				, format_desc_(format_desc)
				, ndi_name_(widen(ndi_name))
				, is_alpha_(is_alpha)
				, is_blocking_(is_blocking)
				, ndi_lib_(load_ndi())
				, ndi_send_(create_ndi_send(ndi_lib_, ndi_name, groups))
				, executor_(print())
				, swr_(nullptr, [](SwrContext* ctx) -> void { if (ctx) swr_free(&ctx); })
				, sws_(nullptr, [](SwsContext* ctx) -> void{ sws_freeContext(ctx); })
			{
				current_encoding_delay_ = 0;
				executor_.set_capacity(1);
				graph_->set_text(print());
				graph_->set_color("audio-send-time", diagnostics::color(0.5f, 1.0f, 0.1f));
				graph_->set_color("video-send-time", diagnostics::color(1.0f, 1.0f, 0.1f));
				graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
				graph_->set_color("dropped-frame", diagnostics::color(1.0f, 0.1f, 0.1f));
				if (!is_alpha)
					graph_->set_color("frame-convert-time", diagnostics::color(0.8f, 0.6f, 0.9f));
				diagnostics::register_graph(graph_);
			}

			~ndi_consumer()
			{
				executor_.stop();
				executor_.join();
				if (ndi_send_)
					ndi_lib_->NDIlib_send_destroy(ndi_send_);
				CASPAR_LOG(info) << print() << L" Successfully Uninitialized.";
			}

			bool do_send(const safe_ptr<core::read_frame>& frame)
			{
				send_video(frame);
				audio_send_timer_.restart();
				send_audio(frame);
				graph_->set_value("audio-send-time", audio_send_timer_.elapsed() * format_desc_.fps * 0.5f);
				current_encoding_delay_ = frame->get_age_millis();
				graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5f);
				tick_timer_.restart();
				return true;
			}
						
			boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame)
			{
				if (is_blocking_)
					return executor_.begin_invoke([this, frame]() -> bool { return do_send(frame); });
				else
				{
					if (executor_.is_running() && executor_.empty())
						executor_.begin_invoke([this, frame]() {
						do_send(frame);
					});
					else
					{
						CASPAR_LOG(warning) << print() << L" Frame dropped.";
						graph_->set_tag("dropped-frame");
					}
					return caspar::wrap_as_future(true);
				}
			}

			void send_video(const safe_ptr<core::read_frame>& frame)
			{
				std::unique_ptr<NDIlib_video_frame_t> ndi_frame(create_video_frame(format_desc_, is_alpha_));
				if (is_alpha_)
				{
					video_send_timer_.restart();
					ndi_frame->p_data = const_cast<uint8_t*>(frame->image_data().begin());
				}
				else  //colorspace conversion
				{
					frame_convert_timer_.restart();
					if (!sws_)
					{
						sws_.reset(sws_getContext(format_desc_.width, format_desc_.height, AV_PIX_FMT_BGRA, format_desc_.width, format_desc_.height, AV_PIX_FMT_UYVY422, SWS_POINT, NULL, NULL, NULL));
						send_frame_buffer_.resize(av_image_get_buffer_size(AV_PIX_FMT_BGRA, format_desc_.width, format_desc_.height, 16));
					}
					uint8_t *src_data[4];
					int src_linesize[4];
					uint8_t *dest_data[4];
					int dst_linesize[4];
					av_image_fill_arrays(src_data, src_linesize, frame->image_data().begin(), AV_PIX_FMT_BGRA, format_desc_.width, format_desc_.height, 1);
					av_image_fill_arrays(dest_data, dst_linesize, &send_frame_buffer_.front(), AV_PIX_FMT_UYVY422, format_desc_.width, format_desc_.height, 16);
					sws_scale(sws_.get(), src_data, src_linesize, 0, format_desc_.height, dest_data, dst_linesize);
					graph_->set_value("frame-convert-time", frame_convert_timer_.elapsed() * format_desc_.fps * 0.5f);
					ndi_frame->p_data = &send_frame_buffer_.front();
					video_send_timer_.restart();
				}
				ndi_lib_->NDIlib_send_send_video(ndi_send_, ndi_frame.get());
				graph_->set_value("video-send-time", video_send_timer_.elapsed() * format_desc_.fps * 0.5f);
			}

			void send_audio(const safe_ptr<core::read_frame>& frame)
			{
				if (!swr_)
					swr_.reset(create_swr(format_desc_, channel_layout_, frame->num_channels()));
				auto audio_frame = create_audio_frame(channel_layout_, frame->multichannel_view().num_samples(), format_desc_.audio_sample_rate);
				const uint8_t* in[] = { reinterpret_cast<const uint8_t*>(frame->audio_data().begin()) };
				int converted_sample_count = swr_convert(swr_.get(),
					reinterpret_cast<uint8_t**>(&audio_frame->p_data), audio_frame->no_samples,
					in, frame->multichannel_view().num_samples());
				if (converted_sample_count != audio_frame->no_samples)
					CASPAR_LOG(warning) << print() << L" Not all samples were converted (" << converted_sample_count << L" of " << audio_frame->no_samples << L").";
				ndi_lib_->NDIlib_util_send_send_audio_interleaved_32f(ndi_send_, audio_frame.get());
			}

			std::wstring print() const
			{
				return L"NewTek NDI[" + ndi_name_ + L"]";
			}
		};


		struct ndi_consumer_proxy : public core::frame_consumer
		{
			const int								index_;
			std::unique_ptr<ndi_consumer>			consumer_;
			const core::channel_layout				channel_layout_;
			const std::string						ndi_name_;
			const std::string						groups_;
			const bool								is_alpha_;
			const bool								is_blocking_;

		public:

			ndi_consumer_proxy(core::channel_layout channel_layout, const std::string& ndi_name, const std::string& groups, const bool is_alpha, const bool is_blocking)
				: index_(NDI_CONSUMER_BASE_INDEX + crc16(ndi_name))
				, channel_layout_(channel_layout)
				, ndi_name_(ndi_name)
				, groups_(groups)
				, is_alpha_(is_alpha)
				, is_blocking_(is_blocking)
			{	}

			virtual void initialize(const core::video_format_desc& format_desc, int) override
			{
				consumer_.reset(new ndi_consumer(format_desc, channel_layout_, ndi_name_, groups_, is_alpha_, is_blocking_));
			}

			virtual bool has_synchronization_clock() const override
			{
				return consumer_ && is_blocking_;
			}

			virtual size_t buffer_depth() const override
			{
				return 1;
			}

			virtual int64_t presentation_frame_age_millis() const override
			{
				return consumer_ ? consumer_->current_encoding_delay_ : 0;
			}

			virtual boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame) override
			{
				if (consumer_)
					return consumer_->send(frame);
				return wrap_as_future(true);
			}

			virtual std::wstring print() const override
			{
				return consumer_ ? consumer_->print() : L"NewTel NDI[" + widen(ndi_name_) + L" (not initialized)]";
			}

			virtual boost::property_tree::wptree info() const override
			{
				boost::property_tree::wptree info;
				info.add(L"type", L"ndi-consumer");
				info.add(L"name", widen(ndi_name_));
				return info;
			}

			virtual int index() const override
			{
				return index_;
			}

		};


		safe_ptr<core::frame_consumer> create_ndi_consumer(const core::parameters& params)
		{
			if (params.size() < 1 || params[0] != L"NDI")
				return core::frame_consumer::empty();

			std::string ndi_name("default");
			if (params.size() > 1)
				ndi_name = narrow(params.at(1));
			std::string groups = narrow(params.get(L"GROUPS", L""));
			bool is_alpha = params.get(L"ALPHA", true);
			bool is_blocking = params.get(L"BLOCKING", false);
			auto channel_layout = core::default_channel_layout_repository()
				.get_by_name(
					params.get(L"CHANNEL_LAYOUT", L"STEREO"));
			return make_safe<ndi_consumer_proxy>(channel_layout, ndi_name, groups, is_alpha, is_blocking);
		}

		safe_ptr<core::frame_consumer> create_ndi_consumer(const boost::property_tree::wptree& ptree)
		{
			auto ndi_name = narrow(ptree.get(L"name", L"default"));
			auto groups = narrow(ptree.get(L"groups", L""));
			bool is_alpha = ptree.get(L"alpha", true);
			bool is_blocking = ptree.get(L"blocking", false);
			auto channel_layout =
				core::default_channel_layout_repository()
				.get_by_name(
					boost::to_upper_copy(ptree.get(L"channel-layout", L"STEREO")));
			return make_safe<ndi_consumer_proxy>(channel_layout, ndi_name, groups, is_alpha, is_blocking);
		}

	}
}