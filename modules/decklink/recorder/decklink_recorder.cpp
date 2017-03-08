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
#include <ffmpeg/consumer/ffmpeg_consumer.h>

#include <boost/algorithm/string.hpp>





namespace caspar {
	namespace decklink {

		CComPtr<IDeckLinkDeckControl>  get_deck_control(const CComPtr<IDeckLink>& decklink)
		{
			CComPtr<IDeckLinkDeckControl> result;
			decklink->QueryInterface(IID_IDeckLinkDeckControl, (void**)&result);
			return result;
		}

		BMDTimecodeBCD decode_timecode(std::wstring tc)
		{
			std::vector<std::wstring> split;
			boost::split(split, tc, boost::is_any_of(":."));
			if (split.size() == 4)
			{
				unsigned int result =  boost::lexical_cast<unsigned int>(split[3])
					| (boost::lexical_cast<unsigned int>(split[2]) << 8)
					| (boost::lexical_cast<unsigned int>(split[1]) << 16)
					| (boost::lexical_cast<unsigned int>(split[0]) << 24);
				return result;
			}
			return 0;
		}
		

		class decklink_recorder : public core::recorder, IDeckLinkDeckControlStatusCallback
		{
		private:
			enum record_state
			{
				idle,
				opening,
				prerolling,
				winding,
				recording,
			}									record_state_;
			com_initializer						init_;
			int									index_;
			CComPtr<IDeckLink>					decklink_;
			CComPtr<IDeckLinkDeckControl>		deck_control_;
			BMDDeckControlError					last_deck_error_;

			//fields of the current operation
			safe_ptr<core::video_channel> *		channel_;
			std::wstring						file_name_;
			safe_ptr<core::frame_consumer> *	consumer_;
			unsigned int						tc_in_;
			unsigned int						tc_out_;

			BMDTimecodeBCD						current_timecode_;


			void clean_recorder()
			{
				auto channel = channel_;
				auto consumer = consumer_;
				if (channel && consumer)
					(*channel)->output()->remove(*consumer);

				consumer_ = nullptr;
				channel_ = nullptr;
				file_name_ = L"";
				tc_in_ = 0;
				tc_out_ = 0;
				record_state_ = record_state::idle;
			}

			void start_capture()
			{
				if (FAILED(deck_control_->StartCapture(FALSE, tc_in_, tc_out_, &last_deck_error_)))
				{
					CASPAR_LOG(error) << print() << L" Could not start capture";
					Abort();
				}
				else
					record_state_ = record_state::winding;
			}

			void begin_recording()
			{

			}

			void finish_recording()
			{

			}

			std::wstring print() const
			{
				return L"decklink-recorder [" + get_model_name(decklink_) + L":" + boost::lexical_cast<std::wstring>(index_) + L"]";
			}


		public:
			decklink_recorder(int index, int device_index)
				: index_(index)
				, decklink_(get_device(device_index))
				, deck_control_(get_deck_control(decklink_))
				, consumer_(nullptr)
				, current_timecode_(0)
				, last_deck_error_(bmdDeckControlNoError)
				, tc_in_(0)
				, tc_out_(0)
			{
				if (FAILED(deck_control_->SetCallback(this)))
					CASPAR_LOG(error) << print() << L" Could not open deck control";
				else
				{

					CASPAR_LOG(error) << print() << L" Initialized";
				}
			}
			
			~decklink_recorder()
			{
				deck_control_->Close(FALSE);
			}

			virtual int index() const {
				return index_;
			}

			virtual void Capture(std::shared_ptr<core::video_channel> channel, std::wstring tc_in, std::wstring tc_out, int preroll, int offset, std::wstring file_name, const core::parameters& params)
			{
				Abort();
				caspar::core::video_format_desc format_desc = channel->get_video_format_desc();
				if (FAILED(deck_control_->Open(format_desc.time_scale, format_desc.duration, FALSE, &last_deck_error_)))
				{
					CASPAR_LOG(error) << print() << L" Could not open deck control";
					return;
				}
				tc_in_ = decode_timecode(tc_in);
				tc_out_ = decode_timecode(tc_out);
				record_state_ = record_state::opening;
				auto consumer = ffmpeg::create_recorder_consumer(file_name, params);
				consumer_ = &consumer;
			}

			virtual void Abort()
			{
				if (record_state_ > record_state::idle)
					deck_control_->Close(FALSE);
				if (record_state_ > record_state::opening)
					deck_control_->Stop(&last_deck_error_);
			}

#pragma region IDeckLinkDeckControlStatusCallback

			STDMETHOD(QueryInterface(REFIID, LPVOID*)) { return E_NOINTERFACE; }
			STDMETHOD_(ULONG, AddRef()) { return 1; }
			STDMETHOD_(ULONG, Release()) { return 1; }

			STDMETHOD(TimecodeUpdate(BMDTimecodeBCD currentTimecode))
			{
				current_timecode_ = currentTimecode;
				if (record_state_ == record_state::winding && currentTimecode == tc_in_)
				{
					(*channel_)->output()->add(*consumer_);
					record_state_ = record_state::recording;
				}
				if (record_state_ == record_state::recording && currentTimecode == tc_out_)
				{
					clean_recorder();
				}
				return S_OK;
			}

			STDMETHOD(VTRControlStateChanged(BMDDeckControlVTRControlState newState, BMDDeckControlError error))
			{

				return S_OK;
			}

			STDMETHOD(DeckControlEventReceived(BMDDeckControlEvent event, BMDDeckControlError error))
			{
				
				return S_OK;
			}

			STDMETHOD(DeckControlStatusChanged(BMDDeckControlStatusFlags flags, unsigned int mask))
			{
				if (record_state_ == record_state::opening && (flags & bmdDeckControlStatusDeckConnected))
				{
					record_state_ = record_state::prerolling;
					start_capture();
				}
				return S_OK;
			}

#pragma endregion

		};


		safe_ptr<core::recorder> create_recorder(int index, const boost::property_tree::wptree& ptree)
		{
			auto device_index = ptree.get(L"device", 1);
			return make_safe<decklink_recorder>(index, device_index);
		}

	}
}