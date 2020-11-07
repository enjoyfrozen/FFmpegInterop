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

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Core;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Storage::Streams;
using namespace std;

namespace winrt::FFmpegInterop::implementation
{
	tuple<bool, GUID> UncompressedVideoSampleProvider::MapAVSampleFormatToMFVideoFormat(_In_ AVPixelFormat format) noexcept
	{
		bool isSupportedFormat{ true };
		GUID mfVideoFormat{ GUID_NULL };

		switch (format)
		{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
			mfVideoFormat = MFVideoFormat_IYUV;
			break;

		default:
			static_assert(DEFAULT_FORMAT == AV_PIX_FMT_NV12, "Need to update default case");
			isSupportedFormat = false;
			__fallthrough;

		case AV_PIX_FMT_NV12:
			mfVideoFormat = MFVideoFormat_NV12;
			break;
		}

		return { isSupportedFormat, mfVideoFormat };
	}

	UncompressedVideoSampleProvider::UncompressedVideoSampleProvider(_In_ const AVFormatContext* formatContext, _In_ AVStream* stream, _In_ Reader& reader, _In_ uint32_t allowedDecodeErrors) :
		UncompressedSampleProvider(formatContext, stream, reader, allowedDecodeErrors, &InitCodecContext),
		m_outputWidth(m_codecContext->width),
		m_outputHeight(m_codecContext->height)
	{
		if (!IsSupportedFormat(m_codecContext->pix_fmt))
		{
			// We need to convert to a supported format
			InitScaler();
		}
		else
		{
			// We need a buffer pool for copying the image data
			InitBufferPool(m_codecContext->pix_fmt);
		}
	}

	void UncompressedVideoSampleProvider::InitCodecContext(_In_ AVCodecContext* codecContext) noexcept
	{
		codecContext->get_format = &GetFormat; // Try to negotiate an AVPixelFormat we support so that we don't need to use the scaler
	}

	AVPixelFormat UncompressedVideoSampleProvider::GetFormat(_In_ AVCodecContext* codecContext, _In_ const AVPixelFormat* formats) noexcept
	{
		// Try to select a format we support so that we don't need to use the scaler
		for (uint32_t i{ 0 }; formats[i] != AV_PIX_FMT_NONE; i++)
		{
			if (IsSupportedFormat(formats[i]))
			{
				return formats[i];
			}
		}

		// None of our supported formats are an option
		return avcodec_default_get_format(codecContext, formats);
	}

	void UncompressedVideoSampleProvider::InitScaler()
	{
		// Setup software scaler to convert the image to the desired output format
		m_swsContext.reset(sws_getContext(
			m_outputWidth,
			m_outputHeight,
			m_codecContext->pix_fmt,
			m_outputWidth,
			m_outputHeight,
			DEFAULT_FORMAT,
			SWS_BICUBIC,
			nullptr,
			nullptr,
			nullptr));
		THROW_IF_NULL_ALLOC(m_swsContext);

		InitBufferPool(DEFAULT_FORMAT);
	}

	void UncompressedVideoSampleProvider::InitBufferPool(_In_ AVPixelFormat format)
	{
		// Get the buffer size
		int requiredBufferSize{ av_image_get_buffer_size(format, m_outputWidth, m_outputHeight, 1) };
		THROW_HR_IF_FFMPEG_FAILED(requiredBufferSize);

		// Allocate a new buffer pool
		m_bufferPool.reset(av_buffer_pool_init(requiredBufferSize, nullptr));
		THROW_IF_NULL_ALLOC(m_bufferPool);

		// Update the line sizes
		THROW_HR_IF_FFMPEG_FAILED(av_image_fill_linesizes(m_lineSizes, format, m_outputWidth));
	}

	void UncompressedVideoSampleProvider::SetEncodingProperties(_Inout_ const IMediaEncodingProperties& encProp, _In_ bool setFormatUserData)
	{
		SampleProvider::SetEncodingProperties(encProp, setFormatUserData);

		VideoEncodingProperties videoEncProp{ encProp.as<VideoEncodingProperties>() };
		videoEncProp.Subtype(to_hstring(GetMFVideoFormat(m_codecContext->pix_fmt)));

		if (m_codecContext->framerate.num != 0 && m_codecContext->framerate.den != 0)
		{
			MediaRatio frameRate{ videoEncProp.FrameRate() };
			frameRate.Numerator(m_codecContext->framerate.num);
			frameRate.Denominator(m_codecContext->framerate.den);
		}

		MediaPropertySet videoProp{ videoEncProp.Properties() };
		videoProp.Insert(MF_MT_INTERLACE_MODE, PropertyValue::CreateUInt32(MFVideoInterlace_MixedInterlaceOrProgressive));
	}

	tuple<IBuffer, int64_t, int64_t, vector<pair<GUID, Windows::Foundation::IInspectable>>, vector<pair<GUID, Windows::Foundation::IInspectable>>> UncompressedVideoSampleProvider::GetSampleData()
	{
		// Get the next decoded sample
		AVFrame_ptr frame;
		uint32_t decodeErrors{ 0 };

		while (true)
		{
			try
			{
				frame = GetFrame();
				break;
			}
			catch (...)
			{
				const hresult hr{ to_hresult() };
				switch (hr)
				{
				case MF_E_END_OF_STREAM: // We've reached EOF. Nothing more to do.
				case E_OUTOFMEMORY: // Always treat as fatal error
					throw;

				default:
					// Unexpected decode error
					if (decodeErrors < m_allowedDecodeErrors)
					{
						decodeErrors++;
						TraceLoggingWrite(g_FFmpegInteropProvider, "AllowedDecodeError", TraceLoggingLevel(TRACE_LEVEL_VERBOSE), TraceLoggingPointer(this, "this"),
							TraceLoggingValue(m_stream->index, "StreamId"),
							TraceLoggingValue(decodeErrors, "DecodeErrorCount"),
							TraceLoggingValue(m_allowedDecodeErrors, "DecodeErrorLimit"));

						m_isDiscontinuous = true;
					}
					else
					{
						throw;
					}

					break;
				}
			}
		}

		// Check for dynamic format changes
		vector<pair<GUID, Windows::Foundation::IInspectable>> formatChanges{ CheckForFormatChanges(frame.get()) };

		// Get the sample buffer
		IBuffer sampleBuf{ nullptr };
		if (!IsUsingScaler())
		{
			// The image is already in the desired output format. Copy the image data into a single buffer.
			AVBufferRef_ptr bufferRef{ av_buffer_pool_get(m_bufferPool.get()) };
			THROW_IF_NULL_ALLOC(bufferRef);
			THROW_HR_IF_FFMPEG_FAILED(av_image_copy_to_buffer(bufferRef->data, bufferRef->size, frame->data, frame->linesize, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, 1));
			sampleBuf = make<FFmpegInteropBuffer>(move(bufferRef));
		}
		else
		{
			// Scale the image to the desired output format
			AVBufferRef_ptr bufferRef{ av_buffer_pool_get(m_bufferPool.get()) };
			THROW_IF_NULL_ALLOC(bufferRef);

			uint8_t* data[4]{ };
			const int requiredBufferSize{ av_image_fill_pointers(data, DEFAULT_FORMAT, frame->height, bufferRef->data, m_lineSizes) };
			THROW_HR_IF_FFMPEG_FAILED(requiredBufferSize);
			THROW_HR_IF(MF_E_BUFFERTOOSMALL, requiredBufferSize > bufferRef->size);

			THROW_HR_IF_FFMPEG_FAILED(sws_scale(m_swsContext.get(), frame->data, frame->linesize, 0, frame->height, data, m_lineSizes));

			sampleBuf = make<FFmpegInteropBuffer>(move(bufferRef));
		}

		// Get the sample properties
		vector<pair<GUID, Windows::Foundation::IInspectable>> properties{ GetSampleProperties(frame.get()) };

		return { move(sampleBuf), frame->best_effort_timestamp, frame->pkt_duration, move(properties), move(formatChanges) };
	}

	vector<pair<GUID, Windows::Foundation::IInspectable>> UncompressedVideoSampleProvider::CheckForFormatChanges(_In_ const AVFrame* frame)
	{
		vector<pair<GUID, Windows::Foundation::IInspectable>> formatChanges;

		// Check if the resolution changed
		if (frame->width != m_outputWidth || frame->height != m_outputHeight)
		{
			TraceLoggingWrite(g_FFmpegInteropProvider, "ResolutionChanged", TraceLoggingLevel(TRACE_LEVEL_VERBOSE), TraceLoggingPointer(this, "this"),
				TraceLoggingValue(m_stream->index, "StreamId"),
				TraceLoggingValue(m_outputWidth, "OldWidth"),
				TraceLoggingValue(m_outputHeight, "OldHeight"),
				TraceLoggingValue(frame->width, "NewWidth"),
				TraceLoggingValue(frame->height, "NewHeight"));

			m_outputWidth = frame->width;
			m_outputHeight = frame->height;
			formatChanges.emplace_back(MF_MT_FRAME_SIZE, PropertyValue::CreateUInt64(Pack2UINT32AsUINT64(m_outputWidth, m_outputHeight)));

			// Reinitialze the scaler if needed
			if (IsUsingScaler())
			{
				InitScaler();
			}
		}

		return formatChanges;
	}

	vector<pair<GUID, Windows::Foundation::IInspectable>> UncompressedVideoSampleProvider::GetSampleProperties(_In_ const AVFrame* frame)
	{
		vector<pair<GUID, Windows::Foundation::IInspectable>> properties;

		if (frame->interlaced_frame)
		{
			properties.emplace_back(MFSampleExtension_Interlaced, PropertyValue::CreateUInt32(true));
			properties.emplace_back(MFSampleExtension_BottomFieldFirst, PropertyValue::CreateUInt32(!frame->top_field_first));
			properties.emplace_back(MFSampleExtension_RepeatFirstField, PropertyValue::CreateUInt32(false));
		}
		else
		{
			properties.emplace_back(MFSampleExtension_Interlaced, PropertyValue::CreateUInt32(false));
		}

		return properties;
	}
}
