/*
* Copyright 2017 Telewizja Polska
*
* This file is part of TVP's CasparCG fork.
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
* Author: Jerzy Jaœkiewicz, jurek@tvp.pl based on Robert Nagy, ronag89@gmail.com work
*/

#pragma once

#include <common/memory/safe_ptr.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/filesystem.hpp>

#include <string>
#include <vector>

namespace caspar {

	namespace core {
		struct frame_consumer;
		class parameters;
		class read_frame;
		struct video_format;
		struct video_format_desc;
	}

	namespace ndi {
		safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params);
		safe_ptr<core::frame_consumer> create_ndi_consumer(const boost::property_tree::wptree& ptree);
	}
}