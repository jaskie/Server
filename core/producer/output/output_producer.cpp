
#include "../../StdAfx.h"
#include "output_producer.h"
#include "../frame/frame_factory.h"
#include "../../parameters/parameters.h"
#include "../frame/basic_frame.h"
#include "../../monitor/monitor.h"
#include "../../video_channel.h"
#include "../../consumer/output.h"
#include "../../mixer/write_frame.h"
#include "../../mixer/read_frame.h"
#include "../../mixer/mixer.h"
#include "../../video_format.h"
#include <core/mixer/audio/audio_util.h>


#include <common/exception/exceptions.h>
#include <common/concurrency/executor.h>
#include <common/concurrency/future_util.h>
#include <common/memory/memcpy.h>

#include <boost/lexical_cast.hpp>

#include <tbb/concurrent_queue.h>

namespace caspar {
	namespace core {

		class output_producer : public frame_producer, public frame_consumer
		{
			core::monitor::subject										monitor_subject_;
			const size_t												channel_index_;
			executor													executor_;
			tbb::concurrent_bounded_queue<safe_ptr<basic_frame>>		frame_buffer_;
			const safe_ptr<frame_factory>								frame_factory_;
			const video_format_desc										video_format_desc_;
			pixel_format_desc											pixel_format_desc_;
			const channel_layout										audio_channel_layout_;
			safe_ptr<basic_frame>										last_frame_;
		public:
			explicit output_producer(
				const safe_ptr<frame_factory>& frame_factory,
				const size_t channel_index,
				channel_layout audio_channel_layout,
				video_format_desc video_format
			) : channel_index_(channel_index)
			  , executor_(print())
			  , frame_factory_(frame_factory)
			  , audio_channel_layout_(audio_channel_layout)
			  , video_format_desc_(video_format)
			  , last_frame_(basic_frame::empty())
			{
				frame_buffer_.set_capacity(1);
			}

	
			virtual std::wstring print() const override
			{
				return L"channel[" + boost::lexical_cast<std::wstring>(channel_index_) + L"]";
			}
			
			virtual boost::property_tree::wptree info() const override
			{
				boost::property_tree::wptree info;
				info.add(L"type", L"output-producer");
				info.add(L"output", channel_index_);
				return info;
			}
			
			//Inherited via frame_producer

			virtual safe_ptr<basic_frame> receive(int hints) override
			{
				auto frame = basic_frame::empty();
				if (frame_buffer_.try_pop(frame))
					last_frame_ = frame;
				return frame;
			}
			virtual safe_ptr<basic_frame> last_frame() const override
			{
				return last_frame_;
			}

			virtual monitor::subject & monitor_output() override
			{
				return monitor_subject_;
			}

			// Inherited via frame_consumer
			virtual boost::unique_future<bool> send(const safe_ptr<read_frame>& frame) override
			{
				executor_.begin_invoke([&]
				{
					auto write = make_write_frame(frame);
					frame_buffer_.try_push(write);
				});
				return caspar::wrap_as_future(true);
			}

			virtual void initialize(const video_format_desc & format_desc, int channel_index) override
			{
				pixel_format_desc_.pix_fmt = caspar::core::pixel_format::bgra;
				pixel_format_desc_.planes.push_back(core::pixel_format_desc::plane(format_desc.width, format_desc.height, 4));
			}

			virtual int64_t presentation_frame_age_millis() const override
			{
				return 0;
			}
			virtual size_t buffer_depth() const override
			{
				return size_t();
			}
			virtual int index() const override
			{
				return 0;
			}

			safe_ptr<core::write_frame> make_write_frame(const safe_ptr<read_frame>& readed_frame)
			{
				safe_ptr<write_frame> write = frame_factory_->create_frame(this, pixel_format_desc_, audio_channel_layout_);
				write->set_type(field_mode::upper);
				for (int n = 0; n < static_cast<int>(pixel_format_desc_.planes.size()); ++n)
				{
					auto plane = pixel_format_desc_.planes[n];
					auto result = write->image_data(n).begin();
					auto readed = readed_frame->image_data();
					fast_memcpy(result, readed.begin(), plane.size);
					write->commit(n);
				}
				return write;
			}
		};


		safe_ptr<frame_producer> create_output_producer(
			const safe_ptr<frame_factory>& frame_factory,
			const parameters& params,
			const std::vector<safe_ptr<video_channel>>& channels)
		{
			if (params.size() == 2 && params[0] == L"CHANNEL")
			{
				auto channel_str = params[1];
				try
				{
					size_t in_channel_nr = boost::lexical_cast<size_t>(channel_str) - 1;
					if (in_channel_nr < channels.size())
					{
						auto channel_to_show = channels[in_channel_nr];
						auto the_producer = make_safe<output_producer>(frame_factory, in_channel_nr + 1, channel_to_show->get_channel_layot(), channel_to_show->get_video_format_desc());
						channel_to_show->output()->add(the_producer);
						return the_producer;
					}
				}
				catch (boost::bad_lexical_cast const&) {}
				BOOST_THROW_EXCEPTION(invalid_argument() << arg_name_info("channel") << arg_value_info(narrow(channel_str)) << msg_info("No such channel index."));
			}
			return core::frame_producer::empty();
		}

		void initialize_output_producer(const std::vector<safe_ptr<video_channel>>& channels)
		{
			producer_factory_t factory = [&](const safe_ptr<frame_factory>& factory, const parameters& params) {
				return create_output_producer(factory, params, channels);
			};
			register_producer_factory(factory);
		}	

		
		
	}
}