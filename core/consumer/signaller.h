#pragma once

#include <boost/noncopyable.hpp>
#include <boost/rational.hpp>
#include <common/memory/safe_ptr.h>
#include "splice_signal.h"

namespace caspar {
	namespace core {
		struct video_format_desc;

		class signaller : boost::noncopyable
		{
		public:
			explicit signaller(const video_format_desc& e);
			bool signal_out(uint32_t event_id, uint16_t program_id, uint32_t frames_to_out, uint32_t duration, bool auto_return);
			bool signal_in(uint32_t event_id, uint16_t program_id, uint32_t frames_to_in);
			bool signal_cancel(uint32_t event_id);
			std::vector<splice_signal> tick();
		private:
			struct implementation;
			safe_ptr<implementation> impl_;
		};
	}
}