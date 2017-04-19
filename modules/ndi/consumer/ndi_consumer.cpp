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

#include"ndi_consumer.h"
#include "../util/ndi_util.h"

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

		struct ndi_consumer : public core::frame_consumer
		{
			core::video_format_desc				format_desc_;
			core::channel_layout				channel_layout_;
			std::wstring						ndi_name_;
			const int							index_;
			const NDIlib_v2*					p_ndi_lib_;
			const NDIlib_send_instance_t		p_ndi_send_;
			executor							executor_;
			safe_ptr<diagnostics::graph>		graph_;
			boost::timer						frame_timer_;

		public:

			// frame_consumer

			ndi_consumer(core::channel_layout channel_layout, const std::string& ndi_name, const std::string groups)
				: channel_layout_(channel_layout)
				, ndi_name_(widen(ndi_name))
				, index_(NDI_CONSUMER_BASE_INDEX + crc16(ndi_name))
				, p_ndi_lib_(NDIlib_v2_load())
				, p_ndi_send_(create_ndi_send(p_ndi_lib_, ndi_name, groups))
				, executor_(print())
			{
				executor_.set_capacity(8);
				graph_->set_color("tick-time", diagnostics::color(0.5f, 1.0f, 0.2f));
				graph_->set_text(print());
				diagnostics::register_graph(graph_);
			}

			~ndi_consumer()
			{
				if (p_ndi_lib_) 
				{
					if (p_ndi_send_)
						p_ndi_lib_->NDIlib_send_destroy(p_ndi_send_);
					p_ndi_lib_->NDIlib_destroy();
				}
				CASPAR_LOG(info) << print() << L" Successfully Uninitialized.";
			}

			virtual void initialize(const core::video_format_desc& format_desc, int) override
			{
				format_desc_ = format_desc;
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
					send_video(frame);
					send_audio(frame);
					graph_->set_value("tick-time", static_cast<float>(frame_timer_.elapsed()*format_desc_.fps*0.5));
					frame_timer_.restart();
					return true;
				});
			}

			void send_video(const safe_ptr<core::read_frame>& frame)
			{
				std::shared_ptr<NDIlib_video_frame_t> video_frame = create_video_frame(format_desc_);
				if (!frame->image_data().empty())
					fast_memcpy(video_frame->p_data, frame->image_data().begin(), frame->image_size());
				else
					fast_memclr(video_frame->p_data, format_desc_.width * format_desc_.height * 4);
				p_ndi_lib_->NDIlib_send_send_video(p_ndi_send_, video_frame.get());
			}

			void send_audio(const safe_ptr<core::read_frame>& frame)
			{
				auto audio_frame = create_audio_frame(core::channel_layout::stereo());
				std::vector<int16_t, tbb::cache_aligned_allocator<int16_t>> audio_buffer;

				if (core::needs_rearranging(
					frame->multichannel_view(),
					channel_layout_,
					channel_layout_.num_channels))
				{
					core::audio_buffer downmixed;

					downmixed.resize(
						frame->multichannel_view().num_samples()
						* channel_layout_.num_channels,
						0);

					auto dest_view = core::make_multichannel_view<int32_t>(
						downmixed.begin(), downmixed.end(), channel_layout_);

					core::rearrange_or_rearrange_and_mix(
						frame->multichannel_view(),
						dest_view,
						core::default_mix_config_repository());

					audio_buffer = core::audio_32_to_16(downmixed);
				}
				else
				{
					audio_buffer = core::audio_32_to_16(frame->audio_data());
				}

				audio_frame->no_channels = channel_layout_.num_channels;
				audio_frame->sample_rate = 48000;
				audio_frame->no_samples = audio_buffer.size() / channel_layout_.num_channels;
				audio_frame->timecode = 0LL;
				audio_frame->p_data = audio_buffer.data();
				audio_frame->reference_level = 0;
				p_ndi_lib_->NDIlib_util_send_send_audio_interleaved_16s(p_ndi_send_, audio_frame.get());
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
			auto channel_layout = core::default_channel_layout_repository()
				.get_by_name(
					params.get(L"CHANNEL_LAYOUT", L"STEREO"));
			return make_safe<ndi_consumer>(channel_layout, ndi_name, groups);
		}

		safe_ptr<core::frame_consumer> create_ndi_consumer(const boost::property_tree::wptree& ptree)
		{
			auto ndi_name = narrow(ptree.get(L"name", L"default"));
			auto groups = narrow(ptree.get(L"groups", L""));
			auto channel_layout =
				core::default_channel_layout_repository()
				.get_by_name(
					boost::to_upper_copy(ptree.get(L"channel-layout", L"STEREO")));
			return make_safe<ndi_consumer>(channel_layout, ndi_name, groups);
		}

	}
}