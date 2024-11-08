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
* Author: Jerzy Ja�kiewicz, jurek@tvp.pl based on Robert Nagy, ronag89@gmail.com work
*/

#include "ndi.h" 
#include "util/ndi_util.h"
#include <core/consumer/frame_consumer.h>
#include <core/producer/frame_producer.h>
#include "consumer/ndi_consumer.h"
#include "producer/ndi_producer.h"
#include <common/log/log.h>
#include <common/utility/string.h>


namespace caspar {
	namespace ndi {
		

		void init()
		{
			auto ndi_lib = load_ndi();
			if (!ndi_lib)
			{
				CASPAR_LOG(info) << L"Newtek NDI library not found.";
				return;
			}
			if (!ndi_lib->NDIlib_initialize())
			{
				CASPAR_LOG(info) << L"Newtek NDI unable to initialize. This may be caused by an unsupported CPU.";
				return;
			}
			ndi_lib->NDIlib_destroy();
			core::register_consumer_factory(create_consumer);
			core::register_producer_factory([](const safe_ptr<core::frame_factory>& factory, const core::parameters& params) { return ndi::create_producer(factory, params); });

		}

		std::wstring get_version()
		{
			auto ndi_lib = load_ndi();
			if (!ndi_lib)
				return L"Unavailable";
			return widen(ndi_lib->NDIlib_version());
		}

	}
}