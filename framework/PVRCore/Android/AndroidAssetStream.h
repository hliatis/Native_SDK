/*!
\brief A Stream implementation used to access Android resources.
\file PVRCore/Android/AndroidAssetStream.h
\author PowerVR by Imagination, Developer Technology Team
\copyright Copyright (c) Imagination Technologies Limited.
*/
#pragma once
#include "PVRCore/stream/Stream.h"

struct AAssetManager;
struct AAsset;

namespace pvr {
/// <summary>A Stream implementation that is used to access resources built in an Android package (apk).
/// </summary>
/// <remarks>This Stream abstraction allows the user to easily access the Resources embedded in an Android .apk
/// package. This is the default way resources are packed in the Android version of the PowerVR Examples.
/// </remarks>
class AndroidAssetStream : public Stream
{
public:
	/// <summary>Constructor from Android NDK Asset manager and a filename</summary>
	/// <param name="assetManager">The Android Asset manager object the app will use</param>
	/// <param name="filename">The file (asset) to open</param>
	AndroidAssetStream(AAssetManager* assetManager, const std::string& filename): Stream(filename), assetManager(assetManager), _asset(NULL)
	{
		_isReadable = true;
	}

	/// <summary>Destructor. Releases this object</summary>
	~AndroidAssetStream()
	{
		close();
	}

	void read(size_t size, size_t count, void* const outData, size_t& outElementsRead) const;
	void write(size_t size, size_t count, const void* data, size_t& dataWritten);
	void seek(long offset, SeekOrigin origin) const;
	void open() const;
	void close();

	virtual bool isopen() const;
	virtual size_t getPosition() const;
	virtual size_t getSize() const;
    
	/// <summary>Retrieves the current android asset manager</summary>
	/// <returns>The current android asset manager</returns>
	AAssetManager* getAndroidAssetManager()
	{
		return assetManager;
	}

private:
	AAssetManager* const assetManager;
	mutable AAsset* _asset;
};
} // namespace pvr