#pragma once

namespace caspar {
	namespace core {

		enum SignalType {
			Out,
			In,
			Cancel
		};

		struct splice_signal
		{
			splice_signal(SignalType signal_type, uint32_t event_id, uint16_t program_id, int32_t frames_to_event, uint32_t break_duration, bool auto_return)
				: signal_type(signal_type)
				, event_id(event_id)
				, program_id(program_id)
				, frames_to_event(frames_to_event)
				, break_duration(break_duration)
				, auto_return(auto_return)
				, is_new(true)
			{ }

			SignalType signal_type;

			const uint32_t event_id;

			const uint16_t program_id;

			// time to start, in frames
			int32_t frames_to_event;

			bool is_new;

			// duration in frames, zero if unspecified
			const uint32_t break_duration;

			const bool auto_return;

			bool tick()
			{
				frames_to_event--;
				if (is_new)
				{
					is_new = false;
					return true;
				}
				return false;
			}
		};
	}
}