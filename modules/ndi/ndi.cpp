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

#include "ndi.h" 

#include <core/parameters/parameters.h>
#include <core/producer/frame_producer.h>
#include <core/consumer/frame_consumer.h>
#include "consumer/ndi_consumer.h"

#include <common/utility/string.h>

#include <Processing.NDI.Lib.h>
#include <windows.h>


namespace caspar {
	namespace ndi {

		void init()
		{
			const NDIlib_v2* p_NDILib = NDIlib_v2_load();
			if (!p_NDILib || !p_NDILib->NDIlib_initialize())
			{	// Cannot run NDI. Most likely because the CPU is not sufficient (see SDK documentation).
				// you can check this directly with a call to NDIlib_is_supported_CPU()
				printf("Cannot run NDI.");
				return;
			}
			p_NDILib->NDIlib_destroy();
			core::register_consumer_factory([](const core::parameters& params)
			{
				return create_ndi_consumer(params);
			});
		}

		std::wstring get_version()
		{
			return L"0.1";
		}

	}
}