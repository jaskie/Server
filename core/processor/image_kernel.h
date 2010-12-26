#pragma once

#include "../format/pixel_format.h"
#include "../format/video_format.h"

#include <memory>

namespace caspar { namespace core {
		
class image_kernel
{
public:
	image_kernel();
	void use(pixel_format::type pix_fmt, video_mode::type mode);

private:
	struct implementation;
	std::shared_ptr<implementation> impl_;
};

}}