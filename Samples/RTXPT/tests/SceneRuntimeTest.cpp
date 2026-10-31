// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// RTXPTSceneRuntime without a device: asset validation, update transactions, handle generations, revisions and
// snapshot immutability.
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "../src/RTXPTSceneRuntime.hpp"
#include "../src/RTXPTSceneHistory.hpp"
using namespace Diligent;

static void Require(bool OK, const std::string& Text)
{
    if (!OK) throw std::runtime_error{Text};
}
static void RequireResult(RTXPTSceneResult Expected, RTXPTSceneResult Actual, const std::string& Text)
{
    if (Expected != Actual)
        throw std::runtime_error{Text + ": expected " + GetRTXPTSceneResultString(Expected) + ", got " + GetRTXPTSceneResultString(Actual)};
}

// One triangle, indexed, one material slot.
struct TriangleGeometry
{
    RTXPTGeometryVertex        Vertices[3];
    Uint32                     Indices[3] = {0, 1, 2};
    RTXPTSkinVertex            Skin[3];
    RTXPTGeometryPrimitiveDesc Primitive = {0, 3, 0, 0, 0};
    RTXPTGeometryMeshDesc      Mesh      = {&Primitive, 1};
    RTXPTGeometryDesc          Desc;

    explicit TriangleGeometry(Uint32 JointCount = 0)
    {
        Vertices[1].position = float3{1, 0, 0};
        Vertices[2].position = float3{0, 1, 0};
        for (RTXPTSkinVertex& V : Skin)
            V.weights = float4{1, 0, 0, 0};
        Desc.Name              = "triangle";
        Desc.VertexCount       = 3;
        Desc.IndexCount        = 3;
        Desc.pVertices         = Vertices;
        Desc.pIndices          = Indices;
        Desc.pSkin             = JointCount != 0 ? Skin : nullptr;
        Desc.pMeshes           = &Mesh;
        Desc.MeshCount         = 1;
        Desc.MaterialSlotCount = 1;
        Desc.JointCount        = JointCount;
    }
};

static RTXPTGeometryHandle RegisterTriangle(RTXPTSceneRuntime& Runtime, Uint32 JointCount = 0)
{
    TriangleGeometry    Geometry{JointCount};
    RTXPTGeometryHandle Handle;
    RequireResult(RTXPTSceneResult::Ok, Runtime.RegisterGeometry(Geometry.Desc, Handle), "register triangle");
    return Handle;
}

static RTXPTMaterialHandle RegisterMaterial(RTXPTSceneRuntime& Runtime, float Roughness = 1.0f)
{
    RTXPTMaterialDesc Desc;
    Desc.Name                 = "material";
    Desc.Data.roughnessFactor = Roughness;
    RTXPTMaterialHandle Handle;
    RequireResult(RTXPTSceneResult::Ok, Runtime.RegisterMaterial(Desc, Handle), "register material");
    return Handle;
}

static float4x4 Translation(float X)
{
    return float4x4::Translation(X, 0, 0);
}

static RTXPTInstanceDesc MakeInstance(RTXPTGeometryHandle Geometry, const RTXPTMaterialHandle& Material, float X = 0)
{
    RTXPTInstanceDesc Desc;
    Desc.Geometry      = Geometry;
    Desc.Transform     = Translation(X);
    Desc.pMaterials    = &Material;
    Desc.MaterialCount = 1;
    return Desc;
}

static void TestGeometryValidation()
{
    RTXPTSceneRuntime   Runtime;
    RTXPTGeometryHandle Handle;
    auto                Expect = [&](RTXPTSceneResult Expected, const TriangleGeometry& Geometry, const char* Text) {
        RequireResult(Expected, Runtime.RegisterGeometry(Geometry.Desc, Handle), Text);
    };

    {
        TriangleGeometry G;
        G.Desc.pVertices = nullptr;
        Expect(RTXPTSceneResult::InvalidArgument, G, "geometry without vertices");
    }
    {
        TriangleGeometry G;
        G.Indices[2] = 3;
        Expect(RTXPTSceneResult::InvalidArgument, G, "index past the vertices");
    }
    {
        TriangleGeometry G;
        G.Primitive.IndexCount = 6;
        Expect(RTXPTSceneResult::InvalidArgument, G, "primitive past the indices");
    }
    {
        TriangleGeometry G;
        G.Primitive.MaterialSlot = 1;
        Expect(RTXPTSceneResult::InvalidArgument, G, "primitive past the material slots");
    }
    {
        TriangleGeometry G;
        G.Vertices[0].normal.y = std::numeric_limits<float>::quiet_NaN();
        Expect(RTXPTSceneResult::InvalidArgument, G, "non-finite vertex");
    }
    {
        TriangleGeometry G;
        G.Desc.pSkin = G.Skin;
        Expect(RTXPTSceneResult::InvalidArgument, G, "skin data on rigid geometry");
    }
    {
        TriangleGeometry G{2};
        G.Desc.pSkin = nullptr;
        Expect(RTXPTSceneResult::InvalidArgument, G, "skinned geometry without skin data");
    }
    {
        TriangleGeometry G{2};
        G.Skin[1].joints = float4{2, 0, 0, 0};
        Expect(RTXPTSceneResult::InvalidArgument, G, "joint past the palette");
    }
    {
        TriangleGeometry G{2};
        G.Skin[1].joints  = float4{1, 7, 0, 0};
        G.Skin[1].weights = float4{1, 0, 0, 0};
        Expect(RTXPTSceneResult::Ok, G, "unweighted joints are not checked");
    }
    {
        TriangleGeometry G;
        G.Desc.VertexCount = (1u << 28) + 1;
        Expect(RTXPTSceneResult::LimitExceeded, G, "vertex limit");
    }
    {
        TriangleGeometry G;
        G.Primitive       = {0, 0, 0, 3, 0};
        G.Desc.pIndices   = nullptr;
        G.Desc.IndexCount = 0;
        Expect(RTXPTSceneResult::Ok, G, "non-indexed geometry");
    }

    Uint32 MeshCount = 0, SlotCount = 0, JointCount = 0;
    RequireResult(RTXPTSceneResult::Ok, Runtime.GetGeometryInfo(Handle, MeshCount, SlotCount, JointCount), "geometry info");
    Require(MeshCount == 1 && SlotCount == 1 && JointCount == 0, "geometry info values");
}

static void TestMaterialValidation()
{
    RTXPTSceneRuntime   Runtime;
    RTXPTMaterialHandle Handle;
    RTXPTMaterialDesc   Desc;
    Desc.Data.flags = kMaterialFlag_HasBaseColorTexture;
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.RegisterMaterial(Desc, Handle), "texture flag without a texture");
    Desc.Data.flags        = 0;
    Desc.EmissionMagnitude = -1.0f;
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.RegisterMaterial(Desc, Handle), "negative emission");
    Desc.EmissionMagnitude = 2.0f;
    ITextureView* pNull    = nullptr;
    Desc.ppTextures        = &pNull;
    Desc.TextureCount      = 1;
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.RegisterMaterial(Desc, Handle), "null texture view");
    Desc.TextureCount = 0;
    RequireResult(RTXPTSceneResult::Ok, Runtime.RegisterMaterial(Desc, Handle), "valid material");
    RequireResult(RTXPTSceneResult::Ok, Runtime.ReleaseMaterial(Handle), "release material");
    RequireResult(RTXPTSceneResult::InvalidHandle, Runtime.ReleaseMaterial(Handle), "release a released material");

    RTXPTMaterialHandle Reused;
    RequireResult(RTXPTSceneResult::Ok, Runtime.RegisterMaterial(Desc, Reused), "register in a released slot");
    Require(Reused.Slot == Handle.Slot && Reused.Generation != Handle.Generation, "a reused slot gets a new generation");
}

static void TestTransformValidation()
{
    Require(IsValidRTXPTInstanceTransform(float4x4::Scale(0.0254f) * Translation(3)), "scaled translation is valid");
    float4x4 Projective = float4x4::Identity();
    Projective._14      = 1;
    Require(!IsValidRTXPTInstanceTransform(Projective), "projective transform");
    Require(!IsValidRTXPTInstanceTransform(float4x4::Scale(1, 0, 1)), "singular transform");
    float4x4 NaN = float4x4::Identity();
    NaN._41      = std::numeric_limits<float>::quiet_NaN();
    Require(!IsValidRTXPTInstanceTransform(NaN), "non-finite transform");
}

static void TestUpdateRules()
{
    RTXPTSceneRuntime         Runtime;
    const RTXPTGeometryHandle Geometry = RegisterTriangle(Runtime);
    const RTXPTMaterialHandle Material = RegisterMaterial(Runtime);

    RTXPTSceneSnapshotPtr Initial = Runtime.GetSnapshot();
    Require(Initial && Initial->SnapshotId == 0 && Initial->Instances.empty() && Initial->Lights, "initial snapshot is empty");

    RTXPTInstanceHandle Instance;
    RequireResult(RTXPTSceneResult::InvalidState, Runtime.CreateInstance(MakeInstance(Geometry, Material), Instance), "create outside an update");
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin");
    RequireResult(RTXPTSceneResult::InvalidState, Runtime.BeginUpdate(), "nested begin");
    RTXPTGeometryHandle Late;
    RequireResult(RTXPTSceneResult::Ok, Runtime.RegisterGeometry(TriangleGeometry{}.Desc, Late), "register during an update");

    RTXPTInstanceDesc Bad = MakeInstance(Geometry, Material);
    Bad.MeshIndex         = 1;
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.CreateInstance(Bad, Instance), "mesh index past the meshes");
    Bad               = MakeInstance(Geometry, Material);
    Bad.MaterialCount = 0;
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.CreateInstance(Bad, Instance), "incomplete material binding");
    const RTXPTMaterialHandle Stale = {};
    Bad                             = MakeInstance(Geometry, Stale);
    RequireResult(RTXPTSceneResult::InvalidHandle, Runtime.CreateInstance(Bad, Instance), "invalid material handle");
    float4x4 Pose       = float4x4::Identity();
    Bad                 = MakeInstance(Geometry, Material);
    Bad.pPose           = &Pose;
    Bad.PoseMatrixCount = 1;
    RequireResult(RTXPTSceneResult::InvalidPose, Runtime.CreateInstance(Bad, Instance), "pose on rigid geometry");
    Require(!Instance.IsValid(), "failed creation returns no handle");

    RTXPTSceneSnapshotPtr Snapshot;
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Snapshot), "commit of failed setters");
    Require(Snapshot->SnapshotId == 1 && Snapshot->ContentRevision == Initial->ContentRevision && Snapshot->Instances.empty(),
            "failed setters publish nothing");
    Require(!Runtime.IsUpdating(), "commit closes the update");
    RequireResult(RTXPTSceneResult::InvalidState, Runtime.CommitUpdate(Snapshot), "commit outside an update");
}

static void TestInstanceLifecycle()
{
    RTXPTSceneRuntime         Runtime;
    const RTXPTGeometryHandle Geometry = RegisterTriangle(Runtime);
    const RTXPTMaterialHandle Material = RegisterMaterial(Runtime);

    RTXPTInstanceHandle   A, B;
    RTXPTSceneSnapshotPtr First;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 1");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 1), A), "create A");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 2), B), "create B");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(First), "commit 1");
    Require(First->Instances.size() == 2 && First->Instances[0]->Handle == A && First->Instances[1]->Handle == B, "instances in slot order");
    Require(First->Instances[0]->Geometry == First->Instances[1]->Geometry, "instances share the geometry asset");

    // Equal values change nothing.
    RTXPTSceneSnapshotPtr Second;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 2");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceTransform(A, Translation(1)), "same transform");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceMaterial(A, 0, Material), "same material");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceEnabled(A, true), "same enabling");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Second), "commit 2");
    Require(Second->SnapshotId == First->SnapshotId + 1 && Second->ContentRevision == First->ContentRevision, "equal values keep the content revision");
    Require(Second->Instances[0] == First->Instances[0], "equal values keep the instance state");

    // A moved instance changes the content; the published snapshot does not change.
    RTXPTSceneSnapshotPtr Third;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 3");
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.SetInstanceTransform(A, float4x4::Scale(0, 1, 1)), "singular transform");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceTransform(A, Translation(5)), "move A");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Third), "commit 3");
    Require(Third->ContentRevision != Second->ContentRevision, "a move changes the content revision");
    Require(Third->Instances[0]->TransformRevision != Second->Instances[0]->TransformRevision, "a move changes the transform revision");
    Require(Third->Instances[0]->MaterialRevision == Second->Instances[0]->MaterialRevision, "a move keeps the material revision");
    Require(Second->Instances[0]->Transform._41 == 1.0f && Third->Instances[0]->Transform._41 == 5.0f, "snapshots are immutable");
    Require(Third->Instances[1] == Second->Instances[1], "other instances keep their state");

    // Changes of a disabled instance do not change the content; enabling does.
    RTXPTSceneSnapshotPtr Fourth, Fifth;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 4");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceEnabled(B, false), "disable B");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Fourth), "commit 4");
    Require(Fourth->ContentRevision != Third->ContentRevision && Fourth->Instances.size() == 2, "a disabled instance stays in the snapshot");
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 5");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceTransform(B, Translation(7)), "move disabled B");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Fifth), "commit 5");
    Require(Fifth->ContentRevision == Fourth->ContentRevision, "moving a disabled instance keeps the content revision");

    // A removed instance leaves the next snapshot; its handle becomes stale and its slot gets a new generation.
    RTXPTSceneSnapshotPtr Sixth;
    RTXPTInstanceHandle   C;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 6");
    RequireResult(RTXPTSceneResult::Ok, Runtime.RemoveInstance(A), "remove A");
    RequireResult(RTXPTSceneResult::InvalidHandle, Runtime.RemoveInstance(A), "remove A twice");
    RequireResult(RTXPTSceneResult::InvalidHandle, Runtime.SetInstanceTransform(A, Translation(1)), "move removed A");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 3), C), "create C");
    Require(C.Slot == A.Slot && C.Generation != A.Generation, "C reuses the slot of A with a new generation");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Sixth), "commit 6");
    Require(Sixth->Instances.size() == 2 && Sixth->Instances[0]->Handle == C && Fifth->Instances[0]->Handle == A, "removal reaches only the next snapshot");

    // A cancelled update publishes nothing, and its handles never match a later occupant.
    RTXPTInstanceHandle D, E;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 7");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 4), D), "create D");
    RequireResult(RTXPTSceneResult::Ok, Runtime.RemoveInstance(C), "remove C");
    Runtime.CancelUpdate();
    Require(Runtime.GetSnapshot() == Sixth, "cancel keeps the snapshot");
    RTXPTSceneSnapshotPtr Seventh;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 8");
    RequireResult(RTXPTSceneResult::InvalidHandle, Runtime.SetInstanceEnabled(D, false), "handle of a cancelled update");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 4), E), "create E");
    Require(E.Slot == D.Slot && E.Generation != D.Generation, "E gets a generation the cancelled D never had");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceEnabled(C, false), "C survived the cancelled removal");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Seventh), "commit 8");
    Require(Seventh->Instances.size() == 3, "three live instances");
}

static void TestPosesMaterialsAndLights()
{
    RTXPTSceneRuntime         Runtime;
    const RTXPTGeometryHandle Skinned  = RegisterTriangle(Runtime, 2);
    const RTXPTMaterialHandle Material = RegisterMaterial(Runtime, 1.0f);
    const RTXPTMaterialHandle Rough    = RegisterMaterial(Runtime, 0.5f);

    float4x4            Pose[2] = {float4x4::Identity(), Translation(1)};
    RTXPTInstanceHandle Instance;
    RTXPTInstanceDesc   Desc = MakeInstance(Skinned, Material);
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin");
    RequireResult(RTXPTSceneResult::InvalidPose, Runtime.CreateInstance(Desc, Instance), "skinned instance without a pose");
    Desc.pPose           = Pose;
    Desc.PoseMatrixCount = 2;
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(Desc, Instance), "skinned instance");
    RTXPTSceneSnapshotPtr First;
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(First), "commit 1");

    RTXPTSceneSnapshotPtr Second;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 2");
    RequireResult(RTXPTSceneResult::InvalidPose, Runtime.SetInstancePose(Instance, Pose, 1), "pose of the wrong size");
    float4x4 Broken[2] = {Pose[0], Pose[1]};
    Broken[1]._22      = std::numeric_limits<float>::infinity();
    RequireResult(RTXPTSceneResult::InvalidPose, Runtime.SetInstancePose(Instance, Broken, 2), "non-finite pose");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstancePose(Instance, Pose, 2), "same pose");
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.SetInstanceMaterial(Instance, 1, Rough), "material slot past the binding");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceMaterial(Instance, 0, Material), "same material");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Second), "commit 2");
    Require(Second->ContentRevision == First->ContentRevision && Second->Instances[0] == First->Instances[0], "equal pose and material change nothing");

    RTXPTSceneSnapshotPtr Third;
    Pose[1] = Translation(2);
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 3");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstancePose(Instance, Pose, 2), "new pose");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceMaterial(Instance, 0, Rough), "new material");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Third), "commit 3");
    const RTXPTSceneInstanceState& Old = *First->Instances[0];
    const RTXPTSceneInstanceState& New = *Third->Instances[0];
    Require(Third->ContentRevision != First->ContentRevision, "pose and material change the content");
    Require(New.PoseRevision != Old.PoseRevision && New.MaterialRevision != Old.MaterialRevision && New.TransformRevision == Old.TransformRevision,
            "pose and material revisions");
    Require((*Old.Pose)[1]._41 == 1.0f && (*New.Pose)[1]._41 == 2.0f, "the old pose is unchanged");
    Require(Old.Materials[0]->Data.roughnessFactor == 1.0f && New.Materials[0]->Data.roughnessFactor == 0.5f, "per-instance material binding");

    // Lights: equal lists change nothing.
    RTXPTSceneLight Light;
    Light.Light.Base.Center = float3{1, 2, 3};
    Light.Key               = {1, 2, 3, 4};
    RTXPTSceneSnapshotPtr Fourth, Fifth;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 4");
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.SetLights(nullptr, 1), "null lights");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetLights(&Light, 1), "set a light");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Fourth), "commit 4");
    Require(Fourth->LightRevision != Third->LightRevision && Fourth->ContentRevision != Third->ContentRevision && Fourth->Lights->size() == 1, "a new light");
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 5");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetLights(&Light, 1), "same light");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Fifth), "commit 5");
    Require(Fifth->LightRevision == Fourth->LightRevision && Fifth->ContentRevision == Fourth->ContentRevision, "an equal light changes nothing");

    // Released assets stay alive while instances use them, but cannot be bound again.
    RequireResult(RTXPTSceneResult::Ok, Runtime.ReleaseMaterial(Rough), "release the bound material");
    RequireResult(RTXPTSceneResult::Ok, Runtime.ReleaseGeometry(Skinned), "release the bound geometry");
    Require(Fifth->Instances[0]->Materials[0]->Data.roughnessFactor == 0.5f && Fifth->Instances[0]->Geometry->JointCount == 2, "instances keep released assets");
    RTXPTInstanceHandle Other;
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "begin 6");
    RequireResult(RTXPTSceneResult::InvalidHandle, Runtime.SetInstanceMaterial(Instance, 0, Rough), "bind a released material");
    RequireResult(RTXPTSceneResult::InvalidHandle, Runtime.CreateInstance(Desc, Other), "instance of a released geometry");
    Runtime.CancelUpdate();
}

static void TestPreparedRigidHistory()
{
    RTXPTSceneRuntime   Runtime;
    RTXPTSceneHistory   History;
    const auto          Geometry = RegisterTriangle(Runtime);
    const auto          Material = RegisterMaterial(Runtime);
    RTXPTInstanceHandle A, B;
    auto                Commit = [&]() {
        RTXPTSceneSnapshotPtr Snapshot;
        RequireResult(RTXPTSceneResult::Ok, Runtime.CommitUpdate(Snapshot), "history commit");
        return Snapshot;
    };
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "history begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 1), A), "history A");
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 2), B), "history B");
    const auto First = Commit();
    History.BeginPrepare();
    Require(!History.FindPreviousRigid(*First->Instances[0]), "new instance has no previous surface");
    const auto Token1 = History.Publish(First);
    Require(Token1.IsValid() && 1 == Token1.Execution && 0 == Token1.PreviousExecution, "first prepared token");

    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "move begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceTransform(B, Translation(5)), "move B");
    RequireResult(RTXPTSceneResult::Ok, Runtime.RemoveInstance(A), "remove A before B");
    const auto Moved = Commit();
    History.BeginPrepare();
    Require(!History.GetToken().IsValid(), "preparing/failed frame invalidates published token");
    const auto* Previous = History.FindPreviousRigid(*Moved->Instances[0]);
    Require(Previous && 2.0f == Previous->Transform._41, "compact row zero still corresponds to B, not A");
    // A failed preparation publishes nothing. Retrying still resolves the last successful surface.
    History.BeginPrepare();
    Require(Previous == History.FindPreviousRigid(*Moved->Instances[0]), "failed prepare does not advance previous surface");
    const auto Token2 = History.Publish(Moved);
    Require(Token1.IsPreviousOf(Token2) && Token1.Epoch == Token2.Epoch, "successful prepare advances one execution");
    History.BeginPrepare();
    const auto Repeated = History.Publish(Moved);
    Require(Token2.IsPreviousOf(Repeated) && Repeated.Execution != Moved->SnapshotId, "same snapshot has distinct prepared executions");

    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "reset begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceContinuity(B, RTXPTInstanceContinuity::Reset), "explicit reset");
    const auto Reset = Commit();
    History.BeginPrepare();
    Require(!History.FindPreviousRigid(*Reset->Instances[0]), "teleport rejects old surface");
    History.Publish(Reset);
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "resume begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceContinuity(B, RTXPTInstanceContinuity::Continuous), "resume continuity");
    const auto Resumed = Commit();
    Require(History.FindPreviousRigid(*Resumed->Instances[0]), "reset frame can become the next continuous baseline");
    History.Publish(Resumed);
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "untracked begin");
    constexpr auto InvalidContinuity = static_cast<RTXPTInstanceContinuity>(static_cast<Uint32>(RTXPTInstanceContinuity::Reset) + 1);
    RequireResult(RTXPTSceneResult::InvalidArgument, Runtime.SetInstanceContinuity(B, InvalidContinuity), "invalid continuity");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceContinuity(B, RTXPTInstanceContinuity::Untracked), "untracked identity");
    const auto Untracked = Commit();
    Require(!History.FindPreviousRigid(*Untracked->Instances[0]), "untracked surface rejects history");
    History.Publish(Untracked);
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "tracked again begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceContinuity(B, RTXPTInstanceContinuity::Continuous), "tracked again");
    const auto TrackedAgain = Commit();
    Require(!History.FindPreviousRigid(*TrackedAgain->Instances[0]), "untracked past cannot become valid retrospectively");
    History.Publish(TrackedAgain);

    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "cancel reset begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceContinuity(B, RTXPTInstanceContinuity::Reset), "cancelled reset");
    Runtime.CancelUpdate();
    Require(History.FindPreviousRigid(*Runtime.GetSnapshot()->Instances[0]), "cancelled CPU update cannot invalidate the published correspondence");
    auto OpaqueChanged         = *TrackedAgain->Instances[0];
    OpaqueChanged.Materials[0] = std::make_shared<RTXPTMaterialAsset>(*OpaqueChanged.Materials[0]);
    Require(History.FindPreviousRigid(OpaqueChanged), "opaque material identity alone does not change geometric correspondence");
    auto MaskedMaterial = std::make_shared<RTXPTMaterialAsset>(*OpaqueChanged.Materials[0]);
    MaskedMaterial->Data.flags |= kMaterialFlag_AlphaTested;
    OpaqueChanged.Materials[0] = MaskedMaterial;
    Require(!History.FindPreviousRigid(OpaqueChanged), "changed coverage/failed masked binding rejects surface history");

    auto SkinnedGeometry        = std::make_shared<RTXPTGeometryAsset>(*TrackedAgain->Instances[0]->Geometry);
    SkinnedGeometry->JointCount = 1;
    auto SkinnedState           = std::make_shared<RTXPTSceneInstanceState>(*TrackedAgain->Instances[0]);
    SkinnedState->Geometry      = SkinnedGeometry;
    auto SkinnedSnapshot        = std::make_shared<RTXPTSceneSnapshot>();
    SkinnedSnapshot->Instances.push_back(SkinnedState);
    RTXPTSceneHistory SkinnedHistory;
    SkinnedHistory.Publish(SkinnedSnapshot);
    Require(!SkinnedHistory.FindPreviousRigid(*SkinnedState), "skinned geometry cannot pretend current deformation is previous data");

    auto OtherGeometry     = *TrackedAgain->Instances[0];
    OtherGeometry.Geometry = std::make_shared<RTXPTGeometryAsset>(*OtherGeometry.Geometry);
    Require(!History.FindPreviousRigid(OtherGeometry), "different immutable geometry/layout version rejects correspondence");
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "disable begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceEnabled(B, false), "disable");
    History.Publish(Commit());
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "restore begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.SetInstanceEnabled(B, true), "restore");
    const auto Restored = Commit();
    Require(!History.FindPreviousRigid(*Restored->Instances[0]), "absence then recovery has no previous surface");
    History.Publish(Restored);
    RequireResult(RTXPTSceneResult::Ok, Runtime.BeginUpdate(), "reuse begin");
    RequireResult(RTXPTSceneResult::Ok, Runtime.RemoveInstance(B), "remove B");
    RTXPTInstanceHandle Reused;
    RequireResult(RTXPTSceneResult::Ok, Runtime.CreateInstance(MakeInstance(Geometry, Material, 5), Reused), "reuse slot");
    const auto ReusedSnapshot = Commit();
    Require(B.Slot == Reused.Slot && B.Generation != Reused.Generation, "test exercises slot reuse");
    Require(!History.FindPreviousRigid(*ReusedSnapshot->Instances[0]), "generation prevents inherited history");
    const auto OldEpoch = History.GetToken().Epoch;
    History.Reset();
    const auto NewToken = History.Publish(ReusedSnapshot);
    Require(NewToken.Epoch != OldEpoch && !Repeated.IsPreviousOf(NewToken), "residency reset creates a different epoch");
}

int main()
{
    try
    {
        TestGeometryValidation();
        TestMaterialValidation();
        TestTransformValidation();
        TestUpdateRules();
        TestInstanceLifecycle();
        TestPosesMaterialsAndLights();
        TestPreparedRigidHistory();
    }
    catch (const std::exception& Error)
    {
        std::cerr << "FAIL " << Error.what() << '\n';
        return 1;
    }
    std::cout << "PASS RTXPTSceneRuntime\n";
    return 0;
}
