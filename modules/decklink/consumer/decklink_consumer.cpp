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
 
#include "decklink_consumer.h"

#include "../util/util.h"

#include "../interop/DeckLinkAPI_h.h"

#include <core/mixer/read_frame.h>

#include <common/concurrency/com_context.h>
#include <common/concurrency/future_util.h>
#include <common/diagnostics/graph.h>
#include <common/exception/exceptions.h>
#include <common/exception/win32_exception.h>
#include <common/utility/assert.h>

#include <core/parameters/parameters.h>
#include <core/consumer/frame_consumer.h>
#include <core/mixer/audio/audio_util.h>
#include <core/system_watcher.h>

#include <tbb/concurrent_queue.h>
#include <tbb/cache_aligned_allocator.h>


#include <boost/circular_buffer.hpp>
#include <boost/timer.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/algorithm/string.hpp>

namespace caspar { namespace decklink { 

	const int TEMPERATURE_WARNING = 75;

struct decklink_consumer : public IDeckLinkVideoOutputCallback, IDeckLinkNotificationCallback, boost::noncopyable
{
	const int										channel_index_;
	const configuration								config_;
	const unsigned int								num_audio_channels_;
	int64_t											prev_temperature_;

	CComPtr<IDeckLink>								decklink_;
	CComQIPtr<IDeckLinkOutput>						output_;
	CComQIPtr<IDeckLinkKeyer>						keyer_;
	CComQIPtr<IDeckLinkProfileAttributes>			attributes_;
	CComQIPtr<IDeckLinkNotification>				notification_;
	CComQIPtr<IDeckLinkStatus>						status_;

	tbb::spin_mutex									exception_mutex_;
	std::exception_ptr								exception_;

	tbb::atomic<bool>								is_running_;
		
	const std::wstring								model_name_;
	const core::video_format_desc					format_desc_;
	const size_t									buffer_size_;

	long long										video_scheduled_;
	long long										audio_scheduled_;

	tbb::concurrent_bounded_queue<std::shared_ptr<core::read_frame>> frame_buffer_;
	std::vector<int32_t>							rearranged_audio_;

	safe_ptr<diagnostics::graph>					graph_;
	boost::timer									tick_timer_;
	retry_task<bool>								send_completion_;

	tbb::atomic<int64_t>							current_presentation_delay_;
	bool											audio_buffer_notified_;

public:
	decklink_consumer(const configuration& config, const core::video_format_desc& format_desc, int channel_index, int num_audio_channels) 
		: channel_index_(channel_index)
		, config_(config)
		, num_audio_channels_(num_decklink_out_channels(num_audio_channels))
		, decklink_(get_device(config.device_index))
		, output_(decklink_)
		, keyer_(decklink_)
		, attributes_(decklink_)
		, notification_(decklink_)
		, status_(decklink_)
		, model_name_(get_model_name(decklink_))
		, format_desc_(format_desc)
		, buffer_size_(config.buffer_depth()) // Minimum buffer-size 3.
	{
		current_presentation_delay_ = 0;
				
		frame_buffer_.set_capacity(1);

		graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));	
		graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
		graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
		graph_->set_color("flushed-frame", diagnostics::color(0.4f, 0.3f, 0.8f));
		graph_->set_color("buffered-audio", diagnostics::color(0.9f, 0.9f, 0.5f));
		graph_->set_color("buffered-video", diagnostics::color(0.2f, 0.9f, 0.9f));
		graph_->set_text(print());
		diagnostics::register_graph(graph_);

		if (FAILED(output_->SetScheduledFrameCompletionCallback(this)))
			BOOST_THROW_EXCEPTION(caspar_exception()
				<< msg_info(narrow(print()) + " Failed to set playback completion callback.")
				<< boost::errinfo_api_function("SetScheduledFrameCompletionCallback"));

		BMDDisplayMode display_mode = get_display_mode(output_, format_desc_.format, bmdFormat8BitBGRA)->GetDisplayMode();
		if (FAILED(output_->EnableVideoOutput(display_mode, bmdVideoOutputFlagDefault)))
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(narrow(print()) + " Could not enable video output."));

		if(config.embedded_audio)
		{
			if (FAILED(output_->EnableAudioOutput(BMDAudioSampleRate::bmdAudioSampleRate48kHz, BMDAudioSampleType::bmdAudioSampleType32bitInteger, num_audio_channels_, BMDAudioOutputStreamType::bmdAudioOutputStreamTimestamped)))
				BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(narrow(print()) + " Could not enable audio output."));
			CASPAR_LOG(info) << print() << L" Enabled embedded-audio.";
		}


		set_latency(CComQIPtr<IDeckLinkConfiguration_v10_2>(decklink_), config.latency, print());
		set_keyer(attributes_, keyer_, config.keyer, print());
				
		start_playback();
		 
		if (FAILED(notification_->Subscribe(BMDNotifications::bmdStatusChanged, this)))
			CASPAR_LOG(warning) << print() << L" Failed to register notification callback.";
		if (SUCCEEDED(status_->GetInt(BMDDeckLinkStatusID::bmdDeckLinkStatusDeviceTemperature, &prev_temperature_)))
		{
			if (prev_temperature_ >= TEMPERATURE_WARNING)
				CASPAR_LOG(warning) << print() << L" Temperature: " << prev_temperature_ << " C.";
			else
				CASPAR_LOG(info) << print() << L" Temperature: " << prev_temperature_ << " C.";
		}
		BMDReferenceStatus reference_status;
		if (FAILED(output_->GetReferenceStatus(&reference_status)))
			CASPAR_LOG(error) << print() << L" Reference signal: failed while querying status";
		else
			if (reference_status == 0)
				CASPAR_LOG(info) << print() << L" Reference signal: not detected.";
			else if (reference_status & bmdReferenceNotSupportedByHardware)
				CASPAR_LOG(info) << print() << L" Reference signal: not supported by hardware.";
			else if (reference_status & bmdReferenceLocked)
				CASPAR_LOG(info) << print() << L" Reference signal: locked.";
			else
				CASPAR_LOG(info) << print() << L" Reference signal: Unhandled enum bitfield: " << reference_status;

		int64_t pci_version = 0, pci_width = 0;
		status_->GetInt(BMDDeckLinkStatusID::bmdDeckLinkStatusPCIExpressLinkWidth, &pci_width);
		status_->GetInt(BMDDeckLinkStatusID::bmdDeckLinkStatusPCIExpressLinkSpeed, &pci_version);
		CASPAR_LOG(info) << print() << L" successfully initialized" 
			<< (pci_version == 0 ? L"." : L" on PCIe v" + std::to_wstring(pci_version)) 
			<< (pci_width == 0 ? L"" : L" x" + std::to_wstring(pci_width) + L".");
	}

	~decklink_consumer()
	{	
		notification_->Unsubscribe(BMDNotifications::bmdStatusChanged, this);
		is_running_ = false;
		frame_buffer_.try_push(std::make_shared<core::read_frame>());

		if(output_ != nullptr) 
		{
			output_->SetScheduledFrameCompletionCallback(nullptr);
			output_->SetAudioCallback(nullptr);
			output_->StopScheduledPlayback(0, nullptr, 0);
			if(config_.embedded_audio)
				output_->DisableAudioOutput();
			output_->DisableVideoOutput();
		}
	}
	

	STDMETHOD (QueryInterface(REFIID, LPVOID*))	{return E_NOINTERFACE;}
	STDMETHOD_(ULONG, AddRef())					{return 1;}
	STDMETHOD_(ULONG, Release())				{return 1;}

	STDMETHOD(ScheduledPlaybackHasStopped())    {return S_OK;}

	STDMETHOD(ScheduledFrameCompleted(IDeckLinkVideoFrame* completed_frame, BMDOutputFrameCompletionResult result))
	{
		win32_exception::ensure_handler_installed_for_thread("decklink-ScheduledFrameCompleted");
		if(!is_running_)
			return E_FAIL;
		
		try
		{
			auto dframe = reinterpret_cast<decklink_frame*>(completed_frame);
			current_presentation_delay_ = dframe->get_age_millis();

			if(result == bmdOutputFrameDisplayedLate)
			{
				graph_->set_tag("late-frame");
				CASPAR_LOG(warning) << print() << L" Frame late.";
			}
			else if (result == bmdOutputFrameDropped)
			{
				graph_->set_tag("dropped-frame");
				CASPAR_LOG(warning) << print() << L" Frame dropped.";
			}
			else if (result == bmdOutputFrameFlushed)
			{
				graph_->set_tag("flushed-frame");
				CASPAR_LOG(warning) << print() << L" Frame flushed.";
			}

			if (config_.embedded_audio)
			{
				unsigned int buffered_audio;
				if (SUCCEEDED(output_->GetBufferedAudioSampleFrameCount(&buffered_audio)))
				{
					graph_->set_value("buffered-audio", static_cast<double>(buffered_audio) / (format_desc_.audio_cadence[0] * buffer_size_));
					if (buffered_audio >= bmdAudioSampleRate48kHz)
					{
						if (!audio_buffer_notified_)
						{
							CASPAR_LOG(warning) << print() << L" Audio buffer overflow: " << buffered_audio << " samples. Further errors will not be notified";
							audio_buffer_notified_ = true;
						}
					}
					else if (buffered_audio < format_desc_.audio_cadence[0])
					{
						if (!audio_buffer_notified_)
						{
							CASPAR_LOG(warning) << print() << L" Audio buffer underflow: " << buffered_audio << " samples. Further errors will not be notified";
							audio_buffer_notified_ = true;
						}
					}
					else if (audio_buffer_notified_)
					{
						audio_buffer_notified_ = false;
						CASPAR_LOG(warning) << print() << L" Previously notified audio buffer size error corrected.";
					}
				}
				else
					CASPAR_LOG(warning) << print() << L" GetBufferedAudioSampleFrameCount failed.";
			}

			unsigned int buffered_video;
			if (SUCCEEDED(output_->GetBufferedVideoFrameCount(&buffered_video)))
			{
				graph_->set_value("buffered-video", static_cast<double>(buffered_video) / buffer_size_);
				if (buffered_video * format_desc_.duration >= format_desc_.time_scale)
					CASPAR_LOG(error) << print() << L" Video buffer overflow: " << buffered_video << " frames";
				if (buffered_video == 0)
					CASPAR_LOG(warning) << print() << L" Video buffer empty. Consider increasing the buffer depth.";
			}
			else
				CASPAR_LOG(warning) << print() << L" GetBufferedVideoFrameCount failed.";

			std::shared_ptr<core::read_frame> frame;
			frame_buffer_.pop(frame);
			send_completion_.try_completion();

			if (config_.embedded_audio)
				schedule_next_audio(frame->multichannel_view());
			schedule_next_video(frame);
		}
		catch(...)
		{
			tbb::spin_mutex::scoped_lock lock(exception_mutex_);
			exception_ = std::current_exception();
			return E_FAIL;
		}
		return S_OK;
	}
		
	STDMETHOD(Notify(/* [in] */ BMDNotifications topic, /* [in] */ ULONGLONG param1, /* [in] */ ULONGLONG param2))
	{
		if (topic != BMDNotifications::bmdStatusChanged)
			return S_OK;
		int64_t int_value;
		BOOL flag;
		switch (param1)
		{
		case BMDDeckLinkStatusID::bmdDeckLinkStatusDeviceTemperature:
			if (SUCCEEDED(status_->GetInt(BMDDeckLinkStatusID::bmdDeckLinkStatusDeviceTemperature, &int_value)))
			{
				if (int_value >= TEMPERATURE_WARNING && std::abs(int_value - prev_temperature_) > 1)
				{
					prev_temperature_ = int_value;
					CASPAR_LOG(warning) << print() << L" Temperature changed: " << int_value << " C";
				}
				else if (std::abs(int_value - prev_temperature_) > 4)
				{
					prev_temperature_ = int_value;
					CASPAR_LOG(info) << print() << L" Temperature changed: " << int_value << " C";
				}
			}
			break;
		case BMDDeckLinkStatusID::bmdDeckLinkStatusReferenceSignalLocked:
			if (SUCCEEDED(status_->GetFlag(BMDDeckLinkStatusID::bmdDeckLinkStatusReferenceSignalLocked, &flag))) 
			{
				CASPAR_LOG(info) << print() << L" Reference signal: " << (flag ? L"locked" : L"missing");
				if (flag && config_.embedded_audio) 
				{
					// decklinks usually sync video to reference input delaying video frame, without stopping audio playout
					// this causes audio buffer is consumed gradually to empty after few synces. The only solid solution is to restart playback
					if (SUCCEEDED(output_->StopScheduledPlayback(0, nullptr, 0)))
					{
						CASPAR_LOG(debug) << print() << L" Scheduled playback stopped.";
						is_running_ = false;
						start_playback();
					}
				}
			}
			break;
		case BMDDeckLinkStatusID::bmdDeckLinkStatusPCIExpressLinkWidth:
			if (SUCCEEDED(status_->GetInt(BMDDeckLinkStatusID::bmdDeckLinkStatusPCIExpressLinkWidth, &int_value)))
				CASPAR_LOG(info) << print() << L" PCIe width changed: " << int_value << "x";;
			break;
		default:
			break;
		}
		
		return S_OK;
	}

	template<typename View>
	void schedule_next_audio(const View& view)
	{
		const int sample_frame_count = view.num_samples();

		if (core::needs_rearranging(
				view, config_.audio_layout, num_audio_channels_))
		{
			rearranged_audio_.resize(
					sample_frame_count * num_audio_channels_);

			auto dest_view = core::make_multichannel_view<int32_t>(
					rearranged_audio_.begin(),
					rearranged_audio_.end(),
					config_.audio_layout,
					num_audio_channels_);

			core::rearrange_or_rearrange_and_mix(
					view, dest_view, core::default_mix_config_repository());

			if (config_.audio_layout.num_channels == 1) // mono
				boost::copy(                            // duplicate L to R
						dest_view.channel(0),
						dest_view.channel(1).begin());
		}
		else
		{
			rearranged_audio_ = std::vector<int32_t>(view.raw_begin(), view.raw_end());
		}
		
		unsigned int samples_written;
		if (FAILED(output_->ScheduleAudioSamples(
			rearranged_audio_.data(),
			sample_frame_count,
			audio_scheduled_,
			format_desc_.audio_sample_rate,
			&samples_written)))
			CASPAR_LOG(error) << print() << L" Failed to schedule audio.";
		if (samples_written != static_cast<unsigned int>(sample_frame_count))
			CASPAR_LOG(warning) << print() << L" Not all available audio samples has been scheduled (" << samples_written << L" of " << sample_frame_count << L")";
		audio_scheduled_ += sample_frame_count;
	}
			
	void schedule_next_video(const std::shared_ptr<core::read_frame>& frame)
	{
		CComPtr<IDeckLinkVideoFrame> frame2(new decklink_frame(frame, format_desc_, config_.key_only));
		if (FAILED(output_->ScheduleVideoFrame(frame2, video_scheduled_, format_desc_.duration, format_desc_.time_scale)))
			CASPAR_LOG(error) << print() << L" Failed to schedule video.";

		video_scheduled_ += format_desc_.duration;
		graph_->set_value("tick-time", tick_timer_.elapsed()*format_desc_.fps*0.5);
		tick_timer_.restart();
	}

	boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame)
	{
		tbb::spin_mutex::scoped_lock lock(exception_mutex_);
		if (exception_ != nullptr)
			std::rethrow_exception(exception_);

		bool buffer_ready = false;

		auto enqueue_task = [buffer_ready, frame, this]() mutable -> boost::optional<bool>
		{

			if (!buffer_ready)
				buffer_ready = frame_buffer_.try_push(frame);

			if (buffer_ready)
				return true;
			else
				return boost::optional<bool>();
		};

		if (enqueue_task())
			return wrap_as_future(true);

		send_completion_.set_task(enqueue_task);

		return send_completion_.get_future();
	}
	
	std::wstring print() const
	{
		return model_name_ + L" Ch:" + boost::lexical_cast<std::wstring>(channel_index_) + L" Id:" +
			boost::lexical_cast<std::wstring>(config_.device_index) + L" Fmt: " +  format_desc_.name;
	}

	void start_playback() 
	{
		video_scheduled_ = 0LL;
		audio_scheduled_ = 0LL;
		if (config_.embedded_audio)
			output_->BeginAudioPreroll();

		for (uint32_t n = 0; n < buffer_size_; ++n)
		{
			if (config_.embedded_audio)
			{
				core::audio_buffer silent_audio_buffer(format_desc_.audio_cadence[n % format_desc_.audio_cadence.size()] * num_audio_channels_, 0);
				auto audio = core::make_multichannel_view<int32_t>(silent_audio_buffer.begin(), silent_audio_buffer.end(), config_.audio_layout, num_audio_channels_);;
				schedule_next_audio(audio);
			}
			schedule_next_video(make_safe<core::read_frame>());
		}

		if (config_.embedded_audio)
			output_->EndAudioPreroll();

		if (FAILED(output_->StartScheduledPlayback(0, format_desc_.time_scale, 1.0)))
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(narrow(print()) + " Failed to schedule playback."));
		is_running_ = true;
		CASPAR_LOG(debug) << print() << L" Scheduled playback started.";
	}

};

struct decklink_consumer_proxy : public core::frame_consumer
{
	const configuration				config_;
	com_context<decklink_consumer>	context_;
	core::video_format_desc			format_desc_;
	int								channel_index_;
public:

	decklink_consumer_proxy(const configuration& config)
		: config_(config)
		, context_(L"decklink_consumer[" + boost::lexical_cast<std::wstring>(config.device_index) + L"]")
	{
	}

	~decklink_consumer_proxy()
	{
		if(context_)
		{
			auto str = print();
			context_.reset();
			CASPAR_LOG(info) << str << L" Successfully Uninitialized.";	
		}
	}

	// frame_consumer
	
	virtual void initialize(const core::video_format_desc& format_desc, const core::channel_layout& audio_channel_layout, int channel_index) override
	{
		format_desc_ = format_desc;
		channel_index_ = channel_index;
		context_.reset([&] {return new decklink_consumer(config_, format_desc, channel_index, audio_channel_layout.num_channels); });
	}
	
	virtual boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame) override
	{
		CASPAR_VERIFY(format_desc_.audio_cadence.front() * frame->num_channels() == static_cast<size_t>(frame->audio_data().size()));
		boost::range::rotate(format_desc_.audio_cadence, std::begin(format_desc_.audio_cadence) + 1);
		return context_->send(frame);
	}
	
	virtual std::wstring print() const override
	{
		return context_ ? context_->print() : L"[decklink_consumer]";
	}		

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"decklink-consumer");
		info.add(L"key-only", config_.key_only);
		info.add(L"device", config_.device_index);
		info.add(L"low-latency", config_.low_latency);
		info.add(L"embedded-audio", config_.embedded_audio);
		info.add(L"presentation-frame-age", presentation_frame_age_millis());
		//info.add(L"internal-key", config_.internal_key);
		return info;
	}

	virtual size_t buffer_depth() const override
	{
		return config_.buffer_depth();
	}

	virtual int index() const override
	{
		return DECKLINK_CONSUMER_BASE_INDEX + config_.device_index;
	}

	virtual int64_t presentation_frame_age_millis() const
	{
		return context_ ? context_->current_presentation_delay_ : 0;
	}

};	

safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params) 
{
	if(params.size() < 1 || params[0] != L"DECKLINK")
		return core::frame_consumer::empty();
	
	configuration config;
		
	if(params.size() > 1)
		config.device_index = lexical_cast_or_default<int>(params[1], config.device_index);
	
	if(std::find(params.begin(), params.end(), L"INTERNAL_KEY")			!= params.end())
		config.keyer = configuration::internal_keyer;
	else if(std::find(params.begin(), params.end(), L"EXTERNAL_KEY")	!= params.end())
		config.keyer = configuration::external_keyer;
	else
		config.keyer = configuration::default_keyer;

	if(std::find(params.begin(), params.end(), L"LOW_LATENCY")	 != params.end())
		config.latency = configuration::low_latency;

	config.embedded_audio	= std::find(params.begin(), params.end(), L"EMBEDDED_AUDIO") != params.end();
	config.key_only			= std::find(params.begin(), params.end(), L"KEY_ONLY")		 != params.end();
	config.audio_layout     = core::default_channel_layout_repository().get_by_name(params.get(L"CHANNEL_LAYOUT", L"STEREO"));
	return make_safe<decklink_consumer_proxy>(config);
}

safe_ptr<core::frame_consumer> create_consumer(const boost::property_tree::wptree& ptree) 
{
	configuration config;

	auto keyer = ptree.get(L"keyer", L"external");
	if(keyer == L"external")
		config.keyer = configuration::external_keyer;
	else if(keyer == L"internal")
		config.keyer = configuration::internal_keyer;

	auto latency = ptree.get(L"latency", L"normal");
	if(latency == L"low")
		config.latency = configuration::low_latency;
	else if(latency == L"normal")
		config.latency = configuration::normal_latency;

	config.key_only				= ptree.get(L"key-only",			config.key_only);
	config.device_index			= ptree.get(L"device",				config.device_index);
	config.embedded_audio		= ptree.get(L"embedded-audio",		config.embedded_audio);
	config.base_buffer_depth	= ptree.get(L"buffer-depth",		config.base_buffer_depth);
	config.audio_layout			= core::default_channel_layout_repository().get_by_name(
			boost::to_upper_copy(ptree.get(L"channel-layout", L"STEREO")));

	return make_safe<decklink_consumer_proxy>(config);
}

}}

/*
##############################################################################
Pre-rolling

Mail: 2011-05-09

Yoshan
BMD Developer Support
developer@blackmagic-design.com

-----------------------------------------------------------------------------

Thanks for your inquiry. The minimum number of frames that you can preroll 
for scheduled playback is three frames for video and four frames for audio. 
As you mentioned if you preroll less frames then playback will not start or
playback will be very sporadic. From our experience with Media Express, we 
recommended that at least seven frames are prerolled for smooth playback. 

Regarding the bmdDeckLinkConfigLowLatencyVideoOutput flag:
There can be around 3 frames worth of latency on scheduled output.
When the bmdDeckLinkConfigLowLatencyVideoOutput flag is used this latency is
reduced  or removed for scheduled playback. If the DisplayVideoFrameSync() 
method is used, the bmdDeckLinkConfigLowLatencyVideoOutput setting will 
guarantee that the provided frame will be output as soon the previous 
frame output has been completed.
################################################################################
*/

/*
##############################################################################
Async DMA Transfer without redundant copying

Mail: 2011-05-10

Yoshan
BMD Developer Support
developer@blackmagic-design.com

-----------------------------------------------------------------------------

Thanks for your inquiry. You could try subclassing IDeckLinkMutableVideoFrame 
and providing a pointer to your video buffer when GetBytes() is called. 
This may help to keep copying to a minimum. Please ensure that the pixel 
format is in bmdFormat10BitYUV, otherwise the DeckLink API / driver will 
have to colourspace convert which may result in additional copying.
################################################################################
*/