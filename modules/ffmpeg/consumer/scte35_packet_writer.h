/*
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
* Author: Jerzy Jaœkiewicz, jurek.jaskiewicz@gmail.com
*/

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
