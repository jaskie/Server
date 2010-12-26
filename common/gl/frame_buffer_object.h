#pragma once

#include <Glee.h>

#include <memory>

namespace caspar { namespace gl {

class frame_buffer_object
{
public:
	frame_buffer_object(size_t width, size_t height, GLenum mode = GL_COLOR_ATTACHMENT0_EXT);
	void bind_pixel_source();
	size_t width() const;
	size_t height() const;
private:
	struct implementation;
	std::shared_ptr<implementation> impl_;
};
typedef frame_buffer_object fbo;

}}