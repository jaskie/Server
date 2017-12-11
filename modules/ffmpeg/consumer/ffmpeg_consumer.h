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

#include <boost/property_tree/ptree.hpp>

#include <string>
#include <vector>
#include "../common/memory/safe_ptr.h"

namespace caspar {

	namespace core {
		struct frame_consumer;
		class parameters;
		class recorder;
	}

	namespace ffmpeg {
		safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params);
		safe_ptr<core::frame_consumer> create_consumer(const boost::property_tree::wptree& ptree);
		safe_ptr<core::frame_consumer> create_capture_consumer(const std::wstring filename, const core::parameters& params, const int tc_in, const int tc_out, bool narrow_aspect_ratio, core::recorder* const recorder);
		safe_ptr<core::frame_consumer> create_manual_record_consumer(const std::wstring filename, const core::parameters& params, const unsigned int frame_limit, bool narrow_aspect_ratio, core::recorder* const recorder);
		void set_frame_limit(const safe_ptr<core::frame_consumer>& consumer, unsigned int frame_limit);
	}
}