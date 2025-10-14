#include "../StdAfx.h"
#include "signaller.h"
#include "../video_format.h"
#include <boost/rational.hpp>
#include <boost/thread/mutex.hpp>

namespace caspar {
	namespace core {
		struct signaller::implementation
		{
			const boost::rational<int> frame_rate_;
			const std::vector<int32_t> when_send_signal_;
			std::vector<std::shared_ptr<splice_signal>> pending_signals_;
			boost::mutex mutex_;

			implementation(const video_format_desc& video_format)
				: frame_rate_(video_format.time_scale, video_format.duration)
				, when_send_signal_(when_send_signal(video_format.time_scale, video_format.duration))
			{ }

			bool signal_out(uint32_t event_id, uint16_t program_id, int32_t frames_to_out, uint32_t duration, bool auto_return)
			{
				boost::unique_lock<boost::mutex> lock(mutex_);
				if (validate_time(frames_to_out, 100) && validate_time(duration, 24*60*60))
				{
					pending_signals_.emplace_back(std::make_shared<splice_signal>(SignalType::Out, event_id, program_id, frames_to_out, duration, auto_return));
					CASPAR_LOG(trace) << L"Scheduled splice_signal OUT.";
					return true;
				}
				CASPAR_LOG(warning) << L"Unable to schedule splice_signal OUT: time validation failed.";
				return false;
			}

			bool signal_in(uint32_t event_id, uint16_t program_id, uint32_t frames_to_in)
			{
				boost::unique_lock<boost::mutex> lock(mutex_);
				if (validate_time(frames_to_in, 100))
				{
					pending_signals_.emplace_back(std::make_shared<splice_signal>(SignalType::In, event_id, program_id, frames_to_in, 0, false));
					CASPAR_LOG(trace) << L"Scheduled splice_signal IN.";
					return true;
				}
				CASPAR_LOG(warning) << L"Unable to schedule splice_signal IN: time validation failed.";
				return false;
			}

			bool signal_cancel(uint32_t event_id)
			{
				boost::unique_lock<boost::mutex> lock(mutex_);
				auto signal_it = std::find_if(pending_signals_.begin(), pending_signals_.end(), [=](const std::shared_ptr<splice_signal>& signal) { return signal->event_id == event_id; });
				if (signal_it == pending_signals_.end())
				{
					CASPAR_LOG(warning) << L"Unable to cancel splice_signal with event_id: " << event_id << L" - not found.";
					return false;
				}
				const auto& signal = *signal_it;
				if (!signal->is_new && signal->frames_to_event <= when_send_signal_[0])
				{
					pending_signals_.erase(std::remove(pending_signals_.begin(), pending_signals_.end(), signal), pending_signals_.end());
					pending_signals_.emplace_back(std::make_shared<splice_signal>(SignalType::Cancel, event_id, 0, 0, 0, false));
					CASPAR_LOG(trace) << L"Scheduled splice_signal CANCEL.";
				}
				else
					CASPAR_LOG(info) << L"splice_signal CANCEL not scheduled - not yet notified.";
				return true;
			}

			std::vector<std::shared_ptr<splice_signal>> tick()
			{
				boost::unique_lock<boost::mutex> lock(mutex_);
				std::vector<std::shared_ptr<splice_signal>> result;
				if (!pending_signals_.empty())
				{
					std::vector<std::shared_ptr<splice_signal>> signals_to_remove;
					for (auto it = pending_signals_.begin(); it != pending_signals_.end(); ++it)
					{
						const auto& signal = *it;
						if ((signal->tick() && (signal->frames_to_event <= when_send_signal_[0])) || // if signal is notified after first time to send
							std::find(when_send_signal_.begin(), when_send_signal_.end(), signal->frames_to_event) != when_send_signal_.end())
							result.push_back(signal);
						if (signal->frames_to_event <= 0)
							signals_to_remove.push_back(signal);
					}
					for (auto it = signals_to_remove.begin(); it != signals_to_remove.end(); ++it)
						pending_signals_.erase(std::remove(pending_signals_.begin(), pending_signals_.end(), *it), pending_signals_.end());
				}
				return result;
			}

			bool validate_time(int frames, int max_seconds)
			{
				return ((frames * frame_rate_.denominator()) / frame_rate_.numerator()) < max_seconds;
			}

			std::vector<int32_t> when_send_signal(uint32_t time_scale, uint32_t duration) const
			{
				std::vector<int32_t> result(5);
				result[0] = 4 * time_scale / duration; // 4s
				result[1] = 2 * time_scale / duration; // 2s
				result[2] = 1 * time_scale / duration; // 1s
				result[3] = time_scale / (2 * duration); // 0.5 s
				result[4] = 0; // at splice time
				return result;
			}
		};


		signaller::signaller(const video_format_desc& video_format) :
			impl_(new implementation(video_format)) {
		};
		bool signaller::signal_out(uint32_t event_id, uint16_t program_id, uint32_t frames_to_start, uint32_t duration, bool auto_return) { return impl_->signal_out(event_id, program_id, frames_to_start, duration, auto_return); }
		bool signaller::signal_in(uint32_t event_id, uint16_t program_id, uint32_t frames_to_finish) { return impl_->signal_in(event_id, program_id, frames_to_finish); }
		bool signaller::signal_cancel(uint32_t event_id) { return impl_->signal_cancel(event_id); }
		std::vector<std::shared_ptr<splice_signal>> signaller::tick() { return impl_->tick(); }
	}
}