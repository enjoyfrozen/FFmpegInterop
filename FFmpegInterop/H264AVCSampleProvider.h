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

#pragma once

#include "MediaSampleProvider.h"

namespace winrt::FFmpegInterop::implementation
{
	class H264AVCSampleProvider :
		public MediaSampleProvider
	{
	public:
		H264AVCSampleProvider(_In_ const AVStream* stream, _Inout_ FFmpegReader& reader);

	protected:
		std::tuple<Windows::Storage::Streams::IBuffer, int64_t, int64_t> GetSampleData() override;

	private:
		void WriteSPSAndPPS(
			_Inout_opt_ Windows::Storage::Streams::DataWriter& dataWriter,
			_In_reads_(codecPrivateDataSize) const uint8_t* codecPrivateData,
			_In_ uint32_t codecPrivateDataSize);

		uint32_t WriteParameterSetData(
			_Inout_opt_ Windows::Storage::Streams::DataWriter& dataWriter,
			_In_ uint8_t parameterSetCount,
			_In_reads_(parameterSetDataSize) const uint8_t* parameterSetData,
			_In_ uint32_t parameterSetDataSize);

		void WriteNALPacket(
			_Inout_opt_ Windows::Storage::Streams::DataWriter& dataWriter,
			_In_reads_(packetDataSize) const uint8_t* packetData,
			_In_ uint32_t packetDataSize);

		bool m_isAVC; // Indicates whether bitstream format is AVC or Annex B
		uint8_t m_nalLenSize; // Not used for Annex B
		Windows::Storage::Streams::DataWriter m_dataWriter; // TODO: Reuse data writer?
	};
}
