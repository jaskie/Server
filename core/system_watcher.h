/*
* This file is part of TVP's fork CasparCG (www.casparcg.com).
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

#include <memory>

namespace caspar {
	namespace core {
		typedef std::function<void()> watcher_callback_t;
		void init_system_watcher(const boost::property_tree::wptree& pt);
		void register_callback(watcher_callback_t& callback);
	}
}