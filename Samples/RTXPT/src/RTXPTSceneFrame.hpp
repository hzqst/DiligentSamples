/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

// The GPU form of RTXPTSceneRuntime snapshots. A frame builder keeps the geometry, BLASes, materials and textures of
// the snapshots it prepares resident across frames, and rebuilds the TLAS, the sub-instance tables and the emissive
// triangles of each prepared snapshot. Every skinned instance has its own skinned vertices and BLAS; they are skinned
// again and the BLAS updated only when its pose changes.

#include <memory>
#include <string>

#include "Buffer.h"
#include "DeviceContext.h"
#include "DeviceObject.h"
#include "EngineFactory.h"
#include "RenderDevice.h"
#include "TopLevelAS.h"
#include "RTXPTLights.hpp"
#include "RTXPTSceneRuntime.hpp"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;
class RTXPTGpuProfiler;

struct RTXPTSceneFrameBuilderCreateInfo
{
    IEngineFactory*      pEngineFactory = nullptr;
    IRenderStateCache*   pStateCache    = nullptr;
    IPipelineStateCache* pPSOCache      = nullptr;
};

// Work counters since Initialize, and the size of the last prepared frame.
struct RTXPTSceneFrameStats
{
    Uint64 PrepareCount           = 0;
    Uint64 TLASBuildCount         = 0;
    Uint64 TLASCreateCount        = 0;
    Uint64 RigidBLASBuildCount    = 0;
    Uint64 SkinnedBLASBuildCount  = 0;
    Uint64 SkinnedBLASUpdateCount = 0;
    Uint64 SkinningDispatchCount  = 0;
    Uint64 ArenaGrowCount         = 0;
    Uint64 TextureTableGrowCount  = 0;
    Uint64 AssetUploadBytes       = 0;
    Uint64 EmissiveBuildCount     = 0;
    Uint64 PipelineCompileCount   = 0;

    Uint32 InstanceCount         = 0;
    Uint32 SubInstanceCount      = 0;
    Uint32 ResidentGeometryCount = 0;
    Uint32 ResidentMaterialCount = 0;
    Uint32 BLASCount             = 0;
    Uint32 SkinnedInstanceCount  = 0;
    Uint32 TextureCount          = 0;
    Uint32 TextureCapacity       = 0;
    Uint32 EmissiveTriangleCount = 0;
};

// The GPU scene of one prepared snapshot. The objects belong to the builder and stay valid until its next Prepare or
// Reset: record every use of them before that. Revisions tell users what to refresh.
struct RTXPTSceneFrame
{
    RTXPTSceneSnapshotPtr Snapshot;
    // Identifies this build of pTLAS. Shader binding tables follow its instances.
    Uint64 BuildIndex = 0;
    // Changes when any object below changes, so resource bindings must be recreated.
    Uint64 BindingRevision = 0;
    // Changes when the analytic lights or the emissive light sources change, so light sampling must be rebuilt
    // from pLights.
    Uint64 LightingRevision = 0;
    // Changes when the emissive triangles were rebuilt with unchanged sources.
    Uint64 EmissiveRevision = 0;

    ITopLevelAS*          pTLAS                       = nullptr;
    IBuffer*              pVertexBuffer               = nullptr;
    IBuffer*              pSkinnedVertexBuffer        = nullptr;
    IBuffer*              pIndexBuffer                = nullptr;
    IBuffer*              pSubInstanceBuffer          = nullptr;
    IBuffer*              pSubInstanceTransformBuffer = nullptr;
    IBuffer*              pMaterialBuffer             = nullptr;
    IDeviceObject* const* ppMaterialTextures          = nullptr;
    // The size of the bindless table; unused entries hold a placeholder texture.
    Uint32             MaterialTextureCount = 0;
    const RTXPTLights* pLights              = nullptr;

    Uint32 InstanceCount    = 0;
    Uint32 SubInstanceCount = 0;
};

// Prepares the GPU frames of the snapshots of one RTXPTSceneRuntime on one device. All GPU work of a builder must be
// recorded on contexts of one queue in the order of the calls.
class RTXPTSceneFrameBuilder
{
public:
    RTXPTSceneFrameBuilder();
    ~RTXPTSceneFrameBuilder();
    RTXPTSceneFrameBuilder(const RTXPTSceneFrameBuilder&) = delete;
    RTXPTSceneFrameBuilder& operator=(const RTXPTSceneFrameBuilder&) = delete;

    RTXPTErrorCode Initialize(IRenderDevice* pDevice, const RTXPTSceneFrameBuilderCreateInfo& CreateInfo);
    // Releases every GPU object. Initialize makes the builder usable again.
    void Reset();
    bool IsInitialized() const;

    // Makes Snapshot resident and records its frame on pContext: uploads new assets, builds missing BLASes, rebuilds
    // the TLAS and the sub-instance tables, and the emissive triangles when they changed. On failure the previous
    // frame is no longer valid.
    RTXPTErrorCode Prepare(IDeviceContext* pContext, const RTXPTSceneSnapshotPtr& Snapshot, RTXPTGpuProfiler* Profiler = nullptr);

    const RTXPTSceneFrame&      GetFrame() const;
    const RTXPTSceneFrameStats& GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Diligent
