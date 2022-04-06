
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

#include "StdAfx.h"
#include "system_watcher.h"
#include "common/concurrency/executor.h"

namespace caspar { namespace core {

	class system_watcher
	{
	public:
		explicit system_watcher()
			: executor_(L"System watcher")
		{
		}

		void init()
		{
			tick();
		}

		void register_callback(watcher_callback_t& callback)
		{
			executor_.invoke([&] {watcher_callbacks_.push_back(callback); });
		}

		void tick()
		{
			executor_.begin_invoke([&] {
				try
				{
					for (auto callback_it = watcher_callbacks_.begin(); callback_it != watcher_callbacks_.end(); callback_it++)
						(*callback_it)();
				}
				catch (...)
				{
					CASPAR_LOG_CURRENT_EXCEPTION();
				}
				boost::this_thread::sleep(boost::posix_time::seconds(10));
				tick();
				});
		}

	private:
		executor executor_;
		std::vector<watcher_callback_t> watcher_callbacks_;
	};


	system_watcher g_watcher;

	void init_system_watcher(const boost::property_tree::wptree& pt)
	{
		g_watcher.init();
	}

	void register_callback(watcher_callback_t& callback)
	{
		g_watcher.register_callback(callback);
	}





}}