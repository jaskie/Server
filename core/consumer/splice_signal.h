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

			splice_signal(const splice_signal& other)
				: signal_type(other.signal_type)
				, event_id(other.event_id)
				, program_id(other.program_id)
				, frames_to_event(other.frames_to_event)
				, break_duration(other.break_duration)
				, auto_return(other.auto_return)
				, is_new(other.is_new)
			{ }

			splice_signal& operator=(const splice_signal& other)
			{
				if (this != &other)
				{
					signal_type = other.signal_type;
					event_id = other.event_id;
					program_id = other.program_id;
					frames_to_event = other.frames_to_event;
					break_duration = other.break_duration;
					auto_return = other.auto_return;
					is_new = other.is_new;
				}
				return *this;
			}

			SignalType signal_type;

			uint32_t event_id;

			uint16_t program_id;

			// time to start, in frames
			int32_t frames_to_event;

			bool is_new;

			// duration in frames, zero if unspecified
			uint32_t break_duration;

			bool auto_return;

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

			bool operator==(const splice_signal& other) const
			{
				return signal_type == other.signal_type
					&& event_id == other.event_id
					&& program_id == other.program_id;
			}

			bool operator!=(const splice_signal& other) const
			{
				return !(*this == other);
			}
		};
	}
}