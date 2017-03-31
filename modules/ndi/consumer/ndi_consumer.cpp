/*
* Copyright 2017 Telewizja Polska
*
* This file is part of TVP's CasparCG fork.
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
* Author: Jerzy Jaœkiewicz, jurek@tvp.pl based on Robert Nagy, ronag89@gmail.com work
*/

#include"ndi_consumer.h"

#include <common/exception/win32_exception.h>
#include <common/exception/exceptions.h>
#include <common/env.h>
#include <common/log/log.h>
#include <common/utility/string.h>
#include <common/concurrency/future_util.h>

#include <core/parameters/parameters.h>
#include <core/consumer/frame_consumer.h>
#include <core/video_format.h>
#include <core/mixer/read_frame.h>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread.hpp>
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#include <boost/crc.hpp>
#pragma warning(pop)

#include <Processing.NDI.Lib.h>

namespace caspar {
	namespace ndi {

		int crc16(const std::wstring& str)
		{
			boost::crc_16_type result;
			result.process_bytes(str.data(), str.length());
			return result.checksum();
		}

		NDIlib_send_instance_t create_ndi_send(const std::wstring ndi_name)
		{
			const NDIlib_send_create_t NDI_send_create_desc = { narrow(ndi_name).c_str(), NULL, true, false };
			return NDIlib_send_create(&NDI_send_create_desc);
		}

		struct ndi_consumer : public core::frame_consumer
		{
			core::video_format_desc			format_desc_;
			std::wstring					ndi_name_;
			const int						index_;
			const NDIlib_send_instance_t	p_ndi_send_;
		public:

			// frame_consumer

			ndi_consumer(const std::wstring& ndi_name)
				: ndi_name_(ndi_name)
				, index_(NDI_CONSUMER_BASE_INDEX + crc16(ndi_name))
				, p_ndi_send_(create_ndi_send(ndi_name))
			{

			}

			~ndi_consumer()
			{
				if (p_ndi_send_)
					NDIlib_send_destroy(p_ndi_send_);
			}

			virtual void initialize(const core::video_format_desc& format_desc, int) override
			{
				format_desc_ = format_desc;
			}

			virtual int64_t presentation_frame_age_millis() const override
			{
				return 0;
			}

			virtual boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame) override
			{
				/*auto format_desc = format_desc_;
				auto filename = filename_;

				boost::thread async([format_desc, frame, filename]
				{
					win32_exception::ensure_handler_installed_for_thread("image-consumer-thread");

					try
					{
						auto filename2 = filename;

						if (filename2.empty())
							filename2 = env::media_folder() + widen(boost::posix_time::to_iso_string(boost::posix_time::second_clock::local_time())) + L".png";
						else
							filename2 = env::media_folder() + filename2 + L".png";

						auto bitmap = std::shared_ptr<FIBITMAP>(FreeImage_Allocate(format_desc.width, format_desc.height, 32), FreeImage_Unload);
						memcpy(FreeImage_GetBits(bitmap.get()), frame->image_data().begin(), frame->image_size());
						FreeImage_FlipVertical(bitmap.get());
						FreeImage_SaveU(FIF_PNG, bitmap.get(), filename2.c_str(), 0);
					}
					catch (...)
					{
						CASPAR_LOG_CURRENT_EXCEPTION();
					}
				});
				async.detach();*/

				return wrap_as_future(true);
			}

			virtual std::wstring print() const override
			{
				return L"ndi[]";
			}

			virtual boost::property_tree::wptree info() const override
			{
				boost::property_tree::wptree info;
				info.add(L"type", L"ndi-consumer");
				return info;
			}

			virtual size_t buffer_depth() const override
			{
				return 0;
			}

			virtual int index() const override
			{
				return index_;
			}
		};


		safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params)
		{
			if (params.size() < 1 || params.at(0) != L"NDI")
				return core::frame_consumer::empty();

			std::wstring ndi_name;

			if (params.size() > 1)
				ndi_name = params.at(1);

			return make_safe<ndi_consumer>(ndi_name);
		}

		safe_ptr<core::frame_consumer> create_consumer(const boost::property_tree::wptree& ptree)
		{
			auto ndi_name = ptree.get<std::wstring>(L"ndi-name");
			return make_safe<ndi_consumer>(ndi_name);
		}

	}
}