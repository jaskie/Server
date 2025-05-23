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
* Author: Nicklas P Andersson
*/

 
#pragma once

namespace caspar { namespace protocol {
namespace cii {

class ICIICommand
{
public:
	virtual ~ICIICommand() {}
	virtual int GetMinimumParameters() = 0;
	virtual void Setup(const std::vector<std::wstring>& parameters) = 0;

	virtual void Execute() = 0;
};
typedef std::tr1::shared_ptr<ICIICommand> CIICommandPtr;

}	//namespace cii
}}	//namespace caspar