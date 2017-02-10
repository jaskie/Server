#pragma once

#include <core/producer/frame_producer.h>
#include <string>
#include <vector>

namespace caspar {
namespace core {
	class video_channel;
	void initialize_output_producer(const std::vector<safe_ptr<video_channel>>& channels);
	}
}

