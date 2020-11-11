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

#include "../../stdafx.h"

#include "channel_producer.h"

#include "../../monitor/monitor.h"
#include "../../consumer/frame_consumer.h"
#include "../../consumer/output.h"
#include "../../video_channel.h"

#include "../frame/basic_frame.h"
#include "../frame/frame_factory.h"
#include "../../mixer/write_frame.h"
#include "../../mixer/read_frame.h"

#include <common/exception/exceptions.h>
#include <common/memory/memcpy.h>
#include <common/concurrency/future_util.h>

#include <tbb/concurrent_queue.h>

namespace caspar { namespace core {

class channel_consumer : public frame_consumer
{	
	tbb::concurrent_bounded_queue<std::pair<std::shared_ptr<read_frame>, bool>>	frame_buffer_; // bool: is repeated
	core::video_format_desc										format_desc_;
	tbb::atomic<int>											channel_index_;
	tbb::atomic<bool>											is_running_;
	tbb::atomic<int64_t>										current_age_;

public:
	channel_consumer() 
	{
		is_running_ = true;
		current_age_ = 0;
		frame_buffer_.set_capacity(3);
	}

	~channel_consumer()
	{
		stop();
	}

	// frame_consumer

	virtual boost::unique_future<bool> send(const safe_ptr<read_frame>& frame) override
	{
		while (is_running_ && !frame_buffer_.try_push(std::make_pair(frame, true)))
		{
			std::pair<std::shared_ptr<read_frame>, bool> recycled_frame;
			frame_buffer_.pop(recycled_frame);
		}
		return caspar::wrap_as_future(is_running_.load());
	}

	virtual void initialize(const core::video_format_desc& format_desc, const channel_layout& audio_channel_layout, int channel_index) override
	{
		format_desc_    = format_desc;
		channel_index_  = channel_index;
	}

	virtual int64_t presentation_frame_age_millis() const override
	{
		return current_age_;
	}

	virtual std::wstring print() const override
	{
		return L"[channel-consumer|" + boost::lexical_cast<std::wstring>(channel_index_) + L"]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"channel-consumer");
		info.add(L"channel-index", channel_index_);
		return info;
	}
	
	virtual bool has_synchronization_clock() const override
	{
		return false;
	}

	virtual uint32_t buffer_depth() const override
	{
		return frame_buffer_.size();
	}

	virtual int index() const override
	{
		return 78500 + channel_index_;
	}
	
	int channel_index() const
	{
		return channel_index_;
	}

	// channel_consumer

	void stop()
	{
		is_running_ = false;
		frame_buffer_.try_push(std::make_pair(make_safe<read_frame>(), true));
	}
	
	const core::video_format_desc& get_video_format_desc()
	{
		return format_desc_;
	}

	std::pair<std::shared_ptr<read_frame>, bool> receive()
	{
		if(!is_running_)
			return std::make_pair(make_safe<read_frame>(), true);
		std::pair<std::shared_ptr<read_frame>, bool> frame;
		
		if (frame_buffer_.try_pop(frame))
			current_age_ = frame.first->get_age_millis();

		return frame;
	}
};
	
class channel_producer : public frame_producer
{
	monitor::subject					monitor_subject_;

	const safe_ptr<frame_factory>		frame_factory_;
	const safe_ptr<channel_consumer>	consumer_;
	const video_format_desc&			chanel_video_format_desc_;

	safe_ptr<basic_frame>				last_frame_;
	uint64_t							frame_number_;
	

public:
	explicit channel_producer(const safe_ptr<frame_factory>& frame_factory, const safe_ptr<video_channel>& channel) 
		: frame_factory_(frame_factory)
		, consumer_(make_safe<channel_consumer>())
		, last_frame_(basic_frame::empty())
		, frame_number_(0)
		, chanel_video_format_desc_(channel->get_video_format_desc())
	{
		if (frame_factory->get_video_format_desc().time_scale * channel->get_video_format_desc().duration != channel->get_video_format_desc().time_scale * frame_factory->get_video_format_desc().duration)
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not initialize channel producer for another if frame rate differs"));
		channel->output()->add(consumer_);
		CASPAR_LOG(info) << print() << L" Initialized";
	}

	~channel_producer()
	{
		consumer_->stop();
		CASPAR_LOG(info) << print() << L" Uninitialized";
	}

	// frame_producer
			
	virtual safe_ptr<basic_frame> receive(int) override
	{

		std::pair<std::shared_ptr<read_frame>, bool> read_frame = consumer_->receive();
		if (!read_frame.first)
			return basic_frame::late();

		frame_number_++;
		
		core::pixel_format_desc desc;

		desc.pix_fmt = core::pixel_format::bgra;
		desc.planes.push_back(core::pixel_format_desc::plane(chanel_video_format_desc_.width, chanel_video_format_desc_.height, 4));
		auto frame = frame_factory_->create_frame(this, desc);
		if (read_frame.first)
		{
			frame->audio_data().reserve(read_frame.first->audio_data().size());
			boost::copy(read_frame.first->audio_data(), std::back_inserter(frame->audio_data()));
		}
		fast_memcpy(frame->image_data().begin(), read_frame.first->image_data().begin(), read_frame.first->image_data().size());
		frame->commit();
		last_frame_ = frame;
		return frame;
	}	

	virtual safe_ptr<basic_frame> last_frame() const override
	{
		return disable_audio(last_frame_); 
	}	

	virtual std::wstring print() const override
	{
		return L"channel[" + boost::lexical_cast<std::wstring>(consumer_->channel_index()) + L"]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"channel-producer");
		return info;
	}

	monitor::subject& monitor_output() 
	{
		return monitor_subject_;
	}
};

safe_ptr<frame_producer> create_channel_producer(const safe_ptr<core::frame_factory>& frame_factory, const safe_ptr<video_channel>& channel)
{
	return create_producer_print_proxy(
			make_safe<channel_producer>(frame_factory, channel));
}

}}