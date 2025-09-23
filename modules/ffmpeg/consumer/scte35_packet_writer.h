#pragma once

namespace caspar {
	namespace ffmpeg {

		class scte35_packet_writer : boost::noncopyable
		{
		public:
			explicit scte35_packet_writer(AVFormatContext* format_ctx, int stream_id);

			// All time parameters are microseconds (AV_TIME_BASE domain).
			void write_network_out_splice(uint32_t splice_event_id,
				uint16_t unique_program_id,
				bool immediate,
				uint64_t splice_time_us,
				uint64_t current_time_us,
				uint64_t duration_us,
				bool auto_return);

			void write_network_in_splice(uint32_t splice_event_id,
				uint16_t unique_program_id,
				bool immediate,
				uint64_t splice_time_us,
				uint64_t current_time_us);

			void write_cancel_splice(uint32_t splice_event_id, uint64_t current_time_us);

			AVRational time_base() const;

		private:
			struct implementation;
			safe_ptr<implementation> impl_;
		};

	}
}
