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
*  Author: Jerzy Jaśkiewicz, jurek@tvp.pl
*/


#include "ndi_producer.h"
#include "../util/ndi_util.h"
#include "../ndi.h"

#include "../../ffmpeg/producer/filter/filter.h"
#include "../../ffmpeg/producer/util/util.h"
#include "../../ffmpeg/producer/muxer/frame_muxer.h"
#include "../../ffmpeg/producer/muxer/display_mode.h"

#include <common/concurrency/executor.h>
#include <common/diagnostics/graph.h>
#include <common/exception/exceptions.h>
#include <common/exception/win32_exception.h>
#include <common/log/log.h>
#include <common/utility/string.h>

#include <core/producer/frame_producer.h>
#include <core/parameters/parameters.h>
#include <core/monitor/monitor.h>
#include <core/mixer/write_frame.h>
#include <core/producer/frame/frame_transform.h>
#include <core/producer/frame/frame_factory.h>

#include <tbb/concurrent_queue.h>

#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/timer.hpp>
#include <boost/circular_buffer.hpp>
#include <queue>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavcodec/avcodec.h>
	#include <libswresample/swresample.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif


namespace caspar { namespace ndi {

	typedef std::unique_ptr<void, std::function<void(NDIlib_recv_instance_t)>> ndi_receiver_ptr_t;
	typedef std::unique_ptr<SwrContext, std::function<void(SwrContext*)>> swr_ptr_t;
	typedef std::pair<int64_t, std::shared_ptr<core::audio_buffer>> audio_buffer_item_t;

	swr_ptr_t create_swr(const int out_sample_rate, const int out_nb_channels, const int in_nb_channels, const int in_sample_rate)
	{
		const auto ALL_63_CHANNELS = 0x7FFFFFFFFFFFFFFFULL;
		auto in_channel_layout = ALL_63_CHANNELS >> (63 - in_nb_channels);
		auto out_channel_layout = ALL_63_CHANNELS >> (63 - out_nb_channels);
		auto swr = swr_alloc_set_opts(NULL,
			out_channel_layout,
			AV_SAMPLE_FMT_S32,
			out_sample_rate,
			in_channel_layout,
			AV_SAMPLE_FMT_FLT,
			in_sample_rate,
			0, NULL);
		if (!swr)
			BOOST_THROW_EXCEPTION(caspar_exception()
				<< msg_info("Cannot alloc audio resampler"));
		if (swr_init(swr) < 0)
			BOOST_THROW_EXCEPTION(caspar_exception()
				<< msg_info("Cannot initialize audio resampler"));
		return swr_ptr_t(swr, [](SwrContext* ctx) { if (ctx) swr_free(&ctx); });
	}

class ndi_producer : public core::frame_producer
{	
	core::monitor::subject																	monitor_subject_;
	safe_ptr<diagnostics::graph>															graph_;
	boost::timer																			tick_timer_;
	boost::timer																			frame_timer_;
	core::video_format_desc																	format_desc_;
	caspar::safe_ptr<core::basic_frame>														last_frame_;

	const NDIlib_v2 *																		ndi_lib_;
	ndi_receiver_ptr_t																		ndi_receive_;
	
	swr_ptr_t																				swr_;
	int																						in_audio_sample_rate_;
	int																						in_audio_nb_channels_;

	const std::wstring																		source_name_;
	const std::wstring																		source_address_;

	ffmpeg::frame_muxer																		muxer_;
			
	safe_ptr<core::frame_factory>															frame_factory_;
	tbb::concurrent_bounded_queue<safe_ptr<core::basic_frame>>								frame_buffer_;
	std::queue<audio_buffer_item_t>															audio_buffer_;
	std::vector<float>																		audio_conversion_buffer_;
	const int64_t																			video_frame_duration_; // in 100 ns

	core::channel_layout																	audio_channel_layout_;
	executor																				executor_;

public:
	ndi_producer(
			const safe_ptr<core::frame_factory>& frame_factory,
			const core::video_format_desc& format_desc,
			const core::channel_layout& audio_channel_layout,
			const std::wstring& source_name,
			const std::wstring& source_address,
			const int buffer_depth
	)
		: source_name_(source_name)
		, source_address_(source_address)
		, format_desc_(format_desc)
		, muxer_(format_desc.fps, frame_factory, false, audio_channel_layout, "")
		, frame_factory_(frame_factory)
		, audio_channel_layout_(audio_channel_layout)
		, ndi_lib_(load_ndi())
		, executor_(print())
		, video_frame_duration_(static_cast<int64_t>(format_desc.duration) * 10000000 / format_desc.time_scale)
	{		
		if (!ndi_lib_)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(" NDI library not loaded"));

		frame_buffer_.set_capacity(buffer_depth);
		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));	
		graph_->set_color("late-frame", diagnostics::color(1.0f, 0.3f, 0.3f));
		graph_->set_color("dropped-frame", diagnostics::color(1.0f, 1.0f, 0.3f));
		graph_->set_color("empty-audio", diagnostics::color(0.3f, 0.9f, 1.0f));
		graph_->set_color("output-buffer", diagnostics::color(0.0f, 1.0f, 0.0f));
		graph_->set_color("audio-buffer", diagnostics::color(0.3f, 0.3f, 1.0f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);
		executor_.begin_invoke([this]() { receiver_proc(); });
		CASPAR_LOG(info) << print() << L" successfully initialized.";
	}

	~ndi_producer()
	{
		executor_.stop();
		executor_.join();
		CASPAR_LOG(info) << print() << L" successfully uninitialized.";
	}

	void ndi_connect() 
	{
		try
		{
			std::string source_name = narrow(source_name_);
			std::string source_address = narrow(source_address_);
			NDIlib_recv_create_t settings;
			settings.source_to_connect_to.p_ip_address = source_address.empty() ? NULL : source_address.c_str();
			settings.source_to_connect_to.p_ndi_name = source_name.empty() ? NULL : source_name.c_str();
			settings.color_format = NDIlib_recv_color_format_e_UYVY_BGRA;
			settings.bandwidth = NDIlib_recv_bandwidth_highest;
			settings.allow_video_fields = false;
			ndi_receive_ = ndi_receiver_ptr_t(ndi_lib_->NDIlib_recv_create2(&settings), [this](NDIlib_recv_instance_t  r)
			{
				if (r == NULL)
					return;
				ndi_lib_->NDIlib_recv_destroy(r);
			});
		}
		catch (...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
		}
	}

	void receiver_proc()
	{
		ndi_connect();
		while (executor_.is_running())
		{
			tick();
		}
	}

	void tick()
	{
		try
		{
			read_next_frame();
			if (auto frame = muxer_.poll())
			{
				auto safe_frame = make_safe_ptr(frame);
				while (!frame_buffer_.try_push(safe_frame))
				{
					auto dummy = core::basic_frame::empty();
					frame_buffer_.try_pop(dummy);
					graph_->set_tag("dropped-frame");
				}
			}
		}
		catch (...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
			muxer_.clear();
		}
	}
	
	void read_next_frame()
	{
		NDIlib_video_frame_t video_frame;
		NDIlib_audio_frame_t audio_frame;
		switch (ndi_lib_->NDIlib_recv_capture(ndi_receive_.get(), &video_frame, &audio_frame, NULL, 1000))
		{
		case NDIlib_frame_type_video:
			process_video_sync_and_send_to_muxer(&video_frame);
			ndi_lib_->NDIlib_recv_free_video(ndi_receive_.get(), &video_frame);
			break;
		case NDIlib_frame_type_audio:
			NDIlib_audio_frame_interleaved_32f_t interleaved_frame;
			if (audio_conversion_buffer_.size() < static_cast<size_t>(audio_frame.no_samples * audio_frame.no_channels))
				audio_conversion_buffer_.resize(audio_frame.no_samples * audio_frame.no_channels);
			interleaved_frame.p_data = audio_conversion_buffer_.data();
			ndi_lib_->NDIlib_util_audio_to_interleaved_32f(&audio_frame, &interleaved_frame);
			queue_audio(&interleaved_frame);
			ndi_lib_->NDIlib_recv_free_audio(ndi_receive_.get(), &audio_frame);
			break;
		case NDIlib_frame_type_error:
			CASPAR_LOG(info) << print() << L" error.";
			break;
		default:
			CASPAR_LOG(trace) << print() << L" no frame.";
			break;
		}
	}

	void process_video_sync_and_send_to_muxer(NDIlib_video_frame_t * ndi_video)
	{
		graph_->set_value("tick-time", tick_timer_.elapsed()*format_desc_.fps*0.5);
		tick_timer_.restart();
		std::shared_ptr<AVFrame> av_frame(av_frame_alloc(), [](AVFrame* frame) {av_frame_free(&frame); });
		av_frame->data[0] = ndi_video->p_data;
		av_frame->linesize[0] = ndi_video->line_stride_in_bytes;
		switch (ndi_video->FourCC)
		{
		case NDIlib_FourCC_type_UYVY:
			av_frame->format = AV_PIX_FMT_UYVY422;
			break;
		case NDIlib_FourCC_type_BGRA:
			av_frame->format = AV_PIX_FMT_BGRA;
			break;
		case NDIlib_FourCC_type_BGRX:
			av_frame->format = AV_PIX_FMT_BGR0;
			break;
		case NDIlib_FourCC_type_RGBA:
			av_frame->format = AV_PIX_FMT_RGBA;
			break;
		case NDIlib_FourCC_type_RGBX:
			av_frame->format = AV_PIX_FMT_RGB0;
			break;
		default:
			CASPAR_LOG(warning) << print() << L" Invalid format of NDI frame (" << ndi_video->FourCC << L").";
			return;
		}
		av_frame->format = ndi_video->FourCC == NDIlib_FourCC_type_UYVY ? AV_PIX_FMT_UYVY422 : AV_PIX_FMT_BGRA;
		av_frame->width = ndi_video->xres;
		av_frame->height = ndi_video->yres;
		av_frame->pict_type = AV_PICTURE_TYPE_I;
		av_frame->interlaced_frame = ndi_video->frame_format_type == NDIlib_frame_format_type_interleaved ? 1 : 0;
		av_frame->top_field_first = av_frame->interlaced_frame;

		muxer_.push(av_frame);
		audio_buffer_item_t audio;
		while (!audio_buffer_.empty())
		{
			int64_t frame_timecode = audio_buffer_.front().first;
			if (frame_timecode > ndi_video->timecode)
				break;
			if (frame_timecode <= ndi_video->timecode)
			{
				if (frame_timecode > ndi_video->timecode - video_frame_duration_)
					muxer_.push(audio_buffer_.front().second);
				audio_buffer_.pop();
			}
		}
		if (!muxer_.audio_ready())
		{
			muxer_.push(std::make_shared<core::audio_buffer>(format_desc_.audio_sample_rate * format_desc_.duration * audio_channel_layout_.num_channels / format_desc_.time_scale, 0));
			graph_->set_tag("empty-audio");
		}
	}

	void queue_audio(NDIlib_audio_frame_interleaved_32f_t * ndi_audio)
	{
		if (!swr_ || ndi_audio->sample_rate != in_audio_sample_rate_ || ndi_audio->no_channels != in_audio_nb_channels_)
		{
			swr_ = create_swr(format_desc_.audio_sample_rate, audio_channel_layout_.num_channels, ndi_audio->no_channels, ndi_audio->sample_rate);
			in_audio_nb_channels_ = ndi_audio->no_channels;
			in_audio_sample_rate_ = ndi_audio->sample_rate;
			CASPAR_LOG(trace) << print() << L" Created resampler for " << in_audio_nb_channels_ << L" channels and " << in_audio_sample_rate_ << L" sample rate";
		}
		int out_samples_count = swr_get_out_samples(swr_.get(), ndi_audio->no_samples);
		std::shared_ptr<core::audio_buffer> buffer (std::make_shared<core::audio_buffer>(out_samples_count * audio_channel_layout_.num_channels , 0));
		uint8_t* out[AV_NUM_DATA_POINTERS] = { reinterpret_cast<uint8_t*>(buffer->data()) }; 
		const uint8_t *in[AV_NUM_DATA_POINTERS] = { reinterpret_cast<uint8_t*>(ndi_audio->p_data) };
		int converted_sample_count = swr_convert(swr_.get(),
			out, out_samples_count,
			in, ndi_audio->no_samples);
		if (converted_sample_count != out_samples_count)
			CASPAR_LOG(warning) << print() << L" Not all samples were converted (" << converted_sample_count << L" of " << out_samples_count << L").";
		audio_buffer_.push(audio_buffer_item_t(ndi_audio->timecode, buffer));
		while (audio_buffer_.size() > 10)
			audio_buffer_.pop();
		graph_->set_value("audio-buffer", static_cast<float>(audio_buffer_.size()) / 10.0f);
	}
				
	virtual safe_ptr<core::basic_frame> receive(int hints) override
	{
		safe_ptr<core::basic_frame> frame = core::basic_frame::late();
		if(!frame_buffer_.try_pop(frame))
			graph_->set_tag("late-frame");
		graph_->set_value("output-buffer", static_cast<float>(frame_buffer_.size())/static_cast<float>(frame_buffer_.capacity()));	
		if (frame != core::basic_frame::late())
			last_frame_ = frame;
		monitor_subject_ << core::monitor::message("/source") % source_name_;
		return frame;
	}

	virtual safe_ptr<core::basic_frame> last_frame() const override
	{
		return disable_audio(last_frame_);
	}
	
	std::wstring print() const
	{
		return L"[ndi_producer] [" + source_name_+ source_address_ + L"]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"ndi-producer");
		return info;
	}

	virtual core::monitor::subject& monitor_output() override
	{
		return monitor_subject_;
	}

};

safe_ptr<core::frame_producer> create_producer(
		const safe_ptr<core::frame_factory>& frame_factory,
		const core::parameters& params)
{
	if(params.empty() || !boost::iequals(params[0], "ndi"))
		return core::frame_producer::empty();
	auto source_address = params.get(L"ADDRESS", L"");
	auto source_name = params.get(L"NAME", L"");
	if (source_name.empty() && source_address.empty())
		source_name = params.get(L"NDI", L"");
	if (source_name.empty() && source_address.empty())
		return core::frame_producer::empty();
	auto buffer_depth = params.get(L"BUFFER", 2);
	auto format_desc = core::video_format_desc::get(params.get(L"FORMAT", L"INVALID"));
	auto audio_layout = core::create_custom_channel_layout(
			params.get(L"CHANNEL_LAYOUT", L"STEREO"),
			core::default_channel_layout_repository());
	if(format_desc.format == core::video_format::invalid)
		format_desc = frame_factory->get_video_format_desc();
	return make_safe<ndi_producer>(frame_factory, format_desc, audio_layout, source_name, source_address, buffer_depth);
}

}
}
