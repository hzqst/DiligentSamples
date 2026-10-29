#pragma once

#include "RenderDevice.h"
#include "PipelineState.h"
#include "PipelineStateCache.h"

namespace Diligent
{
//Returns the native pipeline cache a pass should put in PipelineStateCreateInfo::pPSOCache, or null to bypass it.
//NVIDIA's D3D12 pipeline library can restore the wrong static sampler when root signatures are recreated in a
//different order, so pipelines with immutable samplers, in the resource layout or in a signature, bypass it there.
//Call it after the resource layout and signatures of CreateInfo are set.
inline IPipelineStateCache* SelectRTXPTPipelineStateCache(IRenderDevice* pDevice, const PipelineStateCreateInfo& CreateInfo, IPipelineStateCache* pPSOCache)
{
    if (pPSOCache == nullptr || pDevice == nullptr)
        return nullptr;

    if (pDevice->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D12 && pDevice->GetAdapterInfo().Vendor == ADAPTER_VENDOR_NVIDIA)
    {
        if (CreateInfo.PSODesc.ResourceLayout.NumImmutableSamplers != 0)
            return nullptr;

        for (Uint32 i = 0; i < CreateInfo.ResourceSignaturesCount; ++i)
        {
            const IPipelineResourceSignature* pSignature = CreateInfo.ppResourceSignatures[i];
            if (pSignature != nullptr && pSignature->GetDesc().NumImmutableSamplers != 0)
                return nullptr;
        }
    }

    return pPSOCache;
}
} // namespace Diligent
