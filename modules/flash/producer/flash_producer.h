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

#include <core/producer/frame_producer.h>

#include <common/memory/safe_ptr.h>

#include <vector>
#include <string>

namespace caspar { namespace flash {

safe_ptr<core::frame_producer> create_producer(const safe_ptr<core::frame_factory>& frame_factory, const std::vector<std::wstring>& params);
safe_ptr<core::frame_producer> create_swf_producer(
		const safe_ptr<core::frame_factory>& frame_factory,
		const core::parameters& params);

std::wstring find_template(const std::wstring& templateName);

}}