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

#define SHADER_PROGRAM(prog)    #prog

namespace caspar { namespace core {

class shader;
class ogl_device;

struct texture_id
{
	enum type
	{
		plane0 = 0,
		plane1,
		plane2,
		plane3,
		local_key,
		layer_key,
		background,
	};
};

safe_ptr<shader> get_image_shader(
		ogl_device& ogl, bool& blend_modes, bool& post_processing);


}}