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

// TODO: Smart GC

#include "../../stdafx.h"

#include "ogl_device.h"

#include "shader.h"

#include <common/exception/exceptions.h>
#include <common/utility/assert.h>
#include <common/gl/gl_check.h>
#include <common/env.h>

#include <boost/foreach.hpp>

#include <gl/wglew.h>

#include <SFML/Window/Window.hpp>

namespace caspar { namespace core {

ogl_device::ogl_device(int gpu_index) 
	: executor_(L"ogl_device")
	, pattern_(nullptr)
	, attached_texture_(0)
	, attached_fbo_(0)
	, active_shader_(0)
	, read_buffer_(0)
	, offscreen_rendering_context_(NULL)
{
	CASPAR_LOG(info) << L"Initializing OpenGL Device.";

	std::fill(binded_textures_.begin(), binded_textures_.end(), 0);
	std::fill(viewport_.begin(), viewport_.end(), 0);
	std::fill(scissor_.begin(), scissor_.end(), 0);
	std::fill(blend_func_.begin(), blend_func_.end(), 0);
	
	invoke([=]
	{
		context_.reset(new sf::Context());
		context_->SetActive(true);

		if (glewInit() != GLEW_OK)
			BOOST_THROW_EXCEPTION(gl::ogl_exception() << msg_info("Failed to initialize GLEW."));		
		if (gpu_index >=0)
			if (WGLEW_NV_gpu_affinity)
			{
				CASPAR_LOG(trace) << L"WGLEW_NV_gpu_affinity supported, selecting GPU " << gpu_index << L" to render on.";
				HGPUNV hGPU[2];
				if (wglEnumGpusNV(gpu_index, &hGPU[0]))
				{
					GPU_DEVICE gpuDevice;
					if (wglEnumGpuDevicesNV(hGPU[0], 0, &gpuDevice))
						CASPAR_LOG(info) << L"Selected OpenGL device: " << gpuDevice.DeviceString << L" on " << gpuDevice.DeviceName;
					hGPU[1] = NULL;
					HDC affDC = wglCreateAffinityDCNV(hGPU);
					PIXELFORMATDESCRIPTOR pfd;
					int pf = ChoosePixelFormat(affDC, &pfd);
					SetPixelFormat(affDC, pf, &pfd);
					DescribePixelFormat(affDC, pf, sizeof(PIXELFORMATDESCRIPTOR), &pfd);
					offscreen_rendering_context_ = wglCreateContext(affDC);
					if (!wglMakeCurrent(affDC, offscreen_rendering_context_))
						CASPAR_LOG(error) << L"Unable to set OpenGL context.";
				}
				else
					CASPAR_LOG(error) << L"Selected OpenGL device not found.";
			}
			else
				CASPAR_LOG(error) << L"Cannot select GPU " << gpu_index << L" to render on, WGLEW_NV_gpu_affinity not supported";


		CASPAR_LOG(info) << L"OpenGL " << version();

		if(!GLEW_VERSION_3_0)
			BOOST_THROW_EXCEPTION(gl::ogl_exception() << msg_info("Your graphics card does not meet the minimum hardware requirements since it does not support OpenGL 3.0 or higher. CasparCG Server will not be able to continue."));
	
		glGenFramebuffers(1, &fbo_);	
		
		CASPAR_LOG(info) << L"Successfully initialized OpenGL Device.";
	});
}

ogl_device::~ogl_device()
{
	invoke([=]
	{
		BOOST_FOREACH(auto& pool, device_pools_)
			pool.clear();
		BOOST_FOREACH(auto& pool, host_pools_)
			pool.clear();
		glDeleteFramebuffers(1, &fbo_);
		wglMakeCurrent(NULL, NULL);
		if (offscreen_rendering_context_)
			wglDeleteContext(offscreen_rendering_context_);
	});
}

safe_ptr<device_buffer> ogl_device::allocate_device_buffer(uint32_t width, uint32_t height, uint32_t stride)
{
	std::shared_ptr<device_buffer> buffer;
	try
	{
		buffer.reset(new device_buffer(width, height, stride));
	}
	catch(...)
	{
		try
		{
			yield();
			auto future = gc();
			yield();
			future.wait();
					
			// Try again
			buffer.reset(new device_buffer(width, height, stride));
		}
		catch(...)
		{
			CASPAR_LOG(error) << L"ogl: create_device_buffer failed!";
			throw;
		}
	}
	return make_safe_ptr(buffer);
}
				
safe_ptr<device_buffer> ogl_device::create_device_buffer(uint32_t width, uint32_t height, uint32_t stride)
{
	CASPAR_VERIFY(stride > 0 && stride < 5);
	CASPAR_VERIFY(width > 0 && height > 0);
	auto& pool = device_pools_[stride-1][((width << 16) & 0xFFFF0000) | (height & 0x0000FFFF)];
	std::shared_ptr<device_buffer> buffer;
	if(!pool->items.try_pop(buffer))		
		buffer = executor_.invoke([&]{return allocate_device_buffer(width, height, stride);}, high_priority);			
	
	//++pool->usage_count;

	return safe_ptr<device_buffer>(buffer.get(), [=](device_buffer*) mutable
	{		
		pool->items.push(buffer);	
	});
}

safe_ptr<host_buffer> ogl_device::allocate_host_buffer(uint32_t size, usage_t usage)
{
	std::shared_ptr<host_buffer> buffer;

	try
	{
		buffer.reset(new host_buffer(size, usage));
		if(usage == write_only)
			buffer->map();
		else
			buffer->unmap();			
	}
	catch(...)
	{
		try
		{
			yield();
			auto future = gc();
			yield();
			future.wait();

			// Try again
			buffer.reset(new host_buffer(size, usage));
			if(usage == write_only)
				buffer->map();
			else
				buffer->unmap();	
		}
		catch(...)
		{
			CASPAR_LOG(error) << L"ogl: create_host_buffer failed!";
			throw;		
		}
	}

	return make_safe_ptr(buffer);
}
	
safe_ptr<host_buffer> ogl_device::create_host_buffer(uint32_t size, usage_t usage)
{
	CASPAR_VERIFY(usage == write_only || usage == read_only);
	CASPAR_VERIFY(size > 0);
	auto& pool = host_pools_[usage][size];
	std::shared_ptr<host_buffer> buffer;
	if(!pool->items.try_pop(buffer))	
		buffer = executor_.invoke([=]{return allocate_host_buffer(size, usage);}, high_priority);	
	
	//++pool->usage_count;

	auto self = shared_from_this();
	return safe_ptr<host_buffer>(buffer.get(), [=](host_buffer*) mutable
	{
		self->executor_.begin_invoke([=]() mutable
		{		
			if(usage == write_only)
				buffer->map();
			else
				buffer->unmap();

			pool->items.push(buffer);
		}, high_priority);	
	});
}

safe_ptr<ogl_device> ogl_device::create()
{
	int gpu_index = env::properties().get(L"configuration.mixer.gpu-index", -1);
	return safe_ptr<ogl_device>(new ogl_device(gpu_index));
}

//template<typename T>
//void flush_pool(buffer_pool<T>& pool)
//{	
//	if(pool.flush_count.fetch_and_increment() < 16)
//		return;
//
//	if(pool.usage_count.fetch_and_store(0) < pool.items.size())
//	{
//		std::shared_ptr<T> buffer;
//		pool.items.try_pop(buffer);
//	}
//
//	pool.flush_count = 0;
//	pool.usage_count = 0;
//}

void ogl_device::flush()
{
	GL(glFlush());	
		
	//try
	//{
	//	BOOST_FOREACH(auto& pools, device_pools_)
	//	{
	//		BOOST_FOREACH(auto& pool, pools)
	//			flush_pool(*pool.second);
	//	}
	//	BOOST_FOREACH(auto& pools, host_pools_)
	//	{
	//		BOOST_FOREACH(auto& pool, pools)
	//			flush_pool(*pool.second);
	//	}
	//}
	//catch(...)
	//{
	//	CASPAR_LOG_CURRENT_EXCEPTION();
	//}
}

void ogl_device::yield()
{
	executor_.yield();
}

boost::unique_future<void> ogl_device::gc()
{	
	return begin_invoke([=]
	{
		CASPAR_LOG(info) << " ogl: Running GC.";		
	
		try
		{
			BOOST_FOREACH(auto& pools, device_pools_)
			{
				BOOST_FOREACH(auto& pool, pools)
					pool.second->items.clear();
			}
			BOOST_FOREACH(auto& pools, host_pools_)
			{
				BOOST_FOREACH(auto& pool, pools)
					pool.second->items.clear();
			}
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
		}
	}, high_priority);
}

std::wstring ogl_device::version()
{	
	static std::wstring ver = L"Not found";
	try
	{
		ver = widen(invoke([]{return std::string(reinterpret_cast<const char*>(glGetString(GL_VERSION)));})
		+ " "	+ invoke([]{return std::string(reinterpret_cast<const char*>(glGetString(GL_VENDOR)));}));			
	}
	catch(...){}

	return ver;
}


void ogl_device::enable(GLenum cap)
{
	auto& val = caps_[cap];
	if(!val)
	{
		glEnable(cap);
		val = true;
	}
}

void ogl_device::disable(GLenum cap)
{
	auto& val = caps_[cap];
	if(val)
	{
		glDisable(cap);
		val = false;
	}
}

void ogl_device::viewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	if(x != viewport_[0] || y != viewport_[1] || width != viewport_[2] || height != viewport_[3])
	{		
		glViewport(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width), static_cast<GLsizei>(height));
		viewport_[0] = x;
		viewport_[1] = y;
		viewport_[2] = width;
		viewport_[3] = height;
	}
}

void ogl_device::scissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	if(x != scissor_[0] || y != scissor_[1] || width != scissor_[2] || height != scissor_[3])
	{		
		glScissor(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width), static_cast<GLsizei>(height));
		scissor_[0] = x;
		scissor_[1] = y;
		scissor_[2] = width;
		scissor_[3] = height;
	}
}

void ogl_device::stipple_pattern(const GLubyte* pattern)
{
	if(pattern_ != pattern)
	{		
		glPolygonStipple(pattern);
		pattern_ = pattern;
	}
}

void ogl_device::attach(device_buffer& texture)
{	
	if(attached_texture_ != texture.id())
	{
		if(attached_fbo_ != fbo_)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
			attached_fbo_ = fbo_;
		}

		GL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 0, GL_TEXTURE_2D, texture.id(), 0));
		attached_texture_ = texture.id();
	}
}

void ogl_device::clear(device_buffer& texture)
{	
	attach(texture);
	GL(glClear(GL_COLOR_BUFFER_BIT));
}

void ogl_device::read_buffer(device_buffer&)
{
	if(read_buffer_ != GL_COLOR_ATTACHMENT0)
	{
		GL(glReadBuffer(GL_COLOR_ATTACHMENT0));
		read_buffer_ = GL_COLOR_ATTACHMENT0;
	}
}

void ogl_device::use(shader& shader)
{
	if(active_shader_ != shader.id())
	{		
		GL(glUseProgramObjectARB(shader.id()));	
		active_shader_ = shader.id();
	}
}

void ogl_device::blend_func(int c1, int c2, int a1, int a2)
{
	std::array<int, 4> func = {c1, c2, a1, a2};

	if(blend_func_ != func)
	{
		blend_func_ = func;
		GL(glBlendFuncSeparate(c1, c2, a1, a2));
	}
}

void ogl_device::blend_func(int c1, int c2)
{
	blend_func(c1, c2, c1, c2);
}

}}

