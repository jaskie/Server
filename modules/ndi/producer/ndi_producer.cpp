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
*  Author: Jerzy Jaœkiewicz, jurek@tvp.pl based on Robert Nagy, ronag89@gmail.com work
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
#include <common/memory/memclr.h>
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
#include <boost/thread.hpp>
#include <boost/timer.hpp>
#include <boost/locale.hpp>
#include <boost/circular_buffer.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavcodec/avcodec.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif


namespace caspar { namespace ndi {

	typedef std::unique_ptr<void, std::function<void(NDIlib_recv_instance_t)>> ndi_receiver_ptr_t;

class ndi_producer : public core::frame_producer
{	
	core::monitor::subject																	monitor_subject_;
	safe_ptr<diagnostics::graph>															graph_;
	boost::timer																			tick_timer_;
	boost::timer																			frame_timer_;
	core::video_format_desc																	format_desc_;
	caspar::safe_ptr<core::basic_frame>														last_frame_;

	const std::shared_ptr<NDIlib_v2>														ndi_lib_;
	ndi_receiver_ptr_t																		ndi_receive_;

	const std::wstring																		source_name_;
	const std::wstring																		source_address_;

	std::vector<size_t>																		audio_cadence_;
	boost::circular_buffer<size_t>															sync_buffer_;
	ffmpeg::frame_muxer																		muxer_;
			
	tbb::atomic<int>																		hints_;
	safe_ptr<core::frame_factory>															frame_factory_;
	tbb::concurrent_bounded_queue<safe_ptr<core::basic_frame>>								frame_buffer_;

	std::exception_ptr																		exception_;	
	core::channel_layout																	audio_channel_layout_;
	boost::thread																			receiver_thread_;

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
		, audio_cadence_(format_desc.audio_cadence)
		, muxer_(format_desc.fps, frame_factory, false, audio_channel_layout, "")
		, sync_buffer_(format_desc.audio_cadence.size())
		, frame_factory_(frame_factory)
		, audio_channel_layout_(audio_channel_layout)
		, ndi_lib_(load_ndi())
		, hints_(tbb::atomic<int>())
		, receiver_thread_([this]() { receiver_proc(); })
	{		
		if (!ndi_lib_)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(" NDI library not loaded"));

		frame_buffer_.set_capacity(buffer_depth);

		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));	
		graph_->set_color("late-frame", diagnostics::color(0.8f, 0.3f, 0.3f));
		graph_->set_color("frame-time", diagnostics::color(1.0f, 0.0f, 0.0f));
		graph_->set_color("dropped-frame", diagnostics::color(1.0f, 0.3f, 0.3f));
		graph_->set_color("empty-audio", diagnostics::color(1.0f, 1.0f, 0.0f));
		graph_->set_color("empty-video", diagnostics::color(1.0f, 0.0f, 1.0f));
		graph_->set_color("output-buffer", diagnostics::color(0.0f, 1.0f, 0.0f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);
		CASPAR_LOG(info) << print() << L" successfully initialized.";
	}

	~ndi_producer()
	{
		//close_input();
		receiver_thread_.interrupt();
		receiver_thread_.join();
		CASPAR_LOG(info) << print() << L" successfully uninitialized.";
	}

	void ndi_connect() 
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

	void receiver_proc()
	{
		ndi_connect();
		while (!receiver_thread_.interruption_requested())
		{
			tick();
		}
	}

	void tick()
	{
		read_next_frame();
		for (auto frame = muxer_.poll(); frame; frame = muxer_.poll())
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

	void read_next_frame()
	{
		NDIlib_video_frame_t video_frame;
		NDIlib_audio_frame_t audio_frame;
		switch (ndi_lib_->NDIlib_recv_capture(ndi_receive_.get(), &video_frame, &audio_frame, NULL, 1000))
		{
		case NDIlib_frame_type_video:
			process_video(&video_frame);
			ndi_lib_->NDIlib_recv_free_video(ndi_receive_.get(), &video_frame);
			break;
		case NDIlib_frame_type_audio:
			//processAudio(&audio_frame);
			ndi_lib_->NDIlib_recv_free_audio(ndi_receive_.get(), &audio_frame);
			break;
		case NDIlib_frame_type_error:
			CASPAR_LOG(info) << print() << L" error.";
			break;
		default:
			CASPAR_LOG(info) << print() << L" no frame.";
			break;
		}
	}


	void process_video(NDIlib_video_frame_t * ndi_video)
	{
		graph_->set_value("tick-time", tick_timer_.elapsed()*format_desc_.fps*0.5);
		tick_timer_.restart();
		std::shared_ptr<AVFrame> av_frame(av_frame_alloc(), [](AVFrame* frame) {av_frame_free(&frame); });
		av_frame->data[0] = ndi_video->p_data;
		av_frame->linesize[0] = ndi_video->line_stride_in_bytes;
		av_frame->format = ndi_video->FourCC == NDIlib_FourCC_type_UYVY ? AV_PIX_FMT_UYVY422 : AV_PIX_FMT_BGRA;
		av_frame->width = ndi_video->xres;
		av_frame->height = ndi_video->yres;
		av_frame->pict_type = AV_PICTURE_TYPE_I;
		av_frame->interlaced_frame = ndi_video->frame_format_type == NDIlib_frame_format_type_interleaved ? 1 : 0;
		av_frame->top_field_first = true;
		muxer_.push(av_frame, hints_, static_cast<int>((ndi_video->timecode * ndi_video->frame_rate_N)/(ndi_video->frame_rate_D * 10000000)));
		if (muxer_.video_ready() && !muxer_.audio_ready())
		{
			std::shared_ptr<core::audio_buffer> audio_buffer = std::make_shared<core::audio_buffer>(audio_cadence_.front() * audio_channel_layout_.num_channels, 0);
			muxer_.push(audio_buffer);
			graph_->set_tag("empty-audio");
		}
	}

	void processAudio(NDIlib_audio_frame_t * ndi_audio)
	{
		std::shared_ptr<core::audio_buffer> audio_buffer;

		// It is assumed that audio is always equal or ahead of video.
		auto sample_frame_count = ndi_audio->no_samples;
		auto audio_data = reinterpret_cast<int32_t*>(ndi_audio->p_data);

		if (ndi_audio->no_channels == audio_channel_layout_.num_channels)
		{
			audio_buffer = std::make_shared<core::audio_buffer>(
				audio_data,
				audio_data + sample_frame_count * ndi_audio->no_channels);
		}
		else
		{
			audio_buffer = std::make_shared<core::audio_buffer>();
			audio_buffer->resize(sample_frame_count * audio_channel_layout_.num_channels, 0);
			auto src_view = core::make_multichannel_view<int32_t>(
				audio_data,
				audio_data + sample_frame_count * ndi_audio->no_channels,
				audio_channel_layout_,
				ndi_audio->no_channels);
			auto dst_view = core::make_multichannel_view<int32_t>(
				audio_buffer->begin(),
				audio_buffer->end(),
				audio_channel_layout_);

			core::rearrange(src_view, dst_view);
		}

		// Note: Uses 1 step rotated cadence for 1001 modes (1602, 1602, 1601, 1602, 1601)
		// This cadence fills the audio mixer most optimally.

	/*	sync_buffer_.push_back(audio_buffer->size() / audio_channel_layout_.num_channels);
		if (!boost::range::equal(sync_buffer_, audio_cadence_))
		{
			CASPAR_LOG(trace) << print() << L" Syncing audio.";
			return S_OK;
		}*/

		muxer_.push(audio_buffer);
	}



	/*


	void VideoInputFrameArrived()
	{	
		win32_exception::ensure_handler_installed_for_thread("decklink-VideoInputFrameArrived");
		if(!video)
			return S_OK;

		try
		{
			graph_->set_value("tick-time", tick_timer_.elapsed()*format_desc_.fps*0.5);
			tick_timer_.restart();

			frame_timer_.restart();

			// PUSH

			void* bytes = nullptr;
			if(FAILED(video->GetBytes(&bytes)) || !bytes)
				return S_OK;
			
			std::shared_ptr<AVFrame> av_frame(av_frame_alloc(), [](AVFrame* frame) {av_frame_free(&frame);});
						
			av_frame->data[0]			= reinterpret_cast<uint8_t*>(bytes);
			av_frame->linesize[0]		= video->GetRowBytes();			
			av_frame->format			= AV_PIX_FMT_UYVY422;
			av_frame->width				= video->GetWidth();
			av_frame->height			= video->GetHeight();
			av_frame->pict_type			= AV_PICTURE_TYPE_I;
			auto fieldDominance = current_display_mode_->GetFieldDominance();
			av_frame->interlaced_frame	= fieldDominance == bmdLowerFieldFirst || fieldDominance == bmdUpperFieldFirst;
			av_frame->top_field_first	= fieldDominance == bmdUpperFieldFirst;
			IDeckLinkTimecode * decklink_timecode_bcd = NULL;
			int frame_timecode = std::numeric_limits<int>().max();
			if (SUCCEEDED(video->GetTimecode(timecode_source_, &decklink_timecode_bcd)) && decklink_timecode_bcd)
				frame_timecode = bcd2frame(decklink_timecode_bcd->GetBCD(), static_cast<byte>(time_scale_/frame_duration_));
			
			std::shared_ptr<core::audio_buffer> audio_buffer;

			// It is assumed that audio is always equal or ahead of video.
			if(audio && SUCCEEDED(audio->GetBytes(&bytes)) && bytes)
			{
				auto sample_frame_count = audio->GetSampleFrameCount();
				auto audio_data = reinterpret_cast<int32_t*>(bytes);

				if (num_input_channels_ == audio_channel_layout_.num_channels)
				{
					audio_buffer = std::make_shared<core::audio_buffer>(
							audio_data, 
							audio_data + sample_frame_count * num_input_channels_);
				}
				else
				{
					audio_buffer = std::make_shared<core::audio_buffer>();
					audio_buffer->resize(sample_frame_count * audio_channel_layout_.num_channels, 0);
					auto src_view = core::make_multichannel_view<int32_t>(
							audio_data, 
							audio_data + sample_frame_count * num_input_channels_, 
							audio_channel_layout_, 
							num_input_channels_);
					auto dst_view = core::make_multichannel_view<int32_t>(
							audio_buffer->begin(),
							audio_buffer->end(),
							audio_channel_layout_);

					core::rearrange(src_view, dst_view);
				}
			}
			else			
				audio_buffer = std::make_shared<core::audio_buffer>(audio_cadence_.front() * audio_channel_layout_.num_channels, 0);
			
			// Note: Uses 1 step rotated cadence for 1001 modes (1602, 1602, 1601, 1602, 1601)
			// This cadence fills the audio mixer most optimally.

			sync_buffer_.push_back(audio_buffer->size() / audio_channel_layout_.num_channels);		
			if(!boost::range::equal(sync_buffer_, audio_cadence_))
			{
				CASPAR_LOG(trace) << print() << L" Syncing audio.";
				return S_OK;
			}

			muxer_.push(audio_buffer);
			muxer_.push(av_frame, hints_, frame_timecode);
											
			boost::range::rotate(audio_cadence_, std::begin(audio_cadence_)+1);
			
			// POLL
			
			for(auto frame = muxer_.poll(); frame; frame = muxer_.poll())
			{
				if(!frame_buffer_.try_push(make_safe_ptr(frame)))
				{
					auto dummy = core::basic_frame::empty();
					frame_buffer_.try_pop(dummy);

					frame_buffer_.try_push(make_safe_ptr(frame));

					graph_->set_tag("dropped-frame");
				}
			}

			graph_->set_value("frame-time", frame_timer_.elapsed()*format_desc_.fps*0.5);

			graph_->set_value("output-buffer", static_cast<float>(frame_buffer_.size())/static_cast<float>(frame_buffer_.capacity()));	
		}
		catch(...)
		{
			exception_ = std::current_exception();
			return E_FAIL;
		}

		return S_OK;
	}
	*/
	
	virtual safe_ptr<core::basic_frame> receive(int hints) override
	{
		if(exception_ != nullptr)
			std::rethrow_exception(exception_);
		hints_ = hints;
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
	return create_producer_destroy_proxy(make_safe<ndi_producer>(frame_factory, format_desc, audio_layout, source_name, source_address, buffer_depth));
}

}}