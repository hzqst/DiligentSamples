// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once
#include "AsyncPrefix.hpp"
#include "TextureLoader.h"

namespace Diligent
{
inline void CreateSharedTextureFromFile(const char* Path, const TextureLoadInfo& Info, IRenderDevice* Device, ITexture** Texture)
{
    RefCntAutoPtr<ITextureLoader> Loader;
    CreateTextureLoaderFromFile(Path, IMAGE_FILE_FORMAT_UNKNOWN, Info, &Loader);
    if (!Loader) return;
    auto Desc                 = Loader->GetTextureDesc();
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    auto Data                 = Loader->GetTextureData();
    Device->CreateTexture(Desc, &Data, Texture);
}
} // namespace Diligent
