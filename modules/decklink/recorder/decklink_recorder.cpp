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





namespace caspar {
	namespace decklink {

		CComPtr<IDeckLinkDeckControl>  get_deck_control(const CComPtr<IDeckLink>& decklink)
		{
			CComPtr<IDeckLinkDeckControl> result;
			decklink->QueryInterface(IID_IDeckLinkDeckControl, (void**)&result);
			return result;
		}

		class decklink_recorder : public core::recorder, IDeckLinkDeckControlStatusCallback
		{
		private:
			enum record_state
			{
				idle,
				opening,
				prerolling,
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
			}

			void start_capture()
			{

			}

			void begin_recording()
			{

			}

			void finish_recording()
			{

			}

		public:
			decklink_recorder(int index, int device_index) 
				: index_(index)
				, decklink_(get_device(device_index))
				, deck_control_(get_deck_control(decklink_))
				, consumer_(nullptr)
				, current_timecode_(0)
				, last_deck_error_(bmdDeckControlNoError)
			{
				deck_control_->SetCallback(this);
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
				caspar::core::video_format_desc format_desc = channel->get_video_format_desc();
				if (FAILED(deck_control_->Open(1, 0, FALSE, &last_deck_error_)))
				{
					CASPAR_LOG(error) << L"Could not open deck control ";
				}
				record_state_ = record_state::opening;
				auto consumer = ffmpeg::create_recorder_consumer(file_name, params);
				consumer_ = &consumer;
			}

			virtual void Abort()
			{
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