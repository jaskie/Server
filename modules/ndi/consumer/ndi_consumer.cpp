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
#include <common/exception/exceptions.h>
#include <common/env.h>
#include <common/log/log.h>
#include <common/utility/string.h>
#include <common/concurrency/future_util.h>
#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>
#include <common/memory/memclr.h>
#include <common/memory/memcpy.h>

#include <core/parameters/parameters.h>
#include <core/consumer/frame_consumer.h>
#include <core/video_format.h>
#include <core/mixer/read_frame.h>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread.hpp>
#include <boost/timer.hpp>

#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#include <boost/crc.hpp>
#pragma warning(pop)

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C"
{
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

#include <Processing.NDI.Lib.h>

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

		SwsContext* create_sws(const core::video_format_desc format_desc, const int width, const int height)
		{
			return sws_getContext(format_desc.width, format_desc.height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_UYVY422, SWS_BICUBIC, nullptr, nullptr, NULL);
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

		struct ndi_consumer : public core::frame_consumer
		{
			core::video_format_desc				format_desc_;
			core::channel_layout				channel_layout_;
			std::wstring						ndi_name_;
			const int							index_;
			const NDIlib_v2*					p_ndi_lib_;
			const NDIlib_send_instance_t		p_ndi_send_;
			executor							executor_;
			SwrContext *						swr_;
			SwsContext *						sws_;
			safe_ptr<diagnostics::graph>		graph_;
			const double						scale_;
			int									width_;
			int									height_;
			boost::timer						frame_timer_;
			boost::timer						tick_timer_;
			boost::timer						send_timer_;
			boost::timer						scale_timer_;

		public:

			// frame_consumer

			ndi_consumer(core::channel_layout channel_layout, const std::string& ndi_name, const std::string groups, const double scale)
				: channel_layout_(channel_layout)
				, ndi_name_(widen(ndi_name))
				, index_(NDI_CONSUMER_BASE_INDEX + crc16(ndi_name))
				, p_ndi_lib_(load_ndi())
				, p_ndi_send_(create_ndi_send(p_ndi_lib_, ndi_name, groups))
				, executor_(print())
				, sws_(nullptr)
				, swr_(nullptr)
				, scale_(scale)
				, width_(0)
				, height_(0)
			{
				executor_.set_capacity(1);
				graph_->set_text(print());
				graph_->set_color("frame-time", diagnostics::color(0.5f, 1.0f, 0.2f));
				graph_->set_color("send-time", diagnostics::color(0.9f, 0.6f, 0.2f));
				graph_->set_color("scale-time", diagnostics::color(0.9f, 0.0f, 0.9f));
				graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
				diagnostics::register_graph(graph_);
			}

			~ndi_consumer()
			{
				if (p_ndi_send_)
					p_ndi_lib_->NDIlib_send_destroy(p_ndi_send_);
				if (swr_)
					swr_free(&swr_);
				sws_freeContext(sws_); //if it's null, it does nothing
				CASPAR_LOG(info) << print() << L" Successfully Uninitialized.";
			}

			virtual void initialize(const core::video_format_desc& format_desc, int) override
			{
				format_desc_ = format_desc;
				width_ = static_cast<int>(format_desc.width * scale_);
				height_ = static_cast<int>(format_desc.height * scale_);
			}

			virtual int64_t presentation_frame_age_millis() const override
			{
				return 0;
			}

			virtual bool has_synchronization_clock() const override
			{
				return false;
			}
			
			virtual boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame) override
			{
				return executor_.begin_invoke([this, frame]() -> bool {
					graph_->set_value("tick-time", tick_timer_.elapsed() * format_desc_.fps * 0.5);
					tick_timer_.restart();
					frame_timer_.restart();
					send_video(frame);
					send_audio(frame);
					graph_->set_value("frame-time", frame_timer_.elapsed() * format_desc_.fps * 0.5);
					return true;
				});
			}

			void send_video(const safe_ptr<core::read_frame>& frame)
			{
				scale_timer_.restart();

				//scale and convert colorspace of the frame
				if (!sws_)
					sws_ = create_sws(format_desc_, width_, height_);
				std::shared_ptr<NDIlib_video_frame_t> video_frame = create_video_frame(format_desc_, width_, height_);
				if (!frame->image_data().empty())
				{
					std::shared_ptr<AVFrame> in_frame(av_frame_alloc(), [](AVFrame* frame) { av_frame_free(&frame); });
					auto in_picture = reinterpret_cast<AVPicture*>(in_frame.get());
					avpicture_fill(in_picture, const_cast<uint8_t*>(frame->image_data().begin()), AV_PIX_FMT_BGRA, format_desc_.width, format_desc_.height);
					sws_scale(sws_, in_frame->data, in_frame->linesize, 0, format_desc_.height, &video_frame->p_data, &video_frame->line_stride_in_bytes);
				}
				else
					fast_memclr(video_frame->p_data, format_desc_.width * format_desc_.height * sizeof(float));
				graph_->set_value("scale-time", scale_timer_.elapsed() * format_desc_.fps * 0.5);
				send_timer_.restart();

				// send video to NDI
				p_ndi_lib_->NDIlib_send_send_video(p_ndi_send_, video_frame.get());
				
				graph_->set_value("send-time", send_timer_.elapsed() * format_desc_.fps * 0.5);
			}

			void send_audio(const safe_ptr<core::read_frame>& frame)
			{
				if (!swr_)
					swr_ = create_swr(format_desc_, channel_layout_, frame->num_channels());

				auto audio_frame = create_audio_frame(channel_layout_, frame->multichannel_view().num_samples(), format_desc_.audio_sample_rate);
				const uint8_t* in[] = { reinterpret_cast<const uint8_t*>(frame->audio_data().begin()) };
				int converted_sample_count = swr_convert(swr_,
					reinterpret_cast<uint8_t**>(&audio_frame->p_data), audio_frame->no_samples,
					in, frame->multichannel_view().num_samples());
				if (converted_sample_count != audio_frame->no_samples)
					CASPAR_LOG(warning) << print() << L" Not all samples were converted (" << converted_sample_count << L" of " << audio_frame->no_samples << L").";
				p_ndi_lib_->NDIlib_util_send_send_audio_interleaved_32f(p_ndi_send_, audio_frame.get());
			}

			virtual std::wstring print() const override
			{
				return L"NewTek NDI™[" + ndi_name_ + L"]";
			}

			virtual boost::property_tree::wptree info() const override
			{
				boost::property_tree::wptree info;
				info.add(L"type", L"ndi-consumer");
				info.add(L"name", ndi_name_);
				return info;
			}

			virtual size_t buffer_depth() const override
			{
				return 1;
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
			auto scale = params.get(L"SCALE", 1.0);
			auto channel_layout = core::default_channel_layout_repository()
				.get_by_name(
					params.get(L"CHANNEL_LAYOUT", L"STEREO"));
			return make_safe<ndi_consumer>(channel_layout, ndi_name, groups, scale);
		}

		safe_ptr<core::frame_consumer> create_ndi_consumer(const boost::property_tree::wptree& ptree)
		{
			auto ndi_name = narrow(ptree.get(L"name", L"default"));
			auto groups = narrow(ptree.get(L"groups", L""));
			auto scale = ptree.get(L"scale", 1.0);
			auto channel_layout =
				core::default_channel_layout_repository()
				.get_by_name(
					boost::to_upper_copy(ptree.get(L"channel-layout", L"STEREO")));
			return make_safe<ndi_consumer>(channel_layout, ndi_name, groups, scale);
		}

	}
}