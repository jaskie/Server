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

#include <vector>

namespace caspar { namespace core {
		
struct pixel_format
{
	enum type
	{
		gray = 0,
		bgra,
		rgba,
		argb,
		abgr,
		ycbcr,
		ycbcra,
		luma,
		count,
		invalid
	};
};

struct pixel_format_desc
{
	struct plane
	{
		uint32_t linesize;
		uint32_t width;
		uint32_t height;
		uint32_t size;
		uint32_t channels;

		plane() 
			: linesize(0)
			, width(0)
			, height(0)
			, size(0)
			, channels(0){}

		plane(uint32_t width, uint32_t height, uint32_t channels)
			: linesize(width*channels)
			, width(width)
			, height(height)
			, size(width*height*channels)
			, channels(channels){}
	};

	pixel_format_desc() : pix_fmt(pixel_format::invalid){}
	
	pixel_format::type pix_fmt;
	std::vector<plane> planes;

	static const pixel_format_desc& invalid()
	{
		const static pixel_format_desc desc;
		return desc;
	}
};

}}