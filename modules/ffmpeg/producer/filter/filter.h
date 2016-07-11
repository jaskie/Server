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

#include <common/memory/safe_ptr.h>

#include <boost/noncopyable.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/rational.hpp>

#include <string>
#include <vector>

struct AVFrame;
enum AVPixelFormat;

namespace caspar { namespace ffmpeg {

static std::string append_filter(const std::string& filters, const std::string& filter)
{
	return filters + (filters.empty() ? "" : ",") + filter;
}

class filter : boost::noncopyable
{
public:
	filter(
		int in_width,
		int in_height,
		boost::rational<int> in_time_base,
		boost::rational<int> in_frame_rate,
		boost::rational<int> in_sample_aspect_ratio,
		AVPixelFormat in_pix_fmt,
		std::vector<AVPixelFormat> out_pix_fmts,
		const std::string& filtergraph);
	filter(filter&& other);
	filter& operator=(filter&& other);

	void push(const std::shared_ptr<AVFrame>& frame);
	std::shared_ptr<AVFrame> poll();
	std::vector<safe_ptr<AVFrame>> poll_all();
	void clear();
	bool is_frame_format_changed(const std::shared_ptr<AVFrame>& frame);

	std::string filter_str() const;
			
private:
	struct implementation;
	safe_ptr<implementation> impl_;
};

}}