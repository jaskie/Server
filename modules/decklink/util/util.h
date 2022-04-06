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

#pragma once

#include <common/exception/exceptions.h>
#include <common/log/log.h>
#include <common/memory/memshfl.h>
#include <core/video_format.h>
#include <core/mixer/read_frame.h>

#include "../interop/DeckLinkAPI_h.h"

#include <boost/lexical_cast.hpp>

#include <atlbase.h>

#include <string>

namespace caspar { namespace decklink {

	struct com_initializer
	{
		const HRESULT result_;
		com_initializer(): result_(::CoInitialize(nullptr)) {  }
		~com_initializer() { if (SUCCEEDED(result_)) ::CoUninitialize(); }
	};

	
static BMDDisplayMode get_decklink_video_format(core::video_format::type fmt) 
{
	switch(fmt)
	{
	case core::video_format::pal:			return bmdModePAL;
	case core::video_format::ntsc:			return bmdModeNTSC;
	case core::video_format::x576p2500:		return bmdModePALp;
	case core::video_format::x720p2398:		return bmdModeNTSCp;
	case core::video_format::x720p2400:		return bmdModeUnknown;
	case core::video_format::x720p2500:		return bmdModeUnknown;
	case core::video_format::x720p5000:		return bmdModeHD720p50;
	case core::video_format::x720p2997:		return bmdModeUnknown;
	case core::video_format::x720p5994:		return bmdModeHD720p5994;
	case core::video_format::x720p3000:		return bmdModeUnknown;
	case core::video_format::x720p6000:		return bmdModeHD720p60;
	case core::video_format::x1080p2398:	return bmdModeHD1080p2398;
	case core::video_format::x1080p2400:	return bmdModeHD1080p24;
	case core::video_format::x1080i5000:	return bmdModeHD1080i50;
	case core::video_format::x1080i5994:	return bmdModeHD1080i5994;
	case core::video_format::x1080i6000:	return bmdModeHD1080i6000;
	case core::video_format::x1080p2500:	return bmdModeHD1080p25;
	case core::video_format::x1080p2997:	return bmdModeHD1080p2997;
	case core::video_format::x1080p3000:	return bmdModeHD1080p30;
	case core::video_format::x1080p5000:	return bmdModeHD1080p50;
	case core::video_format::x1080p5994:	return bmdModeHD1080p5994;
	case core::video_format::x1080p6000:	return bmdModeHD1080p6000;
	case core::video_format::x1556p2398:	return bmdMode2k2398;
	case core::video_format::x1556p2400:	return bmdMode2k24;
	case core::video_format::x1556p2500:	return bmdMode2k25;
	case core::video_format::x2160p2398:	return bmdMode4K2160p2398;
	case core::video_format::x2160p2400:	return bmdMode4K2160p24;
	case core::video_format::x2160p2500:	return bmdMode4K2160p25;
	case core::video_format::x2160p2997:	return bmdMode4K2160p2997;
	case core::video_format::x2160p3000:	return bmdMode4K2160p30;
	case core::video_format::x2160p5000:	return bmdMode4K2160p50;
	case core::video_format::x2160p6000:	return bmdMode4K2160p60;
	default:								return bmdModeUnknown;
	}
}

static core::video_format::type get_caspar_video_format(BMDDisplayMode fmt) 
{
	switch(fmt)
	{
	case bmdModePAL:						return core::video_format::pal;		
	case bmdModeNTSC:						return core::video_format::ntsc;		
	case bmdModeHD720p50:					return core::video_format::x720p5000;	
	case bmdModeHD720p5994:					return core::video_format::x720p5994;	
	case bmdModeHD720p60:					return core::video_format::x720p6000;	
	case bmdModeHD1080p2398:				return core::video_format::x1080p2398;	
	case bmdModeHD1080p24:					return core::video_format::x1080p2400;	
	case bmdModeHD1080i50:					return core::video_format::x1080i5000;	
	case bmdModeHD1080i5994:				return core::video_format::x1080i5994;	
	case bmdModeHD1080i6000:				return core::video_format::x1080i6000;	
	case bmdModeHD1080p25:					return core::video_format::x1080p2500;	
	case bmdModeHD1080p2997:				return core::video_format::x1080p2997;	
	case bmdModeHD1080p30:					return core::video_format::x1080p3000;	
	case bmdModeHD1080p50:					return core::video_format::x1080p5000;	
	case bmdModeHD1080p5994:				return core::video_format::x1080p5994;	
	case bmdModeHD1080p6000:				return core::video_format::x1080p6000;	
	case bmdMode2k2398:						return core::video_format::x1556p2398;	
	case bmdMode2k24:						return core::video_format::x1556p2400;	
	case bmdMode2k25:						return core::video_format::x1556p2500;	
	case bmdMode4K2160p2398:				return core::video_format::x2160p2398;	
	case bmdMode4K2160p24:					return core::video_format::x2160p2400;	
	case bmdMode4K2160p25:					return core::video_format::x2160p2500;	
	case bmdMode4K2160p2997:				return core::video_format::x2160p2997;	
	case bmdMode4K2160p30:					return core::video_format::x2160p3000;	
	case bmdMode4K2160p50:					return core::video_format::x2160p5000;	
	case bmdMode4K2160p60:					return core::video_format::x2160p6000;
	default:								return core::video_format::invalid;	
	}
}

static int bcd2frame(BMDTimecodeBCD bcd, byte fps) 
{
	byte hour   = (bcd >> 24 & 0xF) + (bcd >> 28 & 0xF) * 10;
	byte min    = (bcd >> 16 & 0xF) + (bcd >> 20 & 0xF) * 10;
	byte sec    = (bcd >>  8 & 0xF) + (bcd >> 12 & 0xF) * 10;
	byte frames = (bcd       & 0xF) + (bcd >>  4 & 0xF) * 10;
	return ((((static_cast<int>(hour) * 60) + min) * 60) + sec) * fps + frames;
}

static BMDTimecodeBCD frame2bcd(int frames, byte fps)
{
	unsigned int frame = frames   %  fps;
	unsigned int sec   = (frames  /  fps) % 60;
	unsigned int min   = (frames  / (fps * 60)) % 60;
	unsigned int hour  = (frames  / (fps * 60 * 60));
	return (frame % 10) | ((frame / 10) << 4) | ((sec % 10) << 8) | ((sec / 10) << 12) | ((min % 10) << 16) | ((min / 10) << 20) | ((hour % 10) << 24) | ((hour / 10) << 28);
}


template<typename T>
static CComPtr<IDeckLinkDisplayMode> get_display_mode(const CComQIPtr<T>& device, core::video_format::type fmt, BMDPixelFormat pix_fmt)
{
	CComPtr<IDeckLinkDisplayMode> result;
	if (FAILED(device->GetDisplayMode(get_decklink_video_format(fmt), &result)) || !result)
		BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Device could not find requested video-format.")
			<< arg_value_info(boost::lexical_cast<std::string>(fmt))
			<< arg_name_info("format"));
	return result;
}

static unsigned int num_decklink_out_channels(int input_channels) 
{
	if (input_channels <= 2)
		return 2;

	if (input_channels <= 8)
		return 8;

	return 16;
}

template<typename T>
static std::wstring get_version(T& iterator)
{
	CComQIPtr<IDeckLinkAPIInformation> info = iterator;
	if (!info)
		return L"Unknown";
	
	BSTR ver;		
	info->GetString(BMDDeckLinkAPIVersion, &ver);
		
	return ver;					
}

static CComPtr<IDeckLink> get_device(size_t device_index)
{
	CComPtr<IDeckLinkIterator> pDecklinkIterator;
	if(FAILED(pDecklinkIterator.CoCreateInstance(CLSID_CDeckLinkIterator)))
		BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Decklink drivers not found."));
		
	size_t n = 0;
	CComPtr<IDeckLink> decklink;
	while(n < device_index && pDecklinkIterator->Next(&decklink) == S_OK){++n;}	

	if(n != device_index || !decklink)
		BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Decklink device not found.") << arg_name_info("device_index") << arg_value_info(boost::lexical_cast<std::string>(device_index)));
		
	return decklink;
}

static CComPtr<IDeckLinkDeckControl> get_deck_control(const CComPtr<IDeckLink>& decklink)
{
	CComPtr<IDeckLinkDeckControl> result;
	decklink->QueryInterface(IID_IDeckLinkDeckControl, (void**)&result);
	return result;
}


template <typename T>
static std::wstring get_model_name(const T& device)
{	
	BSTR pModelName;
	device->GetModelName(&pModelName);
	return std::wstring(pModelName);
}

static std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>> extract_key(
		const safe_ptr<core::read_frame>& frame)
{
	std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>> result;

	result.resize(frame->image_data().size());
	fast_memshfl(
			result.data(),
			frame->image_data().begin(),
			frame->image_data().size(),
			0x0F0F0F0F, 0x0B0B0B0B, 0x07070707, 0x03030303);

	return std::move(result);
}

class decklink_frame : public IDeckLinkVideoFrame
{
	tbb::atomic<int>											ref_count_;
	std::shared_ptr<core::read_frame>							frame_;
	const core::video_format_desc								format_desc_;

	const bool													key_only_;
	std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>> data_;
public:
	decklink_frame(const std::shared_ptr<core::read_frame>& frame, const core::video_format_desc& format_desc, bool key_only)
		: frame_(frame)
		, format_desc_(format_desc)
		, key_only_(key_only)
	{
		ref_count_ = 0;
	}

	decklink_frame(const std::shared_ptr<core::read_frame>& frame, const core::video_format_desc& format_desc, std::vector<uint8_t, tbb::cache_aligned_allocator<uint8_t>>&& key_data)
		: frame_(frame)
		, format_desc_(format_desc)
		, key_only_(true)
		, data_(std::move(key_data))
	{
		ref_count_ = 0;
	}
	
	// IUnknown

	STDMETHOD (QueryInterface(REFIID, LPVOID*))		
	{
		return E_NOINTERFACE;
	}
	
	STDMETHOD_(ULONG,			AddRef())			
	{
		return ++ref_count_;
	}

	STDMETHOD_(ULONG,			Release())			
	{
		if(--ref_count_ == 0)
			delete this;
		return ref_count_;
	}

	// IDecklinkVideoFrame

	STDMETHOD_(long,			GetWidth())			{return format_desc_.width;}        
    STDMETHOD_(long,			GetHeight())		{return format_desc_.height;}        
    STDMETHOD_(long,			GetRowBytes())		{return format_desc_.width*4;}        
	STDMETHOD_(BMDPixelFormat,	GetPixelFormat())	{return bmdFormat8BitBGRA;}        
    STDMETHOD_(BMDFrameFlags,	GetFlags())			{return bmdFrameFlagDefault;}
        
    STDMETHOD(GetBytes(void** buffer))
	{
		try
		{
			if(static_cast<size_t>(frame_->image_data().size()) != format_desc_.size)
			{
				data_.resize(format_desc_.size, 0);
				*buffer = data_.data();
			}
			else if(key_only_)
			{
				if(data_.empty())
				{
					data_.resize(frame_->image_data().size());
					fast_memshfl(data_.data(), frame_->image_data().begin(), frame_->image_data().size(), 0x0F0F0F0F, 0x0B0B0B0B, 0x07070707, 0x03030303);
				}
				*buffer = data_.data();
			}
			else
				*buffer = const_cast<uint8_t*>(frame_->image_data().begin());
		}
		catch(...)
		{
			CASPAR_LOG_CURRENT_EXCEPTION();
			return E_FAIL;
		}

		return S_OK;
	}
        
    STDMETHOD(GetTimecode(BMDTimecodeFormat format, IDeckLinkTimecode** timecode)){return S_FALSE;}        
    STDMETHOD(GetAncillaryData(IDeckLinkVideoFrameAncillary** ancillary))		  {return S_FALSE;}

	// decklink_frame	

	const boost::iterator_range<const int32_t*> audio_data()
	{
		return frame_->audio_data();
	}

	int64_t get_age_millis() const
	{
		return frame_->get_age_millis();
	}
};

struct configuration
{
	enum keyer_t
	{
		internal_keyer,
		external_keyer,
		default_keyer
	};

	enum latency_t
	{
		low_latency,
		normal_latency,
		default_latency
	};

	size_t					device_index;
	bool					embedded_audio;
	core::channel_layout	audio_layout;
	keyer_t					keyer;
	latency_t				latency;
	bool					key_only;
	size_t					base_buffer_depth;
	
	configuration()
		: device_index(1)
		, embedded_audio(false)
		, audio_layout(core::default_channel_layout_repository().get_by_name(L"STEREO"))
		, keyer(default_keyer)
		, latency(default_latency)
		, key_only(false)
		, base_buffer_depth(3)
	{
	}
	
	size_t buffer_depth() const
	{
		return base_buffer_depth + (latency == low_latency ? 0 : 1) + (embedded_audio ? 1 : 0);
	}

};

static void set_latency(
		const CComQIPtr<IDeckLinkConfiguration_v10_2>& config,
		configuration::latency_t latency,
		const std::wstring& print)
{		
	if (latency == configuration::low_latency)
	{
		config->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, true);
		CASPAR_LOG(info) << print << L" Enabled low-latency mode.";
	}
	else if (latency == configuration::normal_latency)
	{			
		config->SetFlag(bmdDeckLinkConfigLowLatencyVideoOutput, false);
		CASPAR_LOG(info) << print << L" Disabled low-latency mode.";
	}
}

static void set_keyer(
		const CComQIPtr<IDeckLinkProfileAttributes>& attributes,
		const CComQIPtr<IDeckLinkKeyer>& decklink_keyer,
		configuration::keyer_t keyer,
		const std::wstring& print)
{
	if (keyer == configuration::internal_keyer) 
	{
		BOOL value = true;
		if (SUCCEEDED(attributes->GetFlag(BMDDeckLinkSupportsInternalKeying, &value)) && !value)
			CASPAR_LOG(error) << print << L" Failed to enable internal keyer.";	
		else if (FAILED(decklink_keyer->Enable(FALSE)))			
			CASPAR_LOG(error) << print << L" Failed to enable internal keyer.";			
		else if (FAILED(decklink_keyer->SetLevel(255)))			
			CASPAR_LOG(error) << print << L" Failed to set key-level to max.";
		else
			CASPAR_LOG(info) << print << L" Enabled internal keyer.";		
	}
	else if (keyer == configuration::external_keyer)
	{
		BOOL value = true;
		if (SUCCEEDED(attributes->GetFlag(BMDDeckLinkSupportsExternalKeying, &value)) && !value)
			CASPAR_LOG(error) << print << L" Failed to enable external keyer.";	
		else if (FAILED(decklink_keyer->Enable(TRUE)))			
			CASPAR_LOG(error) << print << L" Failed to enable external keyer.";	
		else if (FAILED(decklink_keyer->SetLevel(255)))			
			CASPAR_LOG(error) << print << L" Failed to set key-level to max.";
		else
			CASPAR_LOG(info) << print << L" Enabled external keyer.";			
	}
}

}}