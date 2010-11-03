#pragma once

#include "frame_format.h"

#include <memory>

#include <boost/noncopyable.hpp>

#include <Glee.h>

#include <boost/tuple/tuple.hpp>

namespace caspar { namespace core {
	
struct rectangle
{
	rectangle(double left, double top, double right, double bottom)
		: left(left), top(top), right(right), bottom(bottom)
	{}
	double left;
	double top;
	double right;
	double bottom;
};

class gpu_frame : boost::noncopyable
{
public:
	gpu_frame(size_t width, size_t height);
	virtual ~gpu_frame(){}
	virtual void write_lock();
	virtual bool write_unlock();
	virtual void read_lock(GLenum mode);
	virtual bool read_unlock();
	virtual void draw();
		
	virtual unsigned char* data();
	virtual size_t size() const;
	virtual size_t width() const;
	virtual size_t height() const;
	
	virtual void reset();
			
	virtual const std::vector<short>& audio_data() const;	
	virtual std::vector<short>& audio_data();

	virtual double alpha() const;
	virtual void alpha(double value);

	virtual double x() const;
	virtual double y() const;
	virtual void translate(double x, double y);
	virtual void texcoords(const rectangle& texcoords);

	virtual void mode(video_mode mode);
	virtual video_mode mode() const;

	static std::shared_ptr<gpu_frame> null()
	{
		static auto my_null_frame = std::make_shared<gpu_frame>(0,0);
		return my_null_frame;
	}
		
private:
	struct implementation;
	std::shared_ptr<implementation> impl_;
};
typedef std::shared_ptr<gpu_frame> gpu_frame_ptr;
	
}}