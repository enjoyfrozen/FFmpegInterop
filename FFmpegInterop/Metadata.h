//*****************************************************************************
//
//	Copyright 2019 Microsoft Corporation
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

namespace winrt::FFmpegInterop
{
	struct FFmpegInteropMSSConfig;
}

namespace winrt::FFmpegInterop::implementation
{
	com_ptr<IPropertyStore> GetPropertyHandler(_In_ const Windows::Media::Core::MediaStreamSource& mss);
	void PopulateMetadata(_In_ const Windows::Media::Core::MediaStreamSource& mss, _In_ const AVDictionary* metadata);
	void SetThumbnail(
		_In_ const Windows::Media::Core::MediaStreamSource& mss,
		_In_ const AVStream* thumbnailStream, 
		_In_opt_ const FFmpegInterop::FFmpegInteropMSSConfig& config);
}
