//*****************************************************************************
//
//	Copyright 2015 Microsoft Corporation
//
//	Licensed under the Apache License, Version 2.0 (the "License");
//	you may not use this file except in compliance with the License.
//	You may obtain a copy of the License at
//
//	http ://www.apache.org/licenses/LICENSE-2.0
//
//	Unless required by applicable law or agreed to in writing, software
//	distributed under the License is distributed on an "AS IS" BASIS,
//	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//	See the License for the specific language governing permissions and
//	limitations under the License.
//
//*****************************************************************************

#include "pch.h"
#include "UncompressedVideoSampleProvider.h"
#include "NativeBufferFactory.h"
#include <mfapi.h>

extern "C"
{
#include <libavutil/imgutils.h>
}


using namespace FFmpegInterop;
using namespace NativeBuffer;
using namespace Windows::Media::MediaProperties;

UncompressedVideoSampleProvider::UncompressedVideoSampleProvider(
	FFmpegReader^ reader,
	AVFormatContext* avFormatCtx,
	AVCodecContext* avCodecCtx)
	: UncompressedSampleProvider(reader, avFormatCtx, avCodecCtx)
{
	switch (m_pAvCodecCtx->pix_fmt)
	{
	case AV_PIX_FMT_YUV420P:
		m_OutputPixelFormat = AV_PIX_FMT_YUV420P;
		OutputMediaSubtype = MediaEncodingSubtypes::Iyuv;
		break;
	case AV_PIX_FMT_YUVJ420P:
		m_OutputPixelFormat = AV_PIX_FMT_YUVJ420P;
		OutputMediaSubtype = MediaEncodingSubtypes::Iyuv;
		break;
	case AV_PIX_FMT_YUVA420P:
		m_OutputPixelFormat = AV_PIX_FMT_BGRA;
		OutputMediaSubtype = MediaEncodingSubtypes::Argb32;
		break;
	default:
		m_OutputPixelFormat = AV_PIX_FMT_NV12;
		OutputMediaSubtype = MediaEncodingSubtypes::Nv12;
		break;
	}

	auto width = avCodecCtx->width;
	auto height = avCodecCtx->height;

	if (m_pAvCodecCtx->pix_fmt == m_OutputPixelFormat)
	{
		// if no scaler is used, we need to check decoder frame size
		avcodec_align_dimensions(m_pAvCodecCtx, &width, &height);
		
		// try direct buffer approach, if supported by decoder
		if (m_pAvCodecCtx->codec->capabilities & AV_CODEC_CAP_DR1)
		{
			m_pAvCodecCtx->get_buffer2 = get_buffer2;
			m_pAvCodecCtx->opaque = (void*)this;
			m_bUseDirectBuffer = true;
		}
		else
		{
			m_bUseDirectBuffer = false;
		}
	}
	else
	{
		// scaler also uses direct buffer
		m_bUseDirectBuffer = true;
	}

	DecoderWidth = width;
	DecoderHeight = height;
}


HRESULT UncompressedVideoSampleProvider::InitializeScalerIfRequired(AVFrame *frame)
{
	HRESULT hr = S_OK;
	if (!m_bIsInitialized)
	{
		m_bIsInitialized = true;
		bool needsScaler = m_pAvCodecCtx->pix_fmt != m_OutputPixelFormat;
		if (needsScaler && SUCCEEDED(hr))
		{
			// Setup software scaler to convert any unsupported decoder pixel format to NV12 that is supported in Windows & Windows Phone MediaElement
			m_pSwsCtx = sws_getContext(
				m_pAvCodecCtx->width,
				m_pAvCodecCtx->height,
				m_pAvCodecCtx->pix_fmt,
				m_pAvCodecCtx->width,
				m_pAvCodecCtx->height,
				m_OutputPixelFormat,
				SWS_BICUBIC,
				NULL,
				NULL,
				NULL);

			if (m_pSwsCtx == nullptr)
			{
				hr = E_OUTOFMEMORY;
			}
		}
	}

	return hr;
}

UncompressedVideoSampleProvider::~UncompressedVideoSampleProvider()
{
	if (m_pAvFrame)
	{
		av_frame_free(&m_pAvFrame);
	}

	if (m_pSwsCtx)
	{
		sws_freeContext(m_pSwsCtx);
	}

	if (m_pBufferPool)
	{
		av_buffer_pool_uninit(&m_pBufferPool);
	}
}

HRESULT UncompressedVideoSampleProvider::DecodeAVPacket(DataWriter^ dataWriter, AVPacket* avPacket, int64_t& framePts, int64_t& frameDuration)
{
	HRESULT hr = S_OK;
	hr = UncompressedSampleProvider::DecodeAVPacket(dataWriter, avPacket, framePts, frameDuration);

	// Don't set a timestamp on S_FALSE
	if (hr == S_OK)
	{
		// Try to get the best effort timestamp for the frame.
		framePts = av_frame_get_best_effort_timestamp(m_pAvFrame);
		m_interlaced_frame = m_pAvFrame->interlaced_frame == 1;
		m_top_field_first = m_pAvFrame->top_field_first == 1;
	}

	return hr;
}

MediaStreamSample^ UncompressedVideoSampleProvider::GetNextSample()
{
	MediaStreamSample^ sample = MediaSampleProvider::GetNextSample();

	if (sample != nullptr)
	{
		if (m_interlaced_frame)
		{
			sample->ExtendedProperties->Insert(MFSampleExtension_Interlaced, TRUE);
			sample->ExtendedProperties->Insert(MFSampleExtension_BottomFieldFirst, m_top_field_first ? safe_cast<Platform::Object^>(FALSE) : TRUE);
			sample->ExtendedProperties->Insert(MFSampleExtension_RepeatFirstField, safe_cast<Platform::Object^>(FALSE));
		}
		else
		{
			sample->ExtendedProperties->Insert(MFSampleExtension_Interlaced, safe_cast<Platform::Object^>(FALSE));
		}
	}

	return sample;
}

HRESULT UncompressedVideoSampleProvider::WriteAVPacketToStream(DataWriter^ dataWriter, AVPacket* avPacket)
{
	auto hr = InitializeScalerIfRequired(m_pAvFrame);

	if (SUCCEEDED(hr))
	{
		if (m_pSwsCtx == nullptr)
		{
			if (!m_bUseDirectBuffer)
			{
				// ffmpeg does not allocate contiguous buffers for YUV, so we need to manually copy all three planes
				auto YBuffer = Platform::ArrayReference<uint8_t>(m_pAvFrame->data[0], m_pAvFrame->linesize[0] * m_pAvCodecCtx->height);
				auto UBuffer = Platform::ArrayReference<uint8_t>(m_pAvFrame->data[1], m_pAvFrame->linesize[1] * m_pAvCodecCtx->height / 2);
				auto VBuffer = Platform::ArrayReference<uint8_t>(m_pAvFrame->data[2], m_pAvFrame->linesize[2] * m_pAvCodecCtx->height / 2);
				dataWriter->WriteBytes(YBuffer);
				dataWriter->WriteBytes(UBuffer);
				dataWriter->WriteBytes(VBuffer);
			}
			else
			{
				// create a buffer reference to hand out to MSS pipeline
				auto bufferRef = av_buffer_ref(m_pAvFrame->buf[0]);
				if (bufferRef == NULL)
				{
					hr = E_FAIL;
				}
				else
				{
					m_pDirectBuffer = NativeBufferFactory::CreateNativeBuffer(bufferRef->data, bufferRef->size, free_buffer, bufferRef);
				}
			}
		}
		else
		{
			// allocate a new frame from buffer pool
			auto frame = ref new FrameDataHolder();
			hr = AllocateScalerFrame(frame);
			if (SUCCEEDED(hr))
			{
				// Convert decoded video pixel format to output format using FFmpeg software scaler
				if (sws_scale(m_pSwsCtx, (const uint8_t **)(m_pAvFrame->data), m_pAvFrame->linesize, 0, m_pAvCodecCtx->height, frame->data, frame->linesize) < 0)
				{
					free_buffer(frame->buffer);
					hr = E_FAIL;
				}
				else
				{
					auto bufferRef = frame->buffer;
					m_pDirectBuffer = NativeBufferFactory::CreateNativeBuffer(bufferRef->data, bufferRef->size, free_buffer, bufferRef);
				}
			}
		}
	}

	av_frame_unref(m_pAvFrame);
	av_frame_free(&m_pAvFrame);

	return hr;
}

HRESULT UncompressedVideoSampleProvider::AllocateScalerFrame(FrameDataHolder^ frame)
{
	if (av_image_fill_linesizes(frame->linesize, m_OutputPixelFormat, DecoderWidth) < 0)
	{
		return E_FAIL;
	}
	if (frame->linesize[0] != DecoderWidth)
	{
		return E_FAIL; // unexpected size change cannot be handled
	}

	auto YBufferSize = frame->linesize[0] * DecoderHeight;
	auto UBufferSize = frame->linesize[1] * DecoderHeight / 2;
	auto VBufferSize = frame->linesize[2] * DecoderHeight / 2;
	auto totalSize = YBufferSize + UBufferSize + VBufferSize;

	auto buffer = AllocateBuffer(totalSize);
	if (!buffer)
	{
		return E_OUTOFMEMORY;
	}

	frame->data[0] = buffer->data;
	frame->data[1] = buffer->data + YBufferSize;
	frame->data[2] = buffer->data + YBufferSize + UBufferSize;
	frame->data[3] = NULL;
	
	frame->buffer = buffer;
	
	return S_OK;
}

AVBufferRef* UncompressedVideoSampleProvider::AllocateBuffer(int totalSize)
{
	if (!m_pBufferPool)
	{
		m_pBufferPool = av_buffer_pool_init(totalSize, NULL);
		if (!m_pBufferPool)
		{
			return NULL;
		}
	}

	auto buffer = av_buffer_pool_get(m_pBufferPool);
	if (!buffer)
	{
		return NULL;
	}
	if (buffer->size != totalSize)
	{
		free_buffer(buffer);
		return NULL;
	}

	return buffer;
}

void UncompressedVideoSampleProvider::free_buffer(void *lpVoid)
{
	auto buffer = (AVBufferRef *)lpVoid;
	auto count = av_buffer_get_ref_count(buffer);
	av_buffer_unref(&buffer);
}

int UncompressedVideoSampleProvider::get_buffer2(AVCodecContext *avCodecContext, AVFrame *frame, int flags)
{
	auto provider = reinterpret_cast<UncompressedVideoSampleProvider^>(avCodecContext->opaque);

	auto width = frame->width;
	auto height = frame->height;
	avcodec_align_dimensions(avCodecContext, &width, &height);

	if (width != provider->DecoderWidth || height != provider->DecoderHeight)
	{
		return ERROR; // unexpected size change cannot be handled
	}

	frame->linesize[0] = width;
	frame->linesize[1] = width / 2;
	frame->linesize[2] = width / 2;
	frame->linesize[3] = 0;

	auto YBufferSize = frame->linesize[0] * height;
	auto UBufferSize = frame->linesize[1] * height / 2;
	auto VBufferSize = frame->linesize[2] * height / 2;
	auto totalSize = YBufferSize + UBufferSize + VBufferSize;

	auto buffer = provider->AllocateBuffer(totalSize);
	if (!buffer)
	{
		return ERROR;
	}

	frame->buf[0] = buffer;
	frame->data[0] = buffer->data;
	frame->data[1] = buffer->data + YBufferSize;
	frame->data[2] = buffer->data + YBufferSize + UBufferSize;
	frame->data[3] = NULL;

	return 0;
}
