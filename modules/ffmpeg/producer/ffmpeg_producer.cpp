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

#include "../stdafx.h"

#include "ffmpeg_producer.h"

#include "../ffmpeg.h"
#include "../ffmpeg_error.h"

#include "muxer/frame_muxer.h"
#include "input/input.h"
#include "util/util.h"
#include "audio/audio_decoder.h"
#include "video/video_decoder.h"

#include <common/env.h>
#include <common/utility/assert.h>
#include <common/diagnostics/graph.h>
#include <common/utility/string.h>

#include <core/monitor/monitor.h>
#include <core/video_format.h>
#include <core/parameters/parameters.h>
#include <core/producer/frame_producer.h>
#include <core/producer/frame/frame_factory.h>
#include <core/producer/frame/basic_frame.h>
#include <core/producer/frame/frame_transform.h>

#include <boost/algorithm/string.hpp>
#include <boost/assign.hpp>
#include <boost/timer.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm/find.hpp>
#include <boost/regex.hpp>
#include <boost/locale.hpp>

#include <tbb/parallel_invoke.h>

#include <limits>
#include <memory>
#include <queue>

namespace caspar { namespace ffmpeg {

std::wstring get_relative_or_original(
		const std::wstring& filename,
		const boost::filesystem::wpath& relative_to)
{
	boost::filesystem::wpath file(filename);
	auto result = file.filename();

	boost::filesystem::wpath current_path = file;

	while (true)
	{
		current_path = current_path.parent_path();

		if (boost::filesystem::equivalent(current_path, relative_to))
			break;

		if (current_path.empty())
			return filename;

		result = current_path.filename() + L"/" + result;
	}

	return result;
}
				
struct ffmpeg_producer : public core::frame_producer
{
	//const int MAX_GOP_SIZE = 256;
	core::monitor::subject										monitor_subject_;
	const std::wstring											filename_;
	const std::wstring											path_relative_to_media_;

	const safe_ptr<diagnostics::graph>							graph_;
	boost::timer												frame_timer_;
					
	const safe_ptr<core::frame_factory>							frame_factory_;
	const core::video_format_desc								format_desc_;

	std::shared_ptr<void>										initial_logger_disabler_;

	input														input_;	
	std::unique_ptr<video_decoder>								video_decoder_;
	std::unique_ptr<audio_decoder>								audio_decoder_;	
	std::unique_ptr<frame_muxer>								muxer_;
	core::channel_layout										audio_channel_layout_;
	const std::wstring											custom_channel_order_;	

	const double												fps_;
	uint32_t													start_;
	const uint32_t												length_;
	const bool													thumbnail_mode_;
	const bool													alpha_mode_;
	const std::string											filter_str_;
	bool														loop_;

	safe_ptr<core::basic_frame>									last_frame_;
	
	std::queue<std::pair<safe_ptr<core::basic_frame>, size_t>>	frame_buffer_;
	uint32_t													file_frame_number_;
	uint32_t													decoded_frame_number_;
	
		
public:
	explicit ffmpeg_producer(const safe_ptr<core::frame_factory>& frame_factory, const std::wstring& filename, const std::wstring& filter, bool loop, uint32_t start, uint32_t length, bool thumbnail_mode, bool alpha_mode, const std::wstring& custom_channel_order, bool field_order_inverted)
		: filename_(filename)
		, path_relative_to_media_(get_relative_or_original(filename, env::media_folder()))
		, frame_factory_(frame_factory)
		, format_desc_(frame_factory->get_video_format_desc())
		, initial_logger_disabler_(temporary_disable_logging_for_thread(thumbnail_mode))
		, input_(graph_, filename_, thumbnail_mode)
		, fps_(read_fps(*input_.format_context().get(), format_desc_.fps))
		, length_(length)
		, thumbnail_mode_(thumbnail_mode)
		, alpha_mode_(alpha_mode)
		, last_frame_(core::basic_frame::empty())
		, filter_str_(narrow(filter))
		, custom_channel_order_(custom_channel_order)
		, loop_(loop)
		, start_(start)
	{
		graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("underflow", diagnostics::color(0.6f, 0.3f, 0.9f));	
		diagnostics::register_graph(graph_);
		try
		{
			video_decoder_.reset(new video_decoder(input_, field_order_inverted));
		}
		catch(averror_stream_not_found&)
		{
			CASPAR_LOG(warning) << print() << " No video-stream found. Running without video.";	
		}
		catch(...)
		{
			if (!thumbnail_mode_)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				CASPAR_LOG(warning) << print() << "Failed to open video-stream. Running without video.";	
			}
		}

		audio_channel_layout_ = core::default_channel_layout_repository().get_by_name(L"STEREO");

		if (!thumbnail_mode_)
		{
			try
			{
				audio_decoder_.reset(new audio_decoder(input_, frame_factory->get_video_format_desc(), custom_channel_order));
				audio_channel_layout_ = audio_decoder_->channel_layout();
			}
			catch(averror_stream_not_found&)
			{
				CASPAR_LOG(warning) << print() << " No audio-stream found. Running without audio.";	
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				CASPAR_LOG(warning) << print() << " Failed to open audio-stream. Running without audio.";		
			}
		}

		if(!video_decoder_ && !audio_decoder_)
			BOOST_THROW_EXCEPTION(averror_stream_not_found() << msg_info("No streams found"));

		muxer_.reset(new frame_muxer(format_desc_.fps, frame_factory, thumbnail_mode_, audio_channel_layout_, filter_str_));
		seek(start);
		for (int n = 0; n < 32 && frame_buffer_.size() < 4; ++n)
			try_decode_frame(thumbnail_mode ? core::frame_producer::DEINTERLACE_HINT : alpha_mode ? core::frame_producer::ALPHA_HINT : core::frame_producer::NO_HINT);
	}

	// frame_producer
	
	virtual safe_ptr<core::basic_frame> receive(int hints) override
	{
		return render_frame(hints).first;
	}

	virtual safe_ptr<core::basic_frame> last_frame() const override
	{
		return pause(disable_audio(last_frame_));
	}

	std::pair<safe_ptr<core::basic_frame>, uint32_t> render_frame(int hints)
	{		
		frame_timer_.restart();
		auto disable_logging = temporary_disable_logging_for_thread(thumbnail_mode_);
				
		for(int n = 0; n < 32 && frame_buffer_.size() < 4; ++n)
			try_decode_frame(hints);
		
		graph_->set_value("frame-time", frame_timer_.elapsed()*format_desc_.fps*0.5);

		if (frame_buffer_.empty())
		{
			if (input_.eof())
			{
				send_osc();
				return std::make_pair(last_frame(), -1);
			}
			else 
			{
				graph_->set_tag("underflow");  
				send_osc();
				return std::make_pair(core::basic_frame::late(), -1);     
			}
		}
		
		auto frame = frame_buffer_.front(); 
		last_frame_ = frame.first;
		file_frame_number_ = frame.second;
		frame_buffer_.pop();

		graph_->set_text(print());
		send_osc();

		return frame;
	}

	void send_osc()
	{
		monitor_subject_	<< core::monitor::message("/profiler/time")		% frame_timer_.elapsed() % (1.0/format_desc_.fps);			
								
		monitor_subject_	<< core::monitor::message("/file/time")			% (file_frame_number()/fps_) 
																			% (file_nb_frames()/fps_)
							<< core::monitor::message("/file/frame")			% static_cast<int32_t>(file_frame_number())
																			% static_cast<int32_t>(file_nb_frames())
							<< core::monitor::message("/file/fps")			% fps_
							<< core::monitor::message("/file/path")			% path_relative_to_media_
							<< core::monitor::message("/loop")				% loop_;
	}
	
	safe_ptr<core::basic_frame> render_specific_frame(uint32_t file_position, int hints)
	{
		// Some trial and error and undeterministic stuff here
		static const int NUM_RETRIES = 32;
		
		seek(file_position);

		for (int i = 0; i < NUM_RETRIES; ++i)
		{
			boost::this_thread::sleep(boost::posix_time::milliseconds(40));
			auto frame = render_frame(hints);
			return frame.first;
		}
		return caspar::core::basic_frame::empty();
	}

	virtual safe_ptr<core::basic_frame> create_thumbnail_frame() override
	{
		auto disable_logging = temporary_disable_logging_for_thread(thumbnail_mode_);

		auto total_frames = nb_frames();
		auto grid = env::properties().get(L"configuration.thumbnails.video-grid", 2);

		if (grid < 1)
		{
			CASPAR_LOG(error) << L"configuration/thumbnails/video-grid cannot be less than 1";
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("configuration/thumbnails/video-grid cannot be less than 1"));
		}

		if (grid == 1)
		{
			return render_specific_frame(total_frames / 2, 0/*DEINTERLACE_HINT*/);
		}

		auto num_snapshots = grid * grid;

		std::vector<safe_ptr<core::basic_frame>> frames;

		for (int i = 0; i < num_snapshots; ++i)
		{
			int x = i % grid;
			int y = i / grid;
			int desired_frame;
			
			if (i == 0)
				desired_frame = 0; // first
			else if (i == num_snapshots - 1)
				desired_frame = total_frames - 1; // last
			else
				// evenly distributed across the file.
				desired_frame = total_frames * i / (num_snapshots - 1);

			auto frame = render_specific_frame(desired_frame, DEINTERLACE_HINT);
			frame->get_frame_transform().fill_scale[0] = 1.0 / static_cast<double>(grid);
			frame->get_frame_transform().fill_scale[1] = 1.0 / static_cast<double>(grid);
			frame->get_frame_transform().fill_translation[0] = 1.0 / static_cast<double>(grid) * x;
			frame->get_frame_transform().fill_translation[1] = 1.0 / static_cast<double>(grid) * y;

			frames.push_back(frame);
		}

		return make_safe<core::basic_frame>(frames);
	}
	
	uint32_t file_frame_number() const
	{
		return file_frame_number_;
	}

	virtual uint32_t nb_frames() const override
	{
		if(loop_) 
			return std::numeric_limits<uint32_t>::max();

		uint32_t nb_frames = file_nb_frames();

		if (length_ > 0)
			nb_frames = std::min(length_, nb_frames);
		return nb_frames;
	}

	uint32_t file_nb_frames() const
	{
		if (video_decoder_)
			return  std::max(video_decoder_->nb_frames(), file_frame_number_);
		return std::numeric_limits<uint32_t>::max();
	}
	
	virtual boost::unique_future<std::wstring> call(const std::wstring& param) override
	{
		boost::promise<std::wstring> promise;
		promise.set_value(do_call(param));
		return promise.get_future();
	}
				
	virtual std::wstring print() const override
	{
		return L"ffmpeg[" + boost::filesystem::wpath(filename_).filename() + L"|" 
						  + print_mode() + L"|" 
						  + boost::lexical_cast<std::wstring>(file_frame_number_) + L"/" + boost::lexical_cast<std::wstring>(file_nb_frames()) + L"]";
	}

	boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type",				L"ffmpeg-producer");
		info.add(L"filename",			filename_);
		info.add(L"width",				video_decoder_ ? video_decoder_->width() : 0);
		info.add(L"height",				video_decoder_ ? video_decoder_->height() : 0);
		info.add(L"progressive",		video_decoder_ ? video_decoder_->is_progressive() : false);
		info.add(L"fps",				fps_);
		info.add(L"loop",				loop_);
		auto nb_frames2 = nb_frames();
		info.add(L"nb-frames",			nb_frames2 == std::numeric_limits<int64_t>::max() ? -1 : nb_frames2);
		info.add(L"file-frame-number",	file_frame_number_);
		info.add(L"file-nb-frames",		file_nb_frames());
		return info;
	}

	// ffmpeg_producer

	std::wstring print_mode() const
	{
		return video_decoder_ ? ffmpeg::print_mode(video_decoder_->width(), video_decoder_->height(), fps_, !video_decoder_->is_progressive()) : L"";
	}
					
	std::wstring do_call(const std::wstring& param)
	{
		static const boost::wregex loop_exp(L"LOOP\\s*(?<VALUE>\\d?)?", boost::regex::icase);
		static const boost::wregex seek_exp(L"SEEK\\s+(?<VALUE>\\d+)", boost::regex::icase);
		static const boost::wregex field_order_inverted_exp(L"FIELD_ORDER_INVERTED\\s+(?<VALUE>\\d+)", boost::regex::icase);
		
		boost::wsmatch what;
		if(boost::regex_match(param, what, loop_exp))
		{
			if (!what["VALUE"].str().empty())
			{
				loop_ = (boost::lexical_cast<bool>(what["VALUE"].str()));
				return L"LOOP OK";
			}
		}

		if(boost::regex_match(param, what, seek_exp))
		{
			while (!frame_buffer_.empty())
				frame_buffer_.pop();
			muxer_->clear();
			seek(boost::lexical_cast<uint32_t>(what["VALUE"].str()));
			return L"SEEK OK";
		}
		if(boost::regex_match(param, what, field_order_inverted_exp))
		{
			if (video_decoder_)
				video_decoder_->invert_field_order(boost::lexical_cast<bool>(what["VALUE"].str()));
			return L"FIELD_ORDER_INVERTED OK";
		}
		
		BOOST_THROW_EXCEPTION(invalid_argument());
	}

	void seek(uint32_t frame)
	{
		int64_t time_to_seek = ffmpeg_time_from_frame_number(frame, fps_);
		input_.seek(time_to_seek);
		if (video_decoder_)
			video_decoder_->seek(time_to_seek, frame);
		if (audio_decoder_)
			audio_decoder_->seek(time_to_seek);
		file_frame_number_ = frame;
		decoded_frame_number_ = frame;
	}


	void decode_frame(std::shared_ptr<AVFrame>& video, std::shared_ptr<core::audio_buffer>& audio, const int hints)
	{
		tbb::parallel_invoke(
			[&]
		{
			if (!muxer_->video_ready() && video_decoder_)
				video = video_decoder_->poll();
		},
			[&]
		{
			if (!muxer_->audio_ready() && audio_decoder_)
				audio = audio_decoder_->poll();
		});

		muxer_->push(video, hints);
		muxer_->push(audio);
	}
	
	void try_decode_frame(int hints)
	{
		if (loop_ && 
			((length_ != std::numeric_limits<uint32_t>().max() && decoded_frame_number_ >= start_ + length_) 
				|| input_.eof()))
			seek(start_);
		if (!loop_ && length_ != std::numeric_limits<uint32_t>().max() && decoded_frame_number_ >= start_ + length_)
			return;

		std::shared_ptr<AVFrame>			video;
		std::shared_ptr<core::audio_buffer> audio;

		decode_frame(video, audio, hints);

		if((!audio_decoder_ || (audio == nullptr && input_.eof()))
			&& !muxer_->audio_ready())
				muxer_->push(empty_audio());
		
		if(!video_decoder_)
		{
			if(!muxer_->video_ready())
				muxer_->push(empty_video(), 0);
		}
		
		for (auto frame = muxer_->poll(); frame; frame = muxer_->poll())
		{
			frame_buffer_.push(std::make_pair(make_safe_ptr(frame), decoded_frame_number_++));
		}
	}

	core::monitor::subject& monitor_output()
	{
		return monitor_subject_;
	}
};

safe_ptr<core::frame_producer> create_producer(
		const safe_ptr<core::frame_factory>& frame_factory,
		const core::parameters& params)
{		
	static const std::vector<std::wstring> invalid_exts = boost::assign::list_of(L".png")(L".tga")(L".bmp")(L".jpg")(L".jpeg")(L".gif")(L".tiff")(L".tif")(L".jp2")(L".jpx")(L".j2k")(L".j2c")(L".swf")(L".ct");

	// Infer the resource type from the resource_name
	auto tokens = core::parameters::protocol_split(params.at_original(0));
	auto filename = tokens[1];
	if (!is_valid_file(filename, invalid_exts))
		filename = env::media_folder() + L"\\" + tokens[1];
	if(!boost::filesystem::exists(filename))
		filename = probe_stem(filename, invalid_exts);
	if(filename.empty())
		return core::frame_producer::empty();
	
	auto loop		= params.has(L"LOOP");
	auto start		= params.get(L"SEEK", static_cast<uint32_t>(0));
	auto length		= params.get(L"LENGTH", std::numeric_limits<uint32_t>::max());
	auto filter_str = params.get(L"FILTER", L""); 	
	auto custom_channel_order	= params.get(L"CHANNEL_LAYOUT", L"");
	auto field_order_inverted = params.has(L"FIELD_ORDER_INVERTED");
	bool is_alpha = params.has(L"IS_ALPHA");

	boost::replace_all(filter_str, L"DEINTERLACE", L"YADIF=0:-1");
	boost::replace_all(filter_str, L"DEINTERLACE_BOB", L"YADIF=1:-1");
	
	return create_producer_destroy_proxy(make_safe<ffmpeg_producer>(frame_factory, filename, filter_str, loop, start, length, false, is_alpha, custom_channel_order, field_order_inverted));
}

safe_ptr<core::frame_producer> create_thumbnail_producer(
		const safe_ptr<core::frame_factory>& frame_factory,
		const core::parameters& params)
{		
	static const std::vector<std::wstring> invalid_exts = boost::assign::list_of
			(L".png")(L".tga")(L".bmp")(L".jpg")(L".jpeg")(L".gif")(L".tiff")(L".tif")(L".jp2")(L".jpx")(L".j2k")(L".j2c")(L".swf")(L".ct")
			(L".wav")(L".mp3"); // audio shall not have thumbnails
	auto filename = probe_stem(env::media_folder() + L"\\" + params.at_original(0), invalid_exts);

	if(filename.empty())
		return core::frame_producer::empty();
	
	return make_safe<ffmpeg_producer>(frame_factory, filename, L"", false, 0, std::numeric_limits<uint32_t>::max(), true, false, L"", false);
}

}}