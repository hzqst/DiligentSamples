/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

// Turns a loaded RTXPTScene and its RTXPTMaterials into RTXPTSceneRuntime assets and instances.

#include <string>
#include <vector>

#include "DeviceContext.h"
#include "RenderDevice.h"
#include "RTXPTSceneGraph.hpp"
#include "RTXPTSceneRuntime.hpp"

namespace Diligent
{

class RTXPTScene;
class RTXPTMaterials;

struct RTXPTImportedSceneInstance
{
    RTXPTInstanceHandle Handle;
    RTXPTSceneId        ModelInstanceId = InvalidRTXPTSceneId;
    Int32               NodeIndex       = -1;
    // The joint matrices of skinned nodes: ModelTransforms::Skins[SkinTransformsIndex], or -1 for rigid nodes.
    Int32  SkinTransformsIndex = -1;
    Uint32 JointCount          = 0;
};

// The runtime objects of an imported scene. Each model asset becomes one geometry with a mesh per glTF mesh, plus one
// skinned geometry per glTF skin that a node uses; each scene material becomes one material, and each mesh node of a
// model instance one instance. Skinned instances take their pose from the joint matrices of their skin.
struct RTXPTImportedScene
{
    // Per model asset; invalid for assets without a model.
    std::vector<RTXPTGeometryHandle> Geometries;
    // Per model asset and glTF skin; invalid until a node uses the skin.
    std::vector<std::vector<RTXPTGeometryHandle>> SkinnedGeometries;
    // Per model asset and glTF mesh: the geometry mesh, or -1 for meshes without triangles.
    std::vector<std::vector<Int32>> MeshIndices;
    // Per scene material (RTXPTModelAsset::MaterialRemap).
    std::vector<RTXPTMaterialHandle>        Materials;
    std::vector<RTXPTImportedSceneInstance> Instances;
};

// Registers the geometry and materials of Scene and, in the open update of Runtime, creates the instances of its model
// instances and sets its analytic lights. Geometry is copied from the model buffers when a frame builder first uses it.
// On failure the assets are released; the caller cancels the update.
RTXPTErrorCode ImportRTXPTScene(IRenderDevice*        pDevice,
                                IDeviceContext*       pContext,
                                const RTXPTScene&     Scene,
                                const RTXPTMaterials& Materials,
                                RTXPTSceneRuntime&    Runtime,
                                RTXPTImportedScene&   Imported);

// Sets the instance transforms and poses of the current animation state of Scene in the open update of Runtime.
RTXPTErrorCode UpdateRTXPTImportedSceneTransforms(const RTXPTScene& Scene, const RTXPTImportedScene& Imported, RTXPTSceneRuntime& Runtime);

// Releases the assets of Imported from the repository and forgets them; existing instances keep their assets.
void ReleaseRTXPTImportedScene(RTXPTSceneRuntime& Runtime, RTXPTImportedScene& Imported);

} // namespace Diligent
