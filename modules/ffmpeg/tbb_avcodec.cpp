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

#include "StdAfx.h"

#include "tbb_avcodec.h"

#include <common/log/log.h>
#include <common/env.h>
#include <common/utility/assert.h>

#include <tbb/task.h>
#include <tbb/atomic.h>
#include <tbb/parallel_for.h>
#include <tbb/tbb_thread.h>
#include <boost/thread.hpp>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar {
		
static const size_t MAX_THREADS = 16; // See mpegvideo.h

int thread_execute(AVCodecContext* s, int (*func)(AVCodecContext *c2, void *arg2), void* arg, int* ret, int count, int size)
{
	tbb::parallel_for(0, count, 1, [&](int i)
	{
        int r = func(s, (char*)arg + i*size);
        if(ret) 
			ret[i] = r;
    });

	return 0;
}

int thread_execute2(AVCodecContext* s, int (*func)(AVCodecContext* c2, void* arg2, int, int), void* arg, int* ret, int count)
{	
	tbb::atomic<int> counter;   
    counter = 0;   

	//CASPAR_VERIFY(tbb::tbb_thread::hardware_concurrency() < MAX_THREADS);
	// Note: this will probably only work when tbb::task_scheduler_init::num_threads() < 16.
    tbb::parallel_for(tbb::blocked_range<int>(0, count, 2), [&](const tbb::blocked_range<int> &r)    
    {   
        int threadnr = counter++;   
        for(int jobnr = r.begin(); jobnr != r.end(); ++jobnr)
        {   
            int r = func(s, arg, jobnr, threadnr);   
            if (ret)   
                ret[jobnr] = r;   
        }
        --counter;
    });   

    return 0;  
}

void thread_init(AVCodecContext* s, bool execute2enable, bool encoding, bool frame, bool slice)
{
    s->execute			  = thread_execute;
	if (execute2enable)
		s->execute2			  = thread_execute2;
	if (!encoding && slice)
		s->slice_count		  = std::min(tbb::tbb_thread::hardware_concurrency(), MAX_THREADS);
	if (frame)
		s->thread_count		  = std::min(tbb::tbb_thread::hardware_concurrency(), MAX_THREADS); // We are using a task-scheduler, so use as many "threads/tasks" as possible. 

	CASPAR_LOG(info) << "Initialized ffmpeg tbb context.";
}

int tbb_avcodec_open(AVCodecContext* avctx, AVCodec* codec, AVDictionary** options, bool encoding)
{
	AVCodecID supported_codecs[] = {AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_PRORES, AV_CODEC_ID_FFV1, AV_CODEC_ID_H264, AV_CODEC_ID_HEVC };

	// Some codecs don't like to have multiple multithreaded decoding instances. Only enable for those we know work.
	if(std::find(std::begin(supported_codecs), std::end(supported_codecs), codec->id) != std::end(supported_codecs) && 
	  ((codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) && (avctx->thread_type & FF_THREAD_SLICE)
	  ||  (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)) && (avctx->thread_type & FF_THREAD_FRAME)) 
	{
		thread_init(avctx, codec->id != AV_CODEC_ID_PRORES, encoding, (avctx->thread_type & FF_THREAD_FRAME) != 0 , (avctx->thread_type & FF_THREAD_SLICE) != 0); // do not enable execute2 for prores codec as it cause crash
	}	
	avctx->refcounted_frames = 0;
	return avcodec_open2(avctx, codec, options); 
}

}