// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Optional deterministic scene-linear capture; deliberately stalls only when requested.
#pragma once
#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>
#include "Texture.h"
#include "DeviceContext.h"
#include "RenderDevice.h"
#include "RefCntAutoPtr.hpp"
namespace Diligent
{
class HDRValidationCapture
{
public:
    std::string Path;
    Uint32      StartFrame = 256;
    Uint32      FrameCount = 64;
    void        Capture(IRenderDevice* Device, IDeviceContext* Context, ITexture* Texture, Uint32 Frame)
    {
        if (Path.empty() || m_Done || Frame < StartFrame || !Texture) return;
        const auto& Desc = Texture->GetDesc();
        if (Desc.Format != TEX_FORMAT_RGBA16_FLOAT)
        {
            LOG_ERROR_MESSAGE("HDR capture requires RGBA16_FLOAT");
            m_Done = true;
            return;
        }
        const size_t Count = size_t{Desc.Width} * Desc.Height * 3;
        if (m_Sum.size() != Count)
        {
            m_Sum.assign(Count, 0.f);
            m_Count = 0;
        }
        TextureDesc StagingDesc    = Desc;
        StagingDesc.Name           = "HDR validation readback";
        StagingDesc.Usage          = USAGE_STAGING;
        StagingDesc.BindFlags      = BIND_NONE;
        StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<ITexture> Staging;
        Device->CreateTexture(StagingDesc, nullptr, &Staging);
        if (!Staging)
        {
            m_Done = true;
            return;
        }
        CopyTextureAttribs Copy;
        Copy.pSrcTexture              = Texture;
        Copy.pDstTexture              = Staging;
        Copy.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Context->CopyTexture(Copy);
        Context->Flush();
        Context->WaitForIdle();
        MappedTextureSubresource Map;
        Context->MapTextureSubresource(Staging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Map);
        if (!Map.pData)
        {
            LOG_ERROR_MESSAGE("HDR capture map failed");
            m_Done = true;
            return;
        }
        for (Uint32 Y = 0; Y < Desc.Height; ++Y)
        {
            const auto* Row = reinterpret_cast<const Uint16*>(static_cast<const Uint8*>(Map.pData) + Y * Map.Stride);
            for (Uint32 X = 0; X < Desc.Width; ++X)
                for (Uint32 C = 0; C < 3; ++C)
                {
                    Uint16 H        = Row[X * 4 + C];
                    int    Exponent = (H >> 10) & 31;
                    float  Value    = Exponent == 0 ? std::ldexp(float(H & 1023), -24) :
                                                  (Exponent == 31 ? NAN : std::ldexp(float(1024 + (H & 1023)), Exponent - 25));
                    if (H & 0x8000) Value = -Value;
                    m_Sum[(size_t{Y} * Desc.Width + X) * 3 + C] += Value;
                }
        }
        Context->UnmapTextureSubresource(Staging, 0, 0);
        if (++m_Count >= std::max(FrameCount, 1u))
        {
            std::ofstream File{Path, std::ios::binary};
            File << "PF\n"
                 << Desc.Width << ' ' << Desc.Height << "\n-1.0\n";
            for (auto& Value : m_Sum) Value /= float(m_Count);
            for (Uint32 Y = Desc.Height; Y-- > 0;)
                File.write(reinterpret_cast<const char*>(m_Sum.data() + size_t{Y} * Desc.Width * 3), Desc.Width * 3 * sizeof(float));
            if (!File) LOG_ERROR_MESSAGE("HDR capture write failed: ", Path);
            else
                LOG_INFO_MESSAGE("HDR capture saved: ", Path, " (", m_Count, " frames)");
            m_Done = true;
        }
    }

private:
    bool               m_Done  = false;
    Uint32             m_Count = 0;
    std::vector<float> m_Sum;
};
} // namespace Diligent
