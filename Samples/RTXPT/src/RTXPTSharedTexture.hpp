// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once
#include "AsyncPrefix.hpp"
#include "TextureLoader.h"

namespace Diligent
{
inline void CreateSharedTextureFromLoader(ITextureLoader* Loader, IRenderDevice* Device, ITexture** Texture)
{
    if (!Loader) return;
    auto Desc                 = Loader->GetTextureDesc();
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    auto Data                 = Loader->GetTextureData();
    Device->CreateTexture(Desc, &Data, Texture);
}

inline void CreateSharedTextureFromFile(const char* Path, const TextureLoadInfo& Info, IRenderDevice* Device, ITexture** Texture)
{
    RefCntAutoPtr<ITextureLoader> Loader;
    CreateTextureLoaderFromFile(Path, IMAGE_FILE_FORMAT_UNKNOWN, Info, &Loader);
    CreateSharedTextureFromLoader(Loader, Device, Texture);
}

// The loader does not copy Data, which only has to outlive this call.
inline void CreateSharedTextureFromMemory(const void* Data, size_t Size, const TextureLoadInfo& Info, IRenderDevice* Device, ITexture** Texture)
{
    if (Data == nullptr || Size == 0) return;
    RefCntAutoPtr<ITextureLoader> Loader;
    CreateTextureLoaderFromMemory(Data, Size, false, Info, &Loader);
    CreateSharedTextureFromLoader(Loader, Device, Texture);
}
} // namespace Diligent
