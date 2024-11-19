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

#include "util.h"

#include "flv.h"

#include "../../ffmpeg_error.h"

#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_queue.h>

#include <core/producer/frame/frame_transform.h>
#include <core/producer/frame/frame_factory.h>
#include <core/producer/frame_producer.h>
#include <core/mixer/write_frame.h>
#include <core/mixer/audio/audio_util.h>

#include <common/exception/exceptions.h>
#include <common/utility/assert.h>
#include <common/memory/memcpy.h>

#include <tbb/parallel_for.h>

#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>

namespace caspar { namespace ffmpeg {
		
std::shared_ptr<core::audio_buffer> flush_audio()
{
	static std::shared_ptr<core::audio_buffer> audio(new core::audio_buffer());
	return audio;
}
std::shared_ptr<core::audio_buffer> empty_audio()
{
	static std::shared_ptr<core::audio_buffer> audio(new core::audio_buffer());
	return audio;
}
std::shared_ptr<AVFrame>			flush_video()
{
	static safe_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame* frame){});
	return frame;
}
std::shared_ptr<AVFrame>			empty_video()
{
	static safe_ptr<AVFrame> frame(av_frame_alloc(), [](AVFrame* frame){});
	return frame;
}

std::shared_ptr<AVPacket>			flush_packet()
{
	static std::shared_ptr<AVPacket> packet(
		[]() -> AVPacket* { 
		AVPacket* p = av_packet_alloc(); 
		p->data = nullptr;
		p->size =  0;
		return p;
		}(),
		[](AVPacket* p){} //empty deletor
		);
	return packet;
}

safe_ptr<AVPacket> create_packet()
{
	safe_ptr<AVPacket> packet(av_packet_alloc(), [](AVPacket* p)
	{
		av_packet_free(&p);
	});
	return packet;
}

std::shared_ptr<AVFrame> create_frame()
{
	return std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); });
}

core::field_mode::type get_mode(const AVFrame& frame)
{
	if(!frame.interlaced_frame)
		return core::field_mode::progressive;

	return frame.top_field_first ? core::field_mode::upper : core::field_mode::lower;
}

core::pixel_format::type get_pixel_format(AVPixelFormat pix_fmt)
{
	switch(pix_fmt)
	{
	case CASPAR_PIX_FMT_LUMA:		return core::pixel_format::luma;
	case AV_PIX_FMT_GRAY8:			return core::pixel_format::gray;
	case AV_PIX_FMT_BGRA:			return core::pixel_format::bgra;
	case AV_PIX_FMT_ARGB:			return core::pixel_format::argb;
	case AV_PIX_FMT_RGBA:			return core::pixel_format::rgba;
	case AV_PIX_FMT_ABGR:			return core::pixel_format::abgr;
	case AV_PIX_FMT_YUV444P:		return core::pixel_format::ycbcr;
	case AV_PIX_FMT_YUV422P:		return core::pixel_format::ycbcr;
	case AV_PIX_FMT_YUV420P:		return core::pixel_format::ycbcr;
	case AV_PIX_FMT_YUV411P:		return core::pixel_format::ycbcr;
	case AV_PIX_FMT_YUV410P:		return core::pixel_format::ycbcr;
	case AV_PIX_FMT_YUVA420P:		return core::pixel_format::ycbcra;
	default:						return core::pixel_format::invalid;
	}
}

core::pixel_format_desc get_pixel_format_desc(AVPixelFormat pix_fmt, size_t width, size_t height)
{
	// Get linesizes
	size_t plane_size[4];
	int linesize[4];

	AVPixelFormat av_pix_format = (AVPixelFormat)(pix_fmt == CASPAR_PIX_FMT_LUMA ? AV_PIX_FMT_GRAY8 : pix_fmt);
	FF_RET(av_image_fill_linesizes(linesize, av_pix_format, width), "get_pixel_format_desc.av_image_fill_linesizes");
	FF_RET(av_image_fill_plane_sizes(plane_size, av_pix_format, height, linesize), "get_pixel_format_desc.av_image_fill_plane_sizes");

	core::pixel_format_desc desc;
	desc.pix_fmt = get_pixel_format(pix_fmt);
		
	switch(desc.pix_fmt)
	{
	case core::pixel_format::gray:
	case core::pixel_format::luma:
		{
			desc.planes.push_back(core::pixel_format_desc::plane(linesize[0], height, 1));
			return desc;
		}
	case core::pixel_format::bgra:
	case core::pixel_format::argb:
	case core::pixel_format::rgba:
	case core::pixel_format::abgr:
		{
			desc.planes.push_back(core::pixel_format_desc::plane(linesize[0]/4, height, 4));
			return desc;
		}
	case core::pixel_format::ycbcr:
	case core::pixel_format::ycbcra:
		{
			desc.planes.push_back(core::pixel_format_desc::plane(linesize[0], plane_size[0] / linesize[0], 1));
			desc.planes.push_back(core::pixel_format_desc::plane(linesize[1], plane_size[1] / linesize[1], 1));
			desc.planes.push_back(core::pixel_format_desc::plane(linesize[2], plane_size[2] / linesize[2], 1));
			if(desc.pix_fmt == core::pixel_format::ycbcra)
				desc.planes.push_back(core::pixel_format_desc::plane(linesize[3], plane_size[3] / linesize[3], 1));
			return desc;
		}
	default:
		desc.pix_fmt = core::pixel_format::invalid;
		return desc;
	}
}

int make_alpha_format(int format)
{
	switch(get_pixel_format(static_cast<AVPixelFormat>(format)))
	{
	case core::pixel_format::ycbcr:
	case core::pixel_format::ycbcra:
		return CASPAR_PIX_FMT_LUMA;
	default:
		return format;
	}
}

safe_ptr<core::write_frame> make_write_frame(const void* tag, const safe_ptr<AVFrame>& decoded_frame, const safe_ptr<core::frame_factory>& frame_factory, int hints, const core::channel_layout& audio_channel_layout)
{			
	static tbb::concurrent_unordered_map<int64_t, tbb::concurrent_queue<std::shared_ptr<SwsContext>>> sws_contexts_;
	
	if(decoded_frame->width < 1 || decoded_frame->height < 1)
		return make_safe<core::write_frame>(tag, audio_channel_layout);

	const auto width  = decoded_frame->width;
	const auto height = decoded_frame->height;
	auto desc		  = get_pixel_format_desc(static_cast<AVPixelFormat>(decoded_frame->format), width, height);
	
	if(hints & core::frame_producer::ALPHA_HINT)
		desc = get_pixel_format_desc(static_cast<AVPixelFormat>(make_alpha_format(decoded_frame->format)), width, height);

	std::shared_ptr<core::write_frame> write;

	if(desc.pix_fmt == core::pixel_format::invalid)
	{
		auto pix_fmt = static_cast<AVPixelFormat>(decoded_frame->format);
		auto target_pix_fmt = AV_PIX_FMT_BGRA;

		if(pix_fmt == AV_PIX_FMT_UYVY422)
			target_pix_fmt = AV_PIX_FMT_YUV422P;
		else if(pix_fmt == AV_PIX_FMT_YUYV422)
			target_pix_fmt = AV_PIX_FMT_YUV422P;
		else if(pix_fmt == AV_PIX_FMT_UYYVYY411)
			target_pix_fmt = AV_PIX_FMT_YUV411P;
		else if(pix_fmt == AV_PIX_FMT_YUV420P10)
			target_pix_fmt = AV_PIX_FMT_YUV420P;
		else if(pix_fmt == AV_PIX_FMT_YUV422P10)
			target_pix_fmt = AV_PIX_FMT_YUV422P;
		else if(pix_fmt == AV_PIX_FMT_YUV444P10)
			target_pix_fmt = AV_PIX_FMT_YUV444P;
		
		auto target_desc = get_pixel_format_desc(static_cast<AVPixelFormat>(target_pix_fmt), width, height);

		write = frame_factory->create_frame(tag, target_desc, audio_channel_layout);
		write->set_type(get_mode(*decoded_frame));
		if (decoded_frame->opaque_ref)
		{
			auto time = static_cast<frame_time*>(av_buffer_get_opaque(decoded_frame->opaque_ref));
			if (time)
				write->set_timecode(time->FrameNumber);
		}

		std::shared_ptr<SwsContext> sws_context;

		//CASPAR_LOG(warning) << "Hardware accelerated color transform not supported.";
		
		int64_t key = ((static_cast<int64_t>(width)			 << 32) & 0xFFFF00000000) | 
					  ((static_cast<int64_t>(height)		 << 16) & 0xFFFF0000) | 
					  ((static_cast<int64_t>(pix_fmt)		 <<  8) & 0xFF00) | 
					  ((static_cast<int64_t>(target_pix_fmt) <<  0) & 0xFF);

		auto& pool = sws_contexts_[key];

		if(!pool.try_pop(sws_context))
		{
			sws_context.reset(sws_getContext(width, height, static_cast<AVPixelFormat>(pix_fmt), width, height, target_pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, NULL), sws_freeContext);
			CASPAR_LOG(trace) << L"Created new SWS context w=" << width << L", h=" << height << ", input pix_fmt=" << pix_fmt << L", output pix_fmt=" << target_pix_fmt;
		}

		if(!sws_context)
		{
			BOOST_THROW_EXCEPTION(operation_failed() << msg_info("Could not create software scaling context.") << 
									boost::errinfo_api_function("sws_getContext"));
		}	

		std::shared_ptr<AVFrame> av_frame = create_frame();
		if(target_pix_fmt == AV_PIX_FMT_BGRA)
		{
			auto size = av_image_fill_arrays(av_frame->data, av_frame->linesize, write->image_data().begin(), AV_PIX_FMT_BGRA, width, height, 1);
			CASPAR_VERIFY(size == write->image_data().size()); 
		}
		else
		{
			av_frame->width	 = width;
			av_frame->height = height;
			for(size_t n = 0; n < target_desc.planes.size(); ++n)
			{
				av_frame->data[n]		= write->image_data(n).begin();
				av_frame->linesize[n]	= target_desc.planes[n].linesize;
			}
		}

		sws_scale(sws_context.get(), decoded_frame->data, decoded_frame->linesize, 0, height, av_frame->data, av_frame->linesize);	
		pool.push(sws_context);

		write->commit();
	}
	else
	{
		write = frame_factory->create_frame(tag, desc, audio_channel_layout);
		write->set_type(get_mode(*decoded_frame));
		auto time = static_cast<frame_time*>(av_buffer_get_opaque(decoded_frame->opaque_ref));
		if (time)
			write->set_timecode(time->FrameNumber);


		for(int n = 0; n < static_cast<int>(desc.planes.size()); ++n)
		{
			auto plane            = desc.planes[n];
			auto result           = write->image_data(n).begin();
			auto decoded          = decoded_frame->data[n];
			auto decoded_linesize = decoded_frame->linesize[n];
			
			CASPAR_ASSERT(decoded);
			CASPAR_ASSERT(write->image_data(n).begin());

			if(decoded_linesize != static_cast<int>(plane.linesize))
			{
				// Copy line by line since ffmpeg sometimes pads each line.
				tbb::parallel_for<size_t>(0, desc.planes[n].height, [&](size_t y)
				{
					fast_memcpy(result + y*plane.linesize, decoded + y*decoded_linesize, plane.linesize);
				});
			}
			else
			{
				fast_memcpy(result, decoded, plane.size);
			}

			write->commit(n);
		}
	}

	if(decoded_frame->height == 480) // NTSC DV
	{
		write->get_frame_transform().fill_translation[1] += 2.0/static_cast<double>(frame_factory->get_video_format_desc().height);
		write->get_frame_transform().fill_scale[1] = 1.0 - 6.0*1.0/static_cast<double>(frame_factory->get_video_format_desc().height);
	}
	
	// Fix field-order if needed
	if(write->get_type() == core::field_mode::lower && frame_factory->get_video_format_desc().field_mode == core::field_mode::upper)
		write->get_frame_transform().fill_translation[1] += 1.0/static_cast<double>(frame_factory->get_video_format_desc().height);
	else if(write->get_type() == core::field_mode::upper && frame_factory->get_video_format_desc().field_mode == core::field_mode::lower)
		write->get_frame_transform().fill_translation[1] -= 1.0/static_cast<double>(frame_factory->get_video_format_desc().height);

	return make_safe_ptr(write);
}

boost::rational<int> read_fps(AVFormatContext& context, boost::rational<int> fail_value)
{
	auto video_index = av_find_best_stream(&context, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
	if(video_index > -1)
	{
		const auto video_stream  = context.streams[video_index];
		const auto video_codec_par = video_stream->codecpar;
		const AVRational framerate = video_codec_par->framerate;

		if(framerate.num > 0)
		{
			return boost::rational<int>(framerate.num, framerate.den);
		}

		if(boost::filesystem2::path(context.url).extension() == ".flv")
		{
			try
			{
				auto meta = read_flv_meta_info(context.url);
				return boost::rational<int>(static_cast<int>(boost::lexical_cast<double>(meta["framerate"])), 1);
			}
			catch(...)
			{
				return fail_value;
			}
		}

		return boost::rational<int>(video_stream->r_frame_rate.num, video_stream->r_frame_rate.den);
	}

	return fail_value;
}

std::wstring print_mode(size_t width, size_t height, boost::rational<int> fps, bool interlaced)
{
	std::wostringstream fps_ss;
	fps_ss << std::fixed << std::setprecision(2) << (boost::rational_cast<double>(fps));
	return boost::lexical_cast<std::wstring>(width) + L"x" + boost::lexical_cast<std::wstring>(height) + (!interlaced ? L"p" : L"i") + fps_ss.str();
}

bool is_valid_file(const std::wstring filename, const std::vector<std::wstring>& invalid_exts)
{
	auto ext = boost::to_lower_copy(boost::filesystem::wpath(filename).extension());
		
	if(std::find(invalid_exts.begin(), invalid_exts.end(), ext) != invalid_exts.end())
		return false;	

	auto filename2 = narrow(filename);

	std::basic_ifstream<unsigned char> file(filename, std::ios::in | std::ios::binary | std::ios::beg);

	const int PROBE_BUFFER_SIZE(2048);
	std::vector<unsigned char> buf(PROBE_BUFFER_SIZE + AVPROBE_PADDING_SIZE);

	if (!file.read(buf.data(), PROBE_BUFFER_SIZE))
		return false;
	AVProbeData pb;
	pb.filename = filename2.c_str();
	pb.buf		= buf.data();
	pb.buf_size = PROBE_BUFFER_SIZE;
	pb.mime_type = nullptr;
	return av_probe_input_format(&pb, true) != nullptr;
}

bool is_valid_file(const std::wstring filename)
{
	static const std::vector<std::wstring> invalid_exts = boost::assign::list_of(L".png")(L".tga")(L".bmp")(L".jpg")(L".jpeg")(L".gif")(L".tiff")(L".tif")(L".jp2")(L".jpx")(L".j2k")(L".j2c")(L".swf")(L".ct")(L".db");
	
	return is_valid_file(filename, invalid_exts);
}

bool try_get_duration(const std::wstring filename, std::int64_t& duration, boost::rational<std::int64_t>& time_base)
{
	AVFormatContext* weak_context = nullptr;
	if(avformat_open_input(&weak_context, narrow(filename).c_str(), nullptr, nullptr) < 0)
		return false;

	std::shared_ptr<AVFormatContext> context(weak_context, [](AVFormatContext* p)
	{
		avformat_close_input(&p);
	});
	
	context->probesize = context->probesize / 5;
	context->max_analyze_duration = context->max_analyze_duration / 5;

	if(avformat_find_stream_info(context.get(), nullptr) < 0)
		return false;

	auto fps = read_fps(*context, boost::rational<int>(1, 1));
	if (fps.denominator() == 0)
		return false;

	duration = av_rescale(context->duration, fps.denominator(), fps.numerator() * AV_TIME_BASE);

	time_base = boost::rational<std::int64_t>(fps.denominator(), fps.numerator());

	return true;
}

std::wstring probe_stem(const std::wstring stem, const std::vector<std::wstring>& invalid_exts)
{
	auto stem2 = boost::filesystem2::wpath(stem);
	auto dir = stem2.parent_path();
	if (boost::filesystem::exists(dir))
		for(auto it = boost::filesystem2::wdirectory_iterator(dir); it != boost::filesystem2::wdirectory_iterator(); ++it)
		{
			if(boost::iequals(it->path().stem(), stem2.filename()) && is_valid_file(it->path().file_string(), invalid_exts))
				return it->path().file_string();
		}
	return L"";
}

std::wstring probe_stem(const std::wstring stem)
{
	auto stem2 = boost::filesystem2::wpath(stem);
	auto dir = stem2.parent_path();
	if (boost::filesystem::exists(dir))
		for(auto it = boost::filesystem2::wdirectory_iterator(dir); it != boost::filesystem2::wdirectory_iterator(); ++it)
		{
			if(boost::iequals(it->path().stem(), stem2.filename()) && is_valid_file(it->path().file_string()))
				return it->path().file_string();
		}
	return L"";
}

core::channel_layout get_audio_channel_layout(
		const AVCodecContext& context, const std::wstring& custom_channel_order)
{
	if (!custom_channel_order.empty())
	{
		auto layout = core::create_custom_channel_layout(
				custom_channel_order,
				core::default_channel_layout_repository());

		layout.num_channels = context.ch_layout.nb_channels;

		return layout;
	}

	int nb_channels = context.ch_layout.nb_channels;

	switch (nb_channels) // TODO: refine this auto-detection
	{
	case 1:
		return core::default_channel_layout_repository().get_by_name(L"MONO");
	case 2:
		return core::default_channel_layout_repository().get_by_name(L"STEREO");
	case 4:
		return core::default_channel_layout_repository().get_by_name(L"DUAL-STEREO");
	case 6:
		return core::default_channel_layout_repository().get_by_name(L"SMPTE");
	}

	return core::create_unspecified_layout(nb_channels);
}

int64_t ffmpeg_time_from_frame_number(int32_t frame_number, int fps_num, int fps_den)
{
	return av_rescale(frame_number, fps_den * AV_TIME_BASE, fps_num);
}

int64_t frame_number_from_ffmpeg_time(int64_t time, int fps_num, int fps_den)
{
	return av_rescale(time, fps_num, fps_den * AV_TIME_BASE);
}

std::vector<int> parse_list(const std::string& list)
{
	auto result_list = std::vector<int>();
	boost::tokenizer<> tok(list);
	for (boost::tokenizer<>::iterator i = tok.begin(); i != tok.end(); i++)
		result_list.push_back(boost::lexical_cast<int>(*i));
	return result_list;
}

void av_buffer_free(void* opaque, uint8_t* data)
{
	delete opaque;
};


}}