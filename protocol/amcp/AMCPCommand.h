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
* Author: Nicklas P Andersson
*/

#pragma once

#include "../util/clientinfo.h"

#include <core/consumer/frame_consumer.h>
#include <core/parameters/parameters.h>
#include <core/video_channel.h>
#include <core/recorder.h>
#include <core/producer/media_info/media_info_repository.h>

#include <boost/algorithm/string.hpp>

namespace caspar { namespace protocol { namespace amcp {

	class AMCPCommand
	{
		AMCPCommand(const AMCPCommand&);
		AMCPCommand& operator=(const AMCPCommand&);
	public:
		AMCPCommand();
		virtual ~AMCPCommand() {}
		virtual bool Execute() = 0;

		virtual bool NeedChannel() = 0;
		virtual int GetMinimumParameters() = 0;

		void SendReply();

		void AddParameter(const std::wstring& param) { _parameters.push_back(param); }

		void SetParameters(const core::parameters& p) {
			_parameters = p;
		}

		void SetClientInfo(IO::ClientInfoPtr& s) { pClientInfo_ = s; }
		IO::ClientInfoPtr GetClientInfo() { return pClientInfo_; }

		void SetChannel(const std::shared_ptr<core::video_channel>& pChannel) { pChannel_ = pChannel; }
		std::shared_ptr<core::video_channel> GetChannel() { return pChannel_; }

		void SetChannels(const std::vector<safe_ptr<core::video_channel>>& channels) { channels_ = channels; }
		const std::vector<safe_ptr<core::video_channel>>& GetChannels() { return channels_; }

		void SetRecorders(const std::vector<safe_ptr<core::recorder>>& recorders) { recorders_ = recorders; }
		const std::vector<safe_ptr<core::recorder>>& GetRecorders() { return recorders_; }

		void SetMediaInfoRepo(const safe_ptr<core::media_info_repository>& media_info_repo) { media_info_repo_ = media_info_repo; }
		std::shared_ptr<core::media_info_repository> GetMediaInfoRepo() { return media_info_repo_; }

		void SetChannelIndex(unsigned int channelIndex) { channelIndex_ = channelIndex; }
		unsigned int GetChannelIndex() { return channelIndex_; }

		void SetLayerIntex(int layerIndex) { layerIndex_ = layerIndex; }
		int GetLayerIndex(int defaultValue = 0) const { return layerIndex_ != -1 ? layerIndex_ : defaultValue; }

		virtual void Clear();

		virtual std::wstring print() const = 0;

		void SetReplyString(const std::wstring& str) { replyString_ = str; }

		void SetRequestId(const std::wstring& id) { requestId_ = id; }

	protected:
		core::parameters _parameters;

	private:
		unsigned int channelIndex_;
		int layerIndex_;
		IO::ClientInfoPtr pClientInfo_;
		std::shared_ptr<core::video_channel> pChannel_;
		std::vector<safe_ptr<core::video_channel>> channels_;
		std::vector<safe_ptr<core::recorder>> recorders_;
		std::shared_ptr<core::media_info_repository> media_info_repo_;
		std::wstring replyString_;
		std::wstring requestId_;
	};

	typedef std::shared_ptr<AMCPCommand> AMCPCommandPtr;

	template<bool TNeedChannel, int TMinParameters>
	class AMCPCommandBase : public AMCPCommand
	{
	public:
		AMCPCommandBase()
		{
		}
		virtual bool Execute()
		{
			_parameters.to_upper();
			return (TNeedChannel && !GetChannel()) || _parameters.size() < TMinParameters ? false : DoExecute();
		}

		virtual bool NeedChannel() { return TNeedChannel; }
		virtual int GetMinimumParameters() { return TMinParameters; }
	protected:
		~AMCPCommandBase()
		{
		}

	private:
		virtual bool DoExecute() = 0;
	};

}}}
