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

#pragma once

#include <cstdint>
#include <string>

namespace caspar { namespace flash {

std::string read_template_meta_info(const std::wstring& filename);

struct swf_t
{
	struct header_t
	{
		header_t(const std::wstring& filename);

		std::array<std::uint8_t, 3> signature;
		std::uint8_t				version;
		std::uint32_t				file_length;
		std::uint32_t				frame_width;
		std::uint32_t				frame_height;
		std::uint16_t				frame_rate;
		std::uint16_t				frame_count;

		bool						valid;

	} header;
	
	std::vector<char>				data;
	
	swf_t(const std::wstring& filename);
};

}}