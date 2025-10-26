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
* Author: Jerzy Jaœkiewicz, jurek.jaskiewicz@gmail.com using GPT-5
*/
#include "../StdAfx.h"
#include "scte35_packet_writer.h"
#include "../ffmpeg_error.h"

extern "C" {
#include <libavutil/crc.h>
}

#define WRITE_BE32(p,v) do { *(p)++ = uint8_t(((v)>>24)&0xFF); *(p)++ = uint8_t(((v)>>16)&0xFF); *(p)++ = uint8_t(((v)>>8)&0xFF); *(p)++ = uint8_t((v)&0xFF); } while(0)
#define WRITE_LE32(p,v) do { *(p)++ = uint8_t((v)&0xFF); *(p)++ = uint8_t(((v)>>8)&0xFF); *(p)++ = uint8_t(((v)>>16)&0xFF); *(p)++ = uint8_t(((v)>>24)&0xFF); } while(0)
#define WRITE_BE16(p,v) do { *(p)++ = uint8_t(((v)>>8)&0xFF); *(p)++ = uint8_t((v)&0xFF); } while(0)
#define WRITE_U8(p,v)   do { *(p)++ = uint8_t((v)&0xFF);} while(0)

static const size_t MAX_SCTE35_SECTION_SIZE = 64;

namespace caspar { namespace ffmpeg {

/*
inline std::wstring hex_dump(const uint8_t* data, size_t size)
{
	if (!data || size == 0)
		return L"(empty)";
	std::wostringstream oss;
	oss << std::uppercase << std::setfill(L'0');
	for (size_t i = 0; i < size; i++)
	{
		oss << std::setw(2) << std::hex << static_cast<unsigned>(data[i]) << L' ';
	}
	return oss.str();
}
*/

struct scte35_packet_writer::implementation {

	AVStream*        stream_;
	AVFormatContext* format_context_;

	implementation(AVFormatContext* fmt, int stream_id)
		: format_context_(fmt)
		, stream_(avformat_new_stream(fmt, nullptr))
	{
		if (!stream_)
			BOOST_THROW_EXCEPTION(caspar_exception()
				<< msg_info("Could not allocate SCTE-35 stream")
				<< boost::errinfo_api_function("avformat_new_stream"));

		stream_->codecpar->codec_id   = AV_CODEC_ID_SCTE_35;
		stream_->codecpar->codec_type = AVMEDIA_TYPE_DATA;

		// Request PMT registration descriptor 'CUEI' (descriptor tag 0x05, format_identifier 'C','U','E','I')
		stream_->codecpar->codec_tag  = MKTAG('C','U','E','I');
		stream_->time_base = av_make_q(1, 90000);
		stream_->id        = stream_id;
	}

	void write_network_out_splice(uint32_t splice_event_id,
		uint16_t unique_program_id,
		bool immediate,
		uint64_t splice_time_us,
		uint64_t current_time_us,
		uint64_t duration_us,
		bool auto_return)
	{
		build_and_write_splice_insert(
			/*out_of_network*/ true,
			splice_event_id,
			unique_program_id,
			immediate,
			duration_us,
			auto_return,
			splice_time_us,
			current_time_us);
	}

	void write_network_in_splice(uint32_t splice_event_id,
		uint16_t unique_program_id,
		bool immediate,
		uint64_t splice_time_us,
		uint64_t current_time_us)
	{
		build_and_write_splice_insert(
			/*out_of_network*/ false,
			splice_event_id,
			unique_program_id,
			immediate,
			0,
			false,
			splice_time_us,
			current_time_us);
	}

	void write_cancel_splice(uint32_t splice_event_id, uint64_t current_time_us)
	{
		uint8_t buffer[MAX_SCTE35_SECTION_SIZE];
		uint8_t* p = buffer;

		WRITE_U8(p, 0xFC); // table_id

		uint8_t* section_length_ptr = p;
		WRITE_BE16(p, 0x0000); // placeholder

		WRITE_U8(p, 0x00); // protocol_version
		WRITE_U8(p, 0x00); // encrypted_packet + encryption_algorithm + pts_adjustment[32 high]
		WRITE_BE32(p, 0x00000000); // pts_adjustment low 32 bits
		WRITE_U8(p, 0xFF); // cw_index

		const uint16_t splice_command_length = 5;
		const uint16_t tier = 0x0FFF;
		uint32_t tier_and_len = ((tier & 0x0FFFU) << 20) | ((splice_command_length & 0x0FFFU) << 8) | 0x05U;
		WRITE_BE32(p, tier_and_len);

		WRITE_BE32(p, splice_event_id);
		WRITE_U8(p, 0xFF); // cancel indicator

		WRITE_BE16(p, 0x000); // descriptor_loop_length

		uint16_t section_length = static_cast<uint16_t>((p - buffer) + 1);
		section_length_ptr[0] = 0x30 | (section_length >> 8);
		section_length_ptr[1] = section_length & 0xFF;

		write_crc(buffer, &p);
		write_packet(buffer, int(p - buffer), current_time_us);
	}

	void build_and_write_splice_insert(
		bool out_of_network,
		uint32_t splice_event_id,
		uint16_t unique_program_id,
		bool immediate,
		uint64_t break_duration_us,
		bool auto_return,
		uint64_t splice_time_us,
		uint64_t current_time_us)
	{
		uint8_t buffer[MAX_SCTE35_SECTION_SIZE];
		uint8_t* p = buffer;

		WRITE_U8(p, 0xFC); // table_id
		uint8_t* section_length_ptr = p;
		WRITE_BE16(p, 0x0000); // placeholder for section length

		WRITE_U8(p, 0x00); // protocol_version
		WRITE_U8(p, 0x00); // encrypted_packet + encryption_algorithm + pts_adjustment[32 high]
		WRITE_BE32(p, 0x00000000); // pts_adjustment low 32 bits
		WRITE_U8(p, 0xFF); // cw_index

		// Build splice_insert command body into temp to compute its length.
		uint8_t cmd[48];
		uint8_t* c = cmd;

		WRITE_BE32(c, splice_event_id);
		WRITE_U8(c, 0x7F); // splice_event_cancel_indicator=0

		uint8_t flags = 0;
		if (out_of_network) flags |= 0x80;
		flags |= 0x40; // program_splice_flag
		if (break_duration_us) flags |= 0x20;
		if (immediate) flags |= 0x10;
		flags |= 0x0F; // reserved
		WRITE_U8(c, flags);

		if (!immediate)
		{
			uint64_t pts_90k = av_rescale_q(splice_time_us, AV_TIME_BASE_Q, stream_->time_base);
			uint8_t first = 0xFE | ((pts_90k >> 32) & 0x01);
			WRITE_U8(c, first);
			WRITE_BE32(c, static_cast<uint32_t>(pts_90k & 0xFFFFFFFFULL));
		}

		if (break_duration_us)
		{
			uint64_t dur_90k = av_rescale_q(break_duration_us, AV_TIME_BASE_Q, stream_->time_base);
			if (dur_90k > 0x1FFFFFFFFULL) // trim if longer than 33 bits
				dur_90k = 0x1FFFFFFFFULL;
			uint8_t b0 = (auto_return ? 0x80 : 0x00) | 0x7E | ((dur_90k >> 32) & 0x01);
			// Explanation:
			// bit7 auto_return
			// bits6-1 reserved (set to 1 => 0x7E when OR with auto_return bit cleared)
			// bit0 duration[32]
			WRITE_U8(c, b0);
			WRITE_BE32(c, static_cast<uint32_t>(dur_90k & 0xFFFFFFFFULL));
		}

		WRITE_BE16(c, unique_program_id);
		WRITE_U8(c, 0x00); // avail_num
		WRITE_U8(c, 0x00); // avails_expected

		size_t splice_command_length = c - cmd;

		const uint16_t tier = 0x0FFF;
		uint32_t tier_and_len = ((tier & 0x0FFF) << 20)
			| ((splice_command_length & 0x0FFF) << 8)
			| 0x05; // splice_insert
		WRITE_BE32(p, tier_and_len);

		std::memcpy(p, cmd, splice_command_length);
		p += splice_command_length;

		WRITE_BE16(p, 0x0000); // descriptor_loop_length - empty
		/*
		// alternative below - single descriptor example
		WRITE_BE16(p, 0x000A); // descriptor_loop_length
		WRITE_U8(p, 0x00); // splice_descriptor_tag -> avail_descriptor
		WRITE_U8(p, 0x08); // descriptor_length
		WRITE_BE32(p, 0x43554549); // "CUEI"
		WRITE_BE32(p, 0x00000000); // private bytes -> provider_avail_id
		*/

		size_t total_with_crc = size_t(p - buffer) + 1;
		if (total_with_crc > 0x0FFF)
			return;

		uint16_t section_length = static_cast<uint16_t>(total_with_crc);
		section_length_ptr[0] = 0x30 | (section_length >> 8);
		section_length_ptr[1] = section_length & 0xFF;

		write_crc(buffer, &p);
		write_packet(buffer, int(p - buffer), current_time_us);
	}

	void write_crc(uint8_t* start, uint8_t** end)
	{
		size_t len = size_t(*end - start);
		if (len + 4 > MAX_SCTE35_SECTION_SIZE)
			return;
		uint32_t crc = av_crc(av_crc_get_table(AV_CRC_32_IEEE), 0xFFFFFFFF, start, len);
		WRITE_LE32(*end, crc);
	}

	void write_packet(uint8_t* data, int size, uint64_t current_time_us)
	{
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = data;
		pkt.size = size;
		pkt.stream_index = stream_->index;
		pkt.flags = AV_PKT_FLAG_KEY;
		int64_t ts = av_rescale_q(current_time_us, AV_TIME_BASE_Q, stream_->time_base);
		pkt.pts = pkt.dts = ts;
		LOG_ON_ERROR2(av_write_frame(format_context_, &pkt), L"scte-35 writer");
	}
};

scte35_packet_writer::scte35_packet_writer(AVFormatContext* format_ctx, int stream_id)
	: impl_(new implementation(format_ctx, stream_id)) {}

void scte35_packet_writer::write_network_out_splice(uint32_t splice_event_id,
	uint16_t unique_program_id,
	bool immediate,
	uint64_t splice_time_us,
	uint64_t current_time_us,
	uint64_t duration_us,
	bool auto_return)
{
	impl_->write_network_out_splice(splice_event_id, unique_program_id, immediate,
		splice_time_us, current_time_us, duration_us, auto_return);
}

void scte35_packet_writer::write_network_in_splice(uint32_t splice_event_id,
	uint16_t unique_program_id,
	bool immediate,
	uint64_t splice_time_us,
	uint64_t current_time_us)
{
	impl_->write_network_in_splice(splice_event_id, unique_program_id, immediate,
		splice_time_us, current_time_us);
}

void scte35_packet_writer::write_cancel_splice(uint32_t splice_event_id, uint64_t current_time_us)
{
	impl_->write_cancel_splice(splice_event_id, current_time_us);
}

AVRational scte35_packet_writer::time_base() const
{
	return impl_->stream_->time_base;
}

}} // namespace
