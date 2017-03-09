/*
* This file is part of TVP's fork of CasparCG (www.casparcg.com).
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
*/

#pragma once

#include <common/memory/safe_ptr.h>


#include <boost/property_tree/ptree.hpp>

#include <string>
#include <vector>

namespace caspar {

	namespace core {
		class recorder;
		class video_channel;
	}

	namespace decklink {
		safe_ptr<core::recorder> create_recorder(int id, safe_ptr<core::video_channel> channel, const boost::property_tree::wptree& ptree);
	}
}
