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


#include <core/parameters/parameters.h>
#include <core/video_channel.h>
#include <common/memory/safe_ptr.h>

#include <boost/noncopyable.hpp>
#include <boost/property_tree/ptree.hpp>

#include <agents.h>

namespace caspar {
	namespace core {

		class recorder : boost::noncopyable
		{
		public:

			// Static Members

			// Constructors

			// Methods
			virtual void Capture(const std::shared_ptr<core::video_channel>& channel, const std::wstring tc_in, const std::wstring tc_out, const std::wstring file_name, const bool narrow_aspect_ratio, const core::parameters& params) = 0;
			virtual void Capture(const std::shared_ptr<core::video_channel>& channel, const unsigned int frame_limit, const std::wstring file_name, const bool narrow_aspect_ratio, const core::parameters& params) = 0;
			virtual bool FinishCapture() = 0;
			virtual bool Abort() = 0;
			virtual bool Play() = 0;
			virtual bool Stop() = 0;
			virtual bool FastForward() = 0;
			virtual bool Rewind() = 0;
			virtual bool GoToTimecode(const std::wstring tc) = 0;
			virtual int GetTimecode() = 0;
			virtual void SetFrameLimit(unsigned int frame_limit) = 0;

			// internal methods
			virtual void frame_captured(const unsigned int frames_left) = 0;
			virtual boost::property_tree::wptree info() = 0;
			virtual monitor::subject& monitor_output() = 0;
			// Properties

			virtual int index() const 
			{
				return 0;
			};
		};
	}
}