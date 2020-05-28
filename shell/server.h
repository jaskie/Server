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

#include <core/monitor/monitor.h>

#include <boost/noncopyable.hpp>
#include <boost/thread/future.hpp>

#include <vector>

namespace caspar {

namespace core {
	class video_channel;
	class recorder;
	class thumbnail_generator;
	struct media_info_repository;
}

class server : boost::noncopyable
{
public:
	server();
	const std::vector<safe_ptr<core::video_channel>> get_channels() const;
	const std::vector<safe_ptr<core::recorder>> get_recorders() const;
	std::shared_ptr<core::thumbnail_generator> get_thumbnail_generator() const;
	safe_ptr<core::media_info_repository> get_media_info_repo() const;

	core::monitor::subject& monitor_output();

private:
	struct implementation;
	safe_ptr<implementation> impl_;
};

}