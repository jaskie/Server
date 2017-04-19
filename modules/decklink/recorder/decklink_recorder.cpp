/*
* This file is part of TVP's fork of CasparCG (www.casparcg.com).
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
*/


#include "../StdAfx.h"

#include "decklink_recorder.h"

#include "../util/util.h"

#include "../interop/DeckLinkAPI_h.h"

#include <core/recorder.h>
#include <core/consumer/frame_consumer.h>
#include <core/video_channel.h>
#include <core/consumer/output.h>
#include <core/monitor/monitor.h>
#include <common/concurrency/executor.h>
#include <ffmpeg/consumer/ffmpeg_consumer.h>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>


#define ERR_TO_STR(err) ((err)==bmdDeckControlNoError) ? "No error" :\
						((err)==bmdDeckControlModeError) ? "Mode error":\
						((err)==bmdDeckControlMissedInPointError) ? "Missed InPoint error":\
						((err)==bmdDeckControlDeckTimeoutError) ? "DeckTimeout error":\
						((err)==bmdDeckControlCommandFailedError) ? "Cmd failed error":\
						((err)==bmdDeckControlDeviceAlreadyOpenedError) ? "Device already open":\
						((err)==bmdDeckControlFailedToOpenDeviceError) ? "Failed to open device error":\
						((err)==bmdDeckControlInLocalModeError) ? "InLocal mode error":\
						((err)==bmdDeckControlEndOfTapeError) ? "EOT error":\
						((err)==bmdDeckControlUserAbortError) ? "UserAbort error":\
						((err)==bmdDeckControlNoTapeInDeckError) ? "NoTape error":\
						((err)==bmdDeckControlNoVideoFromCardError) ? "No video from card error":\
						((err)==bmdDeckControlNoCommunicationError) ? "No communication error":"Unknown error"

// translate a BMDDeckControlStatusFlags to string
#define FLAGS_TO_STR(flags) ((flags & bmdDeckControlStatusDeckConnected) > 0) ? " Deck connected" : " Deck disconnected" +\
						((flags & bmdDeckControlStatusRemoteMode) > 0) ? " Remote mode" : " Local mode" +\
						((flags & bmdDeckControlStatusRecordInhibited) > 0) ? " Rec. disabled" : " Rec. enabled" +\
						((flags & bmdDeckControlStatusCassetteOut) > 0) ? " Cassette out" : " Cassette in"

// translate a BMDDeckControlEvent to a string
#define EVT_TO_STR(evt) ((evt)==bmdDeckControlPrepareForExportEvent) ? "Prepare for export" :\
						((evt)==bmdDeckControlPrepareForCaptureEvent) ? "Prepare for capture" :\
						((evt)==bmdDeckControlExportCompleteEvent) ? "Export complete" :\
						((evt)==bmdDeckControlCaptureCompleteEvent) ? "Capture complete" : "Abort"

// translate a BMDDeckControlVTRControlState to a string
#define STATE_TO_STR(state) (state==bmdDeckControlNotInVTRControlMode) ? "N/A" :\
						(state==bmdDeckControlVTRControlPlaying) ? "Play" :\
						(state==bmdDeckControlVTRControlRecording) ? "Record" :\
						(state==bmdDeckControlVTRControlStill) ? "Still" :\
						(state==bmdDeckControlVTRControlShuttleForward) ? "Shuttle forward" :\
						(state==bmdDeckControlVTRControlShuttleReverse) ? "Shuttle reverse" :\
						(state==bmdDeckControlVTRControlJogForward) ? "Jog forward" :\
						(state==bmdDeckControlVTRControlJogReverse) ? "Jog reverse" : "Stop"


namespace caspar {
	namespace decklink {

		BMDTimecodeBCD encode_timecode(std::wstring tc)
		{
			std::vector<std::wstring> split;
			boost::split(split, tc, boost::is_any_of(":."));
			if (split.size() == 4)
			{
				unsigned int hours   = boost::lexical_cast<unsigned int>(split[0]);
				unsigned int minutes = boost::lexical_cast<unsigned int>(split[1]);
				unsigned int seconds = boost::lexical_cast<unsigned int>(split[2]);
				unsigned int frames  = boost::lexical_cast<unsigned int>(split[3]);
				return ((hours / 10) << 28) | (((hours % 10) & 0xF) << 24)
					| ((minutes / 10) << 20) | (((minutes % 10) & 0xF) << 16)
					| ((seconds / 10) << 12) | (((seconds % 10) & 0xF) << 8)
					| ((frames / 10) << 4) | (frames & 0xF);
			}
			return 0;
		}
		
		std::string decode_timecode(BMDTimecodeBCD bcd)
		{
			short hour = (bcd >> 24 & 0xF) + (bcd >> 28 & 0xF) * 10;
			short min = (bcd >> 16 & 0xF) + (bcd >> 20 & 0xF) * 10;
			short sec = (bcd >> 8 & 0xF) + (bcd >> 12 & 0xF) * 10;
			short frames = (bcd & 0xF) + (bcd >> 4 & 0xF) * 10;
			return (boost::format( "%02d:%02d:%02d:%02d" ) % hour % min % sec % frames).str();
		}


		class decklink_recorder : public core::recorder, IDeckLinkDeckControlStatusCallback
		{
		private:
			enum record_state
			{
				idle,
				manual_recording,
				vcr_recording
			}										record_state_;
			com_initializer							init_;
			int										index_;
			const int								device_index_;
			const unsigned int						preroll_;
			int										offset_;
			CComPtr<IDeckLink>						decklink_;
			CComPtr<IDeckLinkDeckControl>			deck_control_;
			BMDDeckControlError						last_deck_error_;
			caspar::core::video_format_desc			format_desc_;
			tbb::atomic<bool>						deck_connected_;

			//fields of the current operation
			std::shared_ptr<core::video_channel>	channel_;
			std::wstring							file_name_;
			safe_ptr<core::frame_consumer>			consumer_;
			
			// timecodes are converted from BCD to unsigned integers
			tbb::atomic<int>						tc_in_;
			tbb::atomic<int>						tc_out_;

			tbb::atomic<int>						current_timecode_;
			safe_ptr<core::monitor::subject>		monitor_subject_;
			executor								executor_;

			void clean_recorder()
			{
				record_state_ = record_state::idle;
				auto consumer = consumer_;
				auto channel = channel_;
				if (channel && consumer != core::frame_consumer::empty())
				{
					consumer_ = core::frame_consumer::empty();
					executor_.begin_invoke([=]() {
						channel->output()->remove(consumer);
					});
				}
				channel_.reset();
				file_name_ = L"";
				tc_in_ = 0;
				tc_out_ = 0;
				current_timecode_ = std::numeric_limits<int>().max();
			}

			BMDTimecodeBCD tc_to_bcd(int tc)
			{
				auto format_desc = format_desc_;
				return frame2bcd(tc, static_cast<byte>(format_desc.time_scale / format_desc.duration));
			}

			int bcd_to_frame(BMDTimecodeBCD bcd)
			{
				auto format_desc = format_desc_;
				return bcd2frame(bcd, static_cast<byte>(format_desc.time_scale / format_desc.duration));
			}

			void open_deck_control(caspar::core::video_format_desc format)
			{
				if (FAILED(deck_control_->Open(format.time_scale, format.duration, FALSE, &last_deck_error_)))
					CASPAR_LOG(error) << print() << L" Could not open deck control";
				format_desc_ = format;
			}

			void begin_vcr_recording()
			{
				auto channel = channel_;
				auto consumer = consumer_;
				if (channel && consumer != core::frame_consumer::empty())
				{
					executor_.begin_invoke([=]
					{
						channel->output()->add(consumer);
					});
					record_state_ = record_state::vcr_recording;
				}
			}

			std::wstring print() const
			{
				return L"[decklink-recorder] [" + get_model_name(decklink_) + L":" + boost::lexical_cast<std::wstring>(index_) + L"]";
			}


		public:
			decklink_recorder(int index, int device_index, unsigned int preroll, int offset)
				: index_(index)
				, decklink_(get_device(device_index))
				, deck_control_(get_deck_control(decklink_))
				, consumer_(core::frame_consumer::empty())
				, last_deck_error_(bmdDeckControlNoError)
				, device_index_(device_index)
				, preroll_(preroll)
				, offset_(offset)
				, monitor_subject_(make_safe<core::monitor::subject>("/recorder/" + boost::lexical_cast<std::string>(index)))
				, executor_(print())
			{
				current_timecode_ = 0;
				executor_.set_capacity(1);
				if (FAILED(deck_control_->SetCallback(this)))
					CASPAR_LOG(error) << print() << L" Could not setup callback";
				if (FAILED(deck_control_->SetPreroll(preroll)))
					CASPAR_LOG(warning) << print() << L" Could not set deck preroll time";
				open_deck_control(caspar::core::video_format_desc::get(caspar::core::video_format::pal));
				CASPAR_LOG(debug) << print() << L" Initialized";
			}
			
			~decklink_recorder()
			{
				deck_control_->Close(FALSE);
			}

			virtual int index() const override {
				return index_;
			}


			virtual void Capture(std::shared_ptr<core::video_channel>& channel, const std::wstring tc_in, const std::wstring tc_out, const std::wstring file_name, const bool narrow_aspect_ratio, const core::parameters& params) override
			{
				Abort();
				caspar::core::video_format_desc new_format_desc = channel->get_video_format_desc();
				if (new_format_desc.time_scale != format_desc_.time_scale || new_format_desc.duration != format_desc_.duration)
				{
					CASPAR_LOG(trace) << print() << L" Video format has changed. Reopening deck control for new time scale";
					deck_control_->Close(false);
					open_deck_control(new_format_desc);
				}

				tc_in_ = bcd_to_frame(encode_timecode(tc_in));
				tc_out_ = bcd_to_frame(encode_timecode(tc_out));
				channel_ = channel;
				
				consumer_ = ffmpeg::create_capture_consumer(file_name, params, tc_in_, tc_out_, narrow_aspect_ratio, this);
				if (FAILED(deck_control_->StartCapture(FALSE, tc_to_bcd(tc_in_), tc_to_bcd(tc_out_), &last_deck_error_)))
				{
					CASPAR_LOG(error) << print() << L" Could not start capture";
					clean_recorder();
				}
			}

			virtual void Capture(std::shared_ptr<core::video_channel>& channel, const unsigned int frame_limit, const std::wstring file_name, const bool narrow_aspect_ratio, const core::parameters& params) override
			{
				channel_ = channel;
				consumer_ = ffmpeg::create_manual_record_consumer(file_name, params, frame_limit, narrow_aspect_ratio, this);
				executor_.begin_invoke([=] {
					channel->output()->add(consumer_);
				});
				record_state_ = record_state::manual_recording;
			}

			// Method called back from ffmpeg_consumer only
			void frame_captured(const unsigned int frames_left) override
			{
				*monitor_subject_ << core::monitor::message("/frames_left") % static_cast<int>(frames_left);
				if (!frames_left)
				{
					clean_recorder();
					executor_.begin_invoke([&]() {
						*monitor_subject_ << core::monitor::message("/control") % std::string("capture_complete");
					});					
				}
			}

			virtual bool FinishCapture() override
			{
				clean_recorder();
				if (record_state_ > record_state::idle)
					deck_control_->Stop(&last_deck_error_);
				executor_.begin_invoke([&]() {
					*monitor_subject_ << core::monitor::message("/control") % std::string("capture_complete");
				});
				return true;
			}

			virtual bool Abort() override
			{
				if (record_state_ > record_state::idle)
					deck_control_->Stop(&last_deck_error_);
				clean_recorder();
				return (SUCCEEDED(deck_control_->Abort()));
			}

			virtual bool Play() override
			{
				return (SUCCEEDED(deck_control_->Play(&last_deck_error_)));
			}
			
			virtual bool Stop() override
			{
				return (SUCCEEDED(deck_control_->Stop(&last_deck_error_)));
			}
			
			virtual bool FastForward() override
			{
				return (SUCCEEDED(deck_control_->FastForward(FALSE, &last_deck_error_)));
			}
			
			virtual bool Rewind() override
			{
				return (SUCCEEDED(deck_control_->Rewind(FALSE, &last_deck_error_)));
			}

			virtual bool GoToTimecode(const std::wstring tc) override
			{
				return (SUCCEEDED(deck_control_->GoToTimecode(tc_to_bcd(encode_timecode(tc)), &last_deck_error_)));
			}

#pragma region IDeckLinkDeckControlStatusCallback

			STDMETHOD(QueryInterface(REFIID, LPVOID*)) { return E_NOINTERFACE; }
			STDMETHOD_(ULONG, AddRef()) { return 1; }
			STDMETHOD_(ULONG, Release()) { return 1; }

			STDMETHOD(TimecodeUpdate(BMDTimecodeBCD currentTimecode))
			{
				*monitor_subject_ << core::monitor::message("/tc") % decode_timecode(currentTimecode);
				return S_OK;
			}

			STDMETHOD(VTRControlStateChanged(BMDDeckControlVTRControlState newState, BMDDeckControlError error))
			{
				if (record_state_ == record_state::vcr_recording)
					Abort();
				switch (newState)
				{
				case bmdDeckControlNotInVTRControlMode:
					*monitor_subject_ << core::monitor::message("/state") % std::string("not_vtr_control");
					break;
				case bmdDeckControlVTRControlPlaying:
					*monitor_subject_ << core::monitor::message("/state") % std::string("playing");
					break;
				case bmdDeckControlVTRControlRecording:
					*monitor_subject_ << core::monitor::message("/state") % std::string("recording");
					break;
				case bmdDeckControlVTRControlStill:
					*monitor_subject_ << core::monitor::message("/state") % std::string("still");
					break;
				case bmdDeckControlVTRControlShuttleForward:
					*monitor_subject_ << core::monitor::message("/state") % std::string("shuttle_forward");
					break;
				case bmdDeckControlVTRControlShuttleReverse:
					*monitor_subject_ << core::monitor::message("/state") % std::string("shuttle_reverse");
					break;
				case bmdDeckControlVTRControlJogForward:
					*monitor_subject_ << core::monitor::message("/state") % std::string("jog_forward");
					break;
				case bmdDeckControlVTRControlJogReverse:
					*monitor_subject_ << core::monitor::message("/state") % std::string("jog_reverse");
					break;
				case bmdDeckControlVTRControlStopped:
					*monitor_subject_ << core::monitor::message("/state") % std::string("stopped");
					break;
				}
				CASPAR_LOG(trace) << print() << L" VTR Control state changed: " << widen(STATE_TO_STR(newState));
				return S_OK;
			}

			STDMETHOD(DeckControlEventReceived(BMDDeckControlEvent e, BMDDeckControlError error))
			{
				if (record_state_ == record_state::vcr_recording)
					Abort();
				// this thread is unable to initialize FFmpeg consumer
				if (e == bmdDeckControlPrepareForCaptureEvent)
					begin_vcr_recording();
				switch (e)
				{
				case bmdDeckControlPrepareForExportEvent:
					*monitor_subject_ << core::monitor::message("/control") % std::string("export_prepare");
					break;
				case bmdDeckControlExportCompleteEvent:
					*monitor_subject_ << core::monitor::message("/control") % std::string("export_complete");
				case bmdDeckControlAbortedEvent:
					*monitor_subject_ << core::monitor::message("/control") % std::string("aborted");
					break;
				case bmdDeckControlPrepareForCaptureEvent:
					*monitor_subject_ << core::monitor::message("/control") % std::string("capture_prepare");
					break;
				case bmdDeckControlCaptureCompleteEvent:
					*monitor_subject_ << core::monitor::message("/control") % std::string("capture_complete");
					break;
				}
				CASPAR_LOG(trace) << print() << L" Event received: " << widen(EVT_TO_STR(e));
				return S_OK;
			}

			STDMETHOD(DeckControlStatusChanged(BMDDeckControlStatusFlags flags, unsigned int mask))
			{
				if (!deck_connected_ && (flags & bmdDeckControlStatusDeckConnected))
				{
					deck_connected_ = true;
					*monitor_subject_ << core::monitor::message("/connected") % std::string("true");
					CASPAR_LOG(info) << print() << L" Deck connected ";
				}
				if (deck_connected_ && !(flags & bmdDeckControlStatusDeckConnected))
				{
					deck_connected_ = false;
					*monitor_subject_ << core::monitor::message("/connected") % std::string("false");
					CASPAR_LOG(info) << print() << L" Deck disconnected ";
				}
				if (record_state_ == record_state::vcr_recording)
					Abort();
				CASPAR_LOG(trace) << print() << L" Deck control status changed: " << widen(FLAGS_TO_STR(flags));
				return S_OK;
			}

#pragma endregion

			int GetTimecode() override
			{
				BMDTimecodeBCD timecode_bcd;
				if (deck_control_ && SUCCEEDED(deck_control_->GetTimecodeBCD(&timecode_bcd, &last_deck_error_)))
				{
					auto format_desc = format_desc_;
					current_timecode_ = bcd_to_frame(timecode_bcd) - offset_;
					return current_timecode_;
				}
				else
					return ++current_timecode_; //assuming 1 call per frame
			}

			virtual void SetFrameLimit(unsigned int frame_limit) override
			{
				auto consumer = consumer_;
				if (consumer != core::frame_consumer::empty())
					ffmpeg::set_time_limit(consumer, frame_limit);
			}

			virtual boost::property_tree::wptree info() override
			{
				boost::property_tree::wptree info;
				info.add(L"recorder-kind", L"decklink");
				info.add(L"device", device_index_);
				info.add(L"preroll", preroll_);
				info.add(L"connected", deck_connected_);
				return info;
			}

			virtual core::monitor::subject& monitor_output() override
			{
				return *monitor_subject_;
			}

		};
		
		safe_ptr<core::recorder> create_recorder(int index, const boost::property_tree::wptree& ptree)
		{
			auto device_index = ptree.get(L"device", 1);
			auto preroll = ptree.get(L"preroll", 3U);
			int offset = ptree.get(L"offset", 0);
			return make_safe<decklink_recorder>(index, device_index, preroll, offset);
		}

	}
}