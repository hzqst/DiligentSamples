// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
// Executes production inline visibility and both production any-hit entry points.
#include <Windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "GraphicsTypesX.hpp"
#include "ShaderMacroHelper.hpp"
#include "../src/RTXPTMaterials.hpp"
#include "../src/RTXPTAccelerationStructures.hpp"

using namespace Diligent;
namespace
{
std::atomic<unsigned>   Errors{0};
void DILIGENT_CALL_TYPE Message(DEBUG_MESSAGE_SEVERITY Severity, const Char* Text, const Char*, const Char*, int)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR || std::strstr(Text, "Undefined-Value") != nullptr) ++Errors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING) std::cerr << Text << '\n';
}
void Require(bool Value, const char* Text)
{
    if (!Value) throw std::runtime_error{Text};
}
Uint64 LocalVideoMemoryUsage(const GraphicsAdapterInfo& RenderAdapter)
{
    // DXGI reports this process's local-segment usage, including driver allocation
    // granularity. Load the system DLL explicitly; no new linked dependency.
    static HMODULE DXGI = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Require(DXGI != nullptr, "DXGI telemetry unavailable");
    auto CreateFactory = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(DXGI, "CreateDXGIFactory1"));
    Require(CreateFactory != nullptr, "DXGI factory unavailable");
    Microsoft::WRL::ComPtr<IDXGIFactory1> Factory;
    Require(SUCCEEDED(CreateFactory(IID_PPV_ARGS(&Factory))), "DXGI telemetry factory failed");
    for (UINT Index = 0;; ++Index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
        if (Factory->EnumAdapters1(Index, &Adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 Desc{};
        Adapter->GetDesc1(&Desc);
        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (Desc.VendorId != RenderAdapter.VendorId || Desc.DeviceId != RenderAdapter.DeviceId) continue;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> Adapter3;
        if (FAILED(Adapter.As(&Adapter3))) continue;
        DXGI_QUERY_VIDEO_MEMORY_INFO Info{};
        if (SUCCEEDED(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
            return Info.CurrentUsage;
    }
    throw std::runtime_error{"Local video-memory telemetry unavailable"};
}
RefCntAutoPtr<IBuffer> Buffer(IRenderDevice* Device, Uint64 Size, Diligent::BIND_FLAGS Bind, const void* Data = nullptr, Uint32 Stride = 0)
{
    BufferDesc Desc;
    Desc.Name              = "Visibility test buffer";
    Desc.Size              = Size;
    Desc.BindFlags         = Bind;
    Desc.Mode              = Stride ? BUFFER_MODE_STRUCTURED : BUFFER_MODE_UNDEFINED;
    Desc.ElementByteStride = Stride;
    BufferData             Init{Data, Size};
    RefCntAutoPtr<IBuffer> Result;
    Device->CreateBuffer(Desc, Data ? &Init : nullptr, &Result);
    Require(Result != nullptr, "Cannot create buffer");
    return Result;
}
std::vector<Uint32> Read(IDeviceContext* Context, IRenderDevice* Device, IBuffer* Source)
{
    BufferDesc Desc;
    Desc.Name           = "Visibility readback";
    Desc.Size           = Source->GetDesc().Size;
    Desc.Usage          = USAGE_STAGING;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<IBuffer> Staging;
    Device->CreateBuffer(Desc, nullptr, &Staging);
    Context->CopyBuffer(Source, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Staging, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->Flush();
    Context->WaitForIdle();
    void* Data = nullptr;
    Context->MapBuffer(Staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
    Require(Data != nullptr, "Readback unavailable");
    std::vector<Uint32> Result(static_cast<size_t>(Desc.Size / 4));
    std::memcpy(Result.data(), Data, static_cast<size_t>(Desc.Size));
    Context->UnmapBuffer(Staging, MAP_READ);
    return Result;
}
struct Vertex
{
    float3 Position;
    float3 Normal;
    float2 UV;
};
static_assert(sizeof(Vertex) == 32, "Production packed vertex stride");
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc >= 3 && Argc <= 5, "Usage: VisibilityRayTest <engine DLL> <shader root> [uint16|uint32] [hitobject]");
    Require(Argc < 4 || std::strcmp(Argv[3], "uint16") == 0 || std::strcmp(Argv[3], "uint32") == 0, "Invalid index type");
    Require(Argc < 5 || std::strcmp(Argv[4], "hitobject") == 0, "Invalid scatter test mode");
    const bool       Use16Bit     = Argc >= 4 && std::strcmp(Argv[3], "uint16") == 0;
    const bool       UseHitObject = Argc == 5 && std::strcmp(Argv[4], "hitobject") == 0;
    const VALUE_TYPE IndexType = Use16Bit ? VT_UINT16 : VT_UINT32;
    const Uint32     IndexSize = Use16Bit ? sizeof(Uint16) : sizeof(Uint32);
    Require(128.0f / 255.0f == RTXPTQuantizeAlphaCutoff(0.5f), "Upstream midpoint cutoff");
    Require(0.0f == RTXPTQuantizeAlphaCutoff(-1) && 1.0f == RTXPTQuantizeAlphaCutoff(2), "Cutoff saturation");
    GLTF::Material         Classification;
    RTXPTMaterialExtension Extension;
    Classification.Attribs.AlphaMode = GLTF::Material::ALPHA_MODE_MASK;
    Require(RTXPTMaterialIsAlphaTested(Classification, nullptr, true), "MASK enables alpha test");
    Require(!RTXPTMaterialIsAlphaTested(Classification, nullptr, false), "Missing texture disables alpha test");
    Extension.Loaded = true;
    Require(!RTXPTMaterialIsAlphaTested(Classification, &Extension, true), "Authored extension overrides glTF MASK");
    Classification.Attribs.AlphaMode = GLTF::Material::ALPHA_MODE_BLEND;
    Require(!RTXPTMaterialNeedsAnyHit(Classification, nullptr, true), "BLEND alone is opaque to traversal");
    Extension.EnableAlphaTesting = true;
    Require(RTXPTMaterialNeedsAnyHit(Classification, &Extension, true), "Explicit alpha-test override");
    Extension.EnableAlphaTesting = false;
    Extension.ExcludeFromNEE     = true;
    Require(RTXPTMaterialNeedsAnyHit(Classification, &Extension, false), "ExcludeFromNEE needs non-opaque geometry without textures");
    HMODULE Library = LoadLibraryA(Argv[1]);
    Require(Library != nullptr, "Cannot load engine");
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    IEngineFactory*               Factory = nullptr;
    auto                          D3D12   = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Library, "GetEngineFactoryD3D12"));
    if (D3D12)
    {
        Factory = D3D12();
        Factory->SetMessageCallback(Message);
        EngineD3D12CreateInfo CI;
        CI.EnableValidation    = true;
        CI.Features.RayTracing = DEVICE_FEATURE_STATE_ENABLED;
        CI.Features.DurationQueries = DEVICE_FEATURE_STATE_ENABLED;
        D3D12()->CreateDeviceAndContextsD3D12(CI, &Device, &Context);
    }
    else
    {
        auto Vulkan = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Library, "GetEngineFactoryVk"));
        Require(Vulkan != nullptr, "Missing factory");
        Factory = Vulkan();
        Factory->SetMessageCallback(Message);
        EngineVkCreateInfo CI;
        CI.EnableValidation    = true;
        CI.Features.RayTracing = DEVICE_FEATURE_STATE_ENABLED;
        CI.Features.DurationQueries = DEVICE_FEATURE_STATE_ENABLED;
        Vulkan()->CreateDeviceAndContextsVk(CI, &Device, &Context);
    }
    Require(Device && Context, "Cannot create device");
    Require(!UseHitObject || (D3D12 && Device->GetDeviceInfo().MaxShaderVersion.HLSL >= ShaderVersion{6, 9}), "HitObject test requires D3D12 SM 6.9");
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    std::string                                    Search = std::string{Argv[2]} + ";" + Argv[2] + "/PathTracer";
    Factory->CreateDefaultShaderSourceStreamFactory(Search.c_str(), &Source);

    // Four geometries, two triangles each; custom IDs deliberately differ from instance indices.
    // Nonzero vertex/index offsets, indexed and non-indexed geometry, and skinned UVs exercise the bridge.
    const char*            Names[] = {"Opaque", "Indexed mask", "Nonindexed mask", "Excluded or skinned"};
    std::array<Vertex, 27> Vertices{};
    for (Uint32 Triangle = 0; Triangle < 8; ++Triangle)
    {
        const float X = 4.0f * Triangle;
        const float U = Triangle / 2 == 1 ? (Triangle % 2 ? 0.125f : 0.375f) :
                                            Triangle / 2 == 2 ? (Triangle % 2 ? 0.875f : 0.625f) : 0.125f;
        Vertices[3 + Triangle * 3 + 0] = {{X - 1, -1, 1}, {}, {U, 0.5f}};
        Vertices[3 + Triangle * 3 + 1] = {{X, 1, 1}, {}, {U, 0.5f}};
        Vertices[3 + Triangle * 3 + 2] = {{X + 1, -1, 1}, {}, {U, 0.5f}};
    }
    auto Skinned = Vertices;
    for (Uint32 V = 21; V < 27; ++V) Skinned[V].UV.x = 0.875f;
    auto                   VB     = Buffer(Device, sizeof(Vertices), BIND_RAY_TRACING | BIND_SHADER_RESOURCE, Vertices.data(), sizeof(Vertex));
    auto                   SkinVB = Buffer(Device, sizeof(Skinned), BIND_SHADER_RESOURCE, Skinned.data(), sizeof(Vertex));
    std::array<Uint32, 27> Indices{};
    for (Uint32 G = 0; G < 4; ++G)
        for (Uint32 I = 0; I < 6; ++I) Indices[3 + G * 6 + I] = (I / 3) * 3 + 2 - I % 3;
    std::array<Uint16, 27> Indices16{};
    for (size_t I = 0; I < Indices.size(); ++I) Indices16[I] = static_cast<Uint16>(Indices[I]);
    BufferDesc IndexDesc;
    IndexDesc.Name              = "Visibility typed index buffer";
    IndexDesc.Size              = Indices.size() * IndexSize;
    IndexDesc.BindFlags         = BIND_SHADER_RESOURCE | BIND_RAY_TRACING;
    IndexDesc.Mode              = BUFFER_MODE_FORMATTED;
    IndexDesc.ElementByteStride = IndexSize;
    BufferData             IndexInit{Use16Bit ? static_cast<const void*>(Indices16.data()) : Indices.data(), IndexDesc.Size};
    RefCntAutoPtr<IBuffer> IB;
    Device->CreateBuffer(IndexDesc, &IndexInit, &IB);
    Require(IB != nullptr, "Index buffer creation failed");
    BufferViewDesc IndexViewDesc;
    IndexViewDesc.ViewType = BUFFER_VIEW_SHADER_RESOURCE;
    IndexViewDesc.Format   = {IndexType, 1, false};
    RefCntAutoPtr<IBufferView> IndexView;
    IB->CreateView(IndexViewDesc, &IndexView);

    std::array<BLASTriangleDesc, 4>      GeometryDescs{};
    std::array<BLASBuildTriangleData, 4> GeometryData{};
    for (Uint32 G = 0; G < 4; ++G)
    {
        auto& Desc                = GeometryDescs[G];
        Desc.GeometryName         = Names[G];
        Desc.MaxVertexCount       = 6;
        Desc.VertexValueType      = VT_FLOAT32;
        Desc.VertexComponentCount = 3;
        Desc.MaxPrimitiveCount    = 2;
        Desc.IndexType            = G == 2 ? VT_UNDEFINED : IndexType;
        auto& Data                = GeometryData[G];
        Data.GeometryName         = Names[G];
        Data.pVertexBuffer        = VB;
        Data.VertexOffset         = (3 + G * 6) * sizeof(Vertex);
        Data.VertexStride         = sizeof(Vertex);
        Data.VertexCount          = 6;
        Data.VertexValueType      = VT_FLOAT32;
        Data.VertexComponentCount = 3;
        Data.PrimitiveCount       = 2;
        if (G != 2)
        {
            Data.pIndexBuffer = IB;
            Data.IndexOffset  = (3 + G * 6) * IndexSize;
            Data.IndexType    = IndexType;
        }
        Data.Flags = G == 0 ? RAYTRACING_GEOMETRY_FLAG_OPAQUE : RAYTRACING_GEOMETRY_FLAG_NONE;
    }
    BottomLevelASDesc BLASDesc;
    BLASDesc.Name          = "Visibility test BLAS";
    BLASDesc.pTriangles    = GeometryDescs.data();
    BLASDesc.TriangleCount = 4;
    BLASDesc.Flags = RAYTRACING_BUILD_AS_ALLOW_UPDATE;
    RefCntAutoPtr<IBottomLevelAS> BLAS;
    Device->CreateBLAS(BLASDesc, &BLAS);
    Require(BLAS != nullptr, "BLAS creation failed");
    auto             BLASScratch = Buffer(Device, std::max(BLAS->GetScratchBufferSizes().Build, BLAS->GetScratchBufferSizes().Update), BIND_RAY_TRACING);
    BuildBLASAttribs BuildBLAS;
    BuildBLAS.pBLAS              = BLAS;
    BuildBLAS.pTriangleData      = GeometryData.data();
    BuildBLAS.TriangleDataCount  = 4;
    BuildBLAS.pScratchBuffer     = BLASScratch;
    BuildBLAS.BLASTransitionMode = BuildBLAS.GeometryTransitionMode = BuildBLAS.ScratchBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    Context->BuildBLAS(BuildBLAS);

    std::array<TLASBuildInstanceData, 3> Instances{};
    const char*                          InstanceNames[] = {"Front", "Other materials", "Background"};
    const Uint32                         CustomIDs[]     = {5, 17, 29};
    for (Uint32 I = 0; I < 3; ++I)
    {
        Instances[I].InstanceName = InstanceNames[I];
        Instances[I].pBLAS        = BLAS;
        Instances[I].CustomId     = CustomIDs[I];
        Instances[I].Mask         = 0xFF;
        Instances[I].Flags        = RAYTRACING_INSTANCE_TRIANGLE_FACING_CULL_DISABLE;
    }
    Instances[1].Transform.SetTranslation(0, 10, 0);
    Instances[2].Transform.SetTranslation(0, 0, 1);
    TopLevelASDesc TLASDesc;
    TLASDesc.Name             = "Visibility test TLAS";
    TLASDesc.MaxInstanceCount = 3;
    TLASDesc.Flags            = RAYTRACING_BUILD_AS_ALLOW_UPDATE;
    RefCntAutoPtr<ITopLevelAS> TLAS;
    Device->CreateTLAS(TLASDesc, &TLAS);
    Require(TLAS != nullptr, "TLAS creation failed");
    auto             TLASScratch    = Buffer(Device, std::max(TLAS->GetScratchBufferSizes().Build, TLAS->GetScratchBufferSizes().Update), BIND_RAY_TRACING);
    BufferDesc InstanceDesc;
    InstanceDesc.Name = "GPU TLAS output";
    InstanceDesc.Size = TLAS_INSTANCE_DATA_SIZE * 3;
    InstanceDesc.BindFlags = BIND_RAY_TRACING | BIND_UNORDERED_ACCESS;
    InstanceDesc.Mode = BUFFER_MODE_RAW;
    RefCntAutoPtr<IBuffer> InstanceBuffer;
    Device->CreateBuffer(InstanceDesc, nullptr, &InstanceBuffer);
    Require(InstanceBuffer != nullptr, "GPU TLAS output creation failed");
    BuildTLASAttribs BuildTLAS;
    BuildTLAS.pTLAS              = TLAS;
    BuildTLAS.pInstances         = Instances.data();
    BuildTLAS.InstanceCount      = 3;
    BuildTLAS.HitGroupStride     = 2;
    BuildTLAS.BindingMode        = HIT_GROUP_BINDING_MODE_PER_GEOMETRY;
    BuildTLAS.pScratchBuffer     = TLASScratch;
    BuildTLAS.pInstanceBuffer    = InstanceBuffer;
    BuildTLAS.TLASTransitionMode = BuildTLAS.BLASTransitionMode = BuildTLAS.ScratchBufferTransitionMode = BuildTLAS.InstanceBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    Context->BuildTLAS(BuildTLAS);

    std::array<SubInstanceData, 33> Subs{};
    std::array<MaterialPTData, 12>  Materials{};
    for (Uint32 I = 0; I < 3; ++I)
        for (Uint32 G = 0; G < 4; ++G)
        {
            auto& Sub      = Subs[CustomIDs[I] + G];
            Sub.MaterialID = I * 4 + G;
            Sub.Flags      = G == 2 ? 0 : kSubInstanceFlag_Indexed;
            if (I == 1 && G == 3) Sub.Flags |= kSubInstanceFlag_Skinned;
            Sub.IndexOffset  = 3 + G * 6;
            Sub.VertexOffset = 3 + G * 6;
            Sub.VertexCount = Sub.IndexCount = 6;
            auto& Mat                        = Materials[I * 4 + G];
            Mat.baseColorFactor              = {1, 1, 1, 0.1f};
            Mat.alphaCutoff                  = RTXPTQuantizeAlphaCutoff(0.5f);
            Mat.baseColorTextureIndex        = 0;
            Mat.flags                        = kMaterialFlag_HasBaseColorTexture;
            if (I < 2 && G > 0) Mat.flags |= kMaterialFlag_AlphaTested;
        }
    Materials[3].flags = kMaterialFlag_HasBaseColorTexture | kMaterialFlag_ExcludeFromNEE;
    Materials[5].flags &= ~kMaterialFlag_AlphaTested; // BLEND alone does not enable a hit filter.
    Materials[5].baseColorFactor.w = 0;
    auto         SubBuffer         = Buffer(Device, sizeof(Subs), BIND_SHADER_RESOURCE, Subs.data(), sizeof(SubInstanceData));
    auto         MaterialBuffer    = Buffer(Device, sizeof(Materials), BIND_SHADER_RESOURCE, Materials.data(), sizeof(MaterialPTData));
    const float4 Texels[]          = {{1, 1, 1, 0.4f}, {1, 1, 1, 0.6f}, {1, 1, 1, 0.5f}, {1, 1, 1, 0.7f}};
    TextureDesc  TexDesc;
    TexDesc.Name      = "Alpha test texture";
    TexDesc.Type      = RESOURCE_DIM_TEX_2D_ARRAY;
    TexDesc.Width     = 4;
    TexDesc.Height    = 1;
    TexDesc.ArraySize = 1;
    TexDesc.Format    = TEX_FORMAT_RGBA32_FLOAT;
    TexDesc.BindFlags = BIND_SHADER_RESOURCE;
    TextureSubResData       SubData{Texels, sizeof(Texels)};
    TextureData             TexData{&SubData, 1};
    RefCntAutoPtr<ITexture> Texture;
    Device->CreateTexture(TexDesc, &TexData, &Texture);
    Require(Texture != nullptr, "Texture creation failed");

    ShaderCreateInfo SCI;
    SCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    SCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    SCI.HLSLVersion                = UseHitObject ? ShaderVersion{6, 9} : ShaderVersion{6, 5};
    SCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    SCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    SCI.pShaderSourceStreamFactory = Source;
    const char* TestShader         = R"(
#include "PathTracer/PathTracerVisibility.hlsli"
#include "PathTracer/PathPayload.hlsli"
RWStructuredBuffer<uint4> u_Results;
RWStructuredBuffer<uint4> u_Hits;
#ifdef TEST_RAYGEN
[shader("raygeneration")] void RayGen()
{
    uint n = DispatchRaysIndex().x;
    uint row = n / 8;
    RayDesc ray;
    ray.Origin = float3((n % 8) * 4, row == 1 ? 10 : 0, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = row == 4 ? 1.1 : 0;
    ray.TMax = row == 2 ? 3 : row == 3 ? 0.5 : 1.5;
    if (row == 5) ray.Origin.y = 100;
    bool visible = Bridge::traceVisibilityRay(ray);
    PathPayload shadow = (PathPayload)0;
    TraceRay(t_SceneBVH, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 1, 2, 0, ray, shadow);
    PathPayload primary = (PathPayload)0;
    TraceRay(t_SceneBVH, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 2, 0, ray, primary);
    u_Results[n] = uint4(visible, shadow.packed[0].x, primary.packed[0].x, 0);
    RayQuery<RAY_FLAG_NONE> identity;
    identity.TraceRayInline(t_SceneBVH, RAY_FLAG_NONE, 0xff, ray);
    while (identity.Proceed())
        if (identity.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            Bridge::AlphaTestImpl(t_SubInstanceData[identity.CandidateInstanceID() + identity.CandidateGeometryIndex()],
                identity.CandidatePrimitiveIndex(), identity.CandidateTriangleBarycentrics()))
            identity.CommitNonOpaqueTriangleHit();
    u_Hits[n] = uint4(0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    if (identity.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        u_Hits[n] = uint4(identity.CommittedInstanceID(), identity.CommittedGeometryIndex(),
            t_SubInstanceData[identity.CommittedInstanceID() + identity.CommittedGeometryIndex()].MaterialID,
            asuint(identity.CommittedRayT()));
#if USE_DX_HIT_OBJECT_EXTENSION
    RayQuery<RAY_FLAG_NONE> query;
    Bridge::traceScatterRay(ray, query);
    uint scatterMiss = 1;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        dx::HitObject hit = Bridge::makeScatterHit(query);
        dx::MaybeReorderThread(hit, n & 1, 1);
        PathPayload scatter = (PathPayload)0;
        dx::HitObject::Invoke(hit, scatter);
        // The primary hit shader writes a distinct value; wrong SBT slots fail.
        scatterMiss = scatter.packed[0].x == 42 ? 0 : 2;
    }
    u_Results[n].w = scatterMiss;
#endif
}
#endif
#ifdef TEST_MISS
[shader("miss")] void Miss(inout PathPayload payload) { payload.packed[0].x = 1; }
#endif
#ifdef TEST_HIT
[shader("closesthit")] void Hit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attributes) { payload.packed[0].x = 42; }
#endif
#ifdef TEST_SHADOW_HIT
[shader("closesthit")] void ShadowHit(inout PathPayload payload, BuiltInTriangleIntersectionAttributes attributes) { payload.packed[0].x = 99; }
#endif
)";
    auto        Compile            = [&](SHADER_TYPE Stage, const char* Entry, const char* File = nullptr) {
        ShaderMacroHelper Macros;
        Macros.Add("ENABLE_MATERIAL_TEXTURES", 1);
        Macros.Add("MATERIAL_TEXTURE_COUNT", 1);
        Macros.Add("RTXPT_USE_INLINE_VISIBILITY", 1);
        Macros.Add("USE_DX_HIT_OBJECT_EXTENSION", UseHitObject ? 1 : 0);
        Macros.Add("RTXPT_INDEX_BUFFER_UINT16", Use16Bit ? 1 : 0);
        if (Stage == SHADER_TYPE_RAY_GEN) Macros.Add("TEST_RAYGEN", 1);
        if (Stage == SHADER_TYPE_RAY_MISS) Macros.Add("TEST_MISS", 1);
        if (Stage == SHADER_TYPE_RAY_CLOSEST_HIT) Macros.Add(std::strcmp(Entry, "ShadowHit") == 0 ? "TEST_SHADOW_HIT" : "TEST_HIT", 1);
        if (std::strcmp(Entry, "VisibilityMain") == 0) Macros.Add("RTXPT_VISIBILITY_ANY_HIT", 1);
        SCI.Macros     = Macros;
        SCI.Desc       = {Entry, Stage, false};
        SCI.EntryPoint = Entry;
        SCI.FilePath   = File;
        SCI.Source     = File ? nullptr : TestShader;
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(SCI, &Shader);
        Require(Shader != nullptr, "Shader compilation failed");
        return Shader;
    };
    auto                               RayGen        = Compile(SHADER_TYPE_RAY_GEN, "RayGen");
    auto                               Miss          = Compile(SHADER_TYPE_RAY_MISS, "Miss");
    auto                               Hit           = Compile(SHADER_TYPE_RAY_CLOSEST_HIT, "Hit");
    auto                               ShadowHit     = Compile(SHADER_TYPE_RAY_CLOSEST_HIT, "ShadowHit");
    auto                               PrimaryAnyHit = Compile(SHADER_TYPE_RAY_ANY_HIT, "main", "PathTracer/PathTracerAnyHit.rahit");
    auto                               ShadowAnyHit  = Compile(SHADER_TYPE_RAY_ANY_HIT, "VisibilityMain", "PathTracer/PathTracerAnyHit.rahit");
    RayTracingPipelineStateCreateInfoX PCI;
    PCI.PSODesc.Name                               = "Production visibility regression";
    PCI.PSODesc.PipelineType                       = PIPELINE_TYPE_RAY_TRACING;
    PCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    PCI.AddGeneralShader("RayGen", RayGen).AddGeneralShader("Miss", Miss);
    PCI.AddTriangleHitShader("Primary", Hit, PrimaryAnyHit).AddTriangleHitShader("Shadow", ShadowHit, ShadowAnyHit);
    PCI.RayTracingPipeline.MaxRecursionDepth = 1;
    PCI.MaxPayloadSize                       = 80;
    PCI.MaxAttributeSize                     = 8;
    SamplerDesc                Sampler{FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR,
                        TEXTURE_ADDRESS_WRAP, TEXTURE_ADDRESS_WRAP, TEXTURE_ADDRESS_WRAP};
    const ImmutableSamplerDesc Immutable{SHADER_TYPE_RAY_GEN | SHADER_TYPE_RAY_ANY_HIT, "s_MaterialSampler", Sampler};
    PCI.PSODesc.ResourceLayout.ImmutableSamplers    = &Immutable;
    PCI.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    RefCntAutoPtr<IPipelineState> PSO;
    Device->CreateRayTracingPipelineState(PCI, &PSO);
    Require(PSO != nullptr, "RT pipeline creation failed");
    RefCntAutoPtr<IShaderResourceBinding> SRB;
    PSO->CreateShaderResourceBinding(&SRB, true);
    auto Output = Buffer(Device, 48 * 4 * sizeof(Uint32), BIND_UNORDERED_ACCESS, nullptr, 16);
    auto Hits = Buffer(Device, 48 * 4 * sizeof(Uint32), BIND_UNORDERED_ACCESS, nullptr, 16);
    for (SHADER_TYPE Stage : {SHADER_TYPE_RAY_GEN, SHADER_TYPE_RAY_ANY_HIT})
    {
        auto Set = [&](const char* Name, IDeviceObject* Object) {
            if (auto* Variable = SRB->GetVariableByName(Stage, Name)) Variable->Set(Object);
        };
        Set("t_SceneBVH", TLAS);
        Set("t_SubInstanceData", SubBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        Set("t_PTMaterialData", MaterialBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        Set("t_VertexBuffer", VB->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        Set("t_SkinnedVertexBuffer", SkinVB->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        Set("t_IndexBuffer", IndexView);
        Set("t_BindlessTextures", Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Set("u_Results", Output->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Set("u_Hits", Hits->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    }
    ShaderBindingTableDesc SBTDesc;
    SBTDesc.Name = "Visibility test SBT";
    SBTDesc.pPSO = PSO;
    RefCntAutoPtr<IShaderBindingTable> SBT;
    Device->CreateSBT(SBTDesc, &SBT);
    SBT->BindRayGenShader("RayGen");
    SBT->BindMissShader("Miss", 0);
    SBT->BindHitGroupForTLAS(TLAS, 0, "Primary");
    SBT->BindHitGroupForTLAS(TLAS, 1, "Shadow");
    Context->UpdateSBT(SBT);
    Context->SetPipelineState(PSO);
    Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    TraceRaysAttribs Trace;
    Trace.pSBT       = SBT;
    Trace.DimensionX = 48;
    Context->TraceRays(Trace);
    const auto Result = Read(Context, Device, Output);
    // Explicit upstream expectations, independent of the shared production predicate.
    const Uint32 FrontShadow[] = {0, 0, 0, 1, 1, 0, 1, 1};
    const Uint32 OtherShadow[] = {0, 0, 0, 0, 1, 0, 0, 0};
    for (Uint32 N = 0; N < 48; ++N)
    {
        Uint32 Expected = N < 8 ? FrontShadow[N] : N < 16 ? OtherShadow[N - 8] : N < 24 ? 0 : 1;
        for (Uint32 Channel = 0; Channel < (UseHitObject ? 4u : 3u); ++Channel)
        {
            const Uint32 ChannelExpected = Channel >= 2 && (N == 6 || N == 7) ? 0 : Expected;
            if (ChannelExpected != Result[N * 4 + Channel])
            {
                std::cerr << "Ray " << N << " channel " << Channel << ": expected " << ChannelExpected << ", got " << Result[N * 4 + Channel] << '\n';
                throw std::runtime_error{"Visibility mismatch"};
            }
        }
    }
    // Differential coverage uses the production GPU descriptor shader. CPU and GPU
    // builds deliberately receive different CPU transforms/masks, detecting any
    // accidental CPU overwrite of the prebuilt buffer in either backend.
    auto TraceAndRead = [&]() {
        // Topology changes invalidate the old table, including removed records.
        SBT.Release();
        Device->CreateSBT(SBTDesc, &SBT);
        SBT->BindRayGenShader("RayGen");
        SBT->BindMissShader("Miss", 0);
        SBT->BindHitGroupForTLAS(TLAS, 0, "Primary");
        SBT->BindHitGroupForTLAS(TLAS, 1, "Shadow");
        Context->UpdateSBT(SBT);
        Context->SetPipelineState(PSO);
        Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->TraceRays(Trace);
        auto Values = Read(Context, Device, Output);
        auto HitValues = Read(Context, Device, Hits);
        Values.insert(Values.end(), HitValues.begin(), HitValues.end());
        return Values;
    };
    std::array<float4x4, 33> Transforms;
    std::array<GPUTLAS::InstanceData, 3> GPUInstances;
    auto TransformBuffer = Buffer(Device, sizeof(Transforms), BIND_SHADER_RESOURCE, nullptr, sizeof(float4x4));
    RefCntAutoPtr<IQuery> Timing;
    Device->CreateQuery(QueryDesc{QUERY_TYPE_DURATION}, &Timing);
    Require(Timing != nullptr, "TLAS duration query unavailable");
    auto ReportTiming = [&](const char* Path, Uint32 Scenario, double SubmitUs) {
        QueryDataDuration Data;
        Require(Timing->GetData(&Data, sizeof(Data)) && Data.Frequency != 0, "TLAS duration unavailable");
        std::cout << "TLAS timing," << Path << ',' << Scenario << ",cpu_us=" << SubmitUs
                  << ",gpu_us=" << double(Data.Duration) * 1e6 / double(Data.Frequency) << '\n';
    };
    for (Uint32 Scenario = 0; Scenario < 7; ++Scenario)
    {
        if (Scenario == 6)
        {
            for (auto& V : Vertices) V.Position.z += 0.25f;
            Context->UpdateBuffer(VB, 0, sizeof(Vertices), Vertices.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            BuildBLAS.Update = true;
            Context->BuildBLAS(BuildBLAS);
        }
        // Removal, addition/reload, transform, mask, and fully invisible scene.
        const Uint32 Count = Scenario == 4 ? 1 : Scenario == 5 ? 0 : 3;
        for (Uint32 I = 0; I < 3; ++I)
        {
            const float X = Scenario == 1 ? 4.0f : 0.0f;
            const float Y = I == 1 ? 10.0f : 0.0f;
            const float Z = I == 2 ? 1.0f : 0.0f;
            Transforms[CustomIDs[I]] = float4x4::Translation(X, Y, Z);
            if (Scenario == 1)
                Transforms[CustomIDs[I]] = float4x4::Scale(0.75f, 1.25f, 1.0f) * float4x4::RotationZ(0.1f) * Transforms[CustomIDs[I]];
            Instances[I].Transform.SetRotation(&Transforms[CustomIDs[I]]._11, 4);
            Instances[I].Transform.SetTranslation(X, Y, Z);
            Instances[I].Mask = Scenario == 3 || (Scenario == 2 && I == 0) ? 0 : 0xff;
            GPUInstances[I] = {CustomIDs[I], CustomIDs[I], Instances[I].Mask};
        }
        BuildTLAS.Update = false;
        BuildTLAS.InstanceCount = Count;
        BuildTLAS.InstanceBufferIsPrebuilt = false;
        Context->BuildTLAS(BuildTLAS);
        // Compare identical refit modes; topology/bootstrap builds are not timed.
        BuildTLAS.Update = true;
        Context->BeginQuery(Timing);
        auto SubmitStart = std::chrono::steady_clock::now();
        Context->BuildTLAS(BuildTLAS);
        double SubmitUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - SubmitStart).count();
        Context->EndQuery(Timing);
        const auto Expected = TraceAndRead();
        ReportTiming("cpu", Scenario, SubmitUs);
        const auto ExpectedInstances = Read(Context, Device, InstanceBuffer);
        const Uint64 CPUVideoMemory = LocalVideoMemoryUsage(Device->GetAdapterInfo());
        if (Count != 0)
        {
            GPUTLAS GPU;
            Require(GPU.Initialize(Device, Context, Factory, InstanceBuffer, GPUInstances.data(), Count, Argv[2]), "GPU TLAS initialization failed");
            Context->UpdateBuffer(TransformBuffer, 0, sizeof(Transforms), Transforms.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Context->BeginQuery(Timing);
            SubmitStart = std::chrono::steady_clock::now();
            Require(GPU.Prepare(Context, TransformBuffer, InstanceBuffer), "GPU TLAS preparation failed");
            for (auto& Instance : Instances)
            {
                Instance.Transform.SetTranslation(100, 100, 100);
                Instance.Mask = 0;
            }
            BuildTLAS.InstanceBufferIsPrebuilt = true;
            BuildTLAS.Update = true;
            Context->BuildTLAS(BuildTLAS);
            SubmitUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - SubmitStart).count();
            Context->EndQuery(Timing);
            Require(Expected == TraceAndRead(), "CPU/GPU TLAS ray, material or visibility mismatch");
            ReportTiming("gpu", Scenario, SubmitUs);
            Require(ExpectedInstances == Read(Context, Device, InstanceBuffer), "Native GPU descriptor bytes differ (transform, flags, mask, BLAS or SBT)");
            std::cout << "TLAS additional buffer bytes," << GPU.GetMemorySize() << '\n';
            std::cout << "TLAS process local VRAM bytes,cpu=" << CPUVideoMemory << ",gpu=" << LocalVideoMemoryUsage(Device->GetAdapterInfo()) << '\n';
            // Exercise a full GPU rebuild too, not only refits.
            Require(GPU.Prepare(Context, TransformBuffer, InstanceBuffer), "GPU TLAS rebuild preparation failed");
            BuildTLAS.Update = false;
            Context->BuildTLAS(BuildTLAS);
            Require(Expected == TraceAndRead(), "GPU TLAS rebuild mismatch");
            if (Scenario == 2)
            {
                // Change visibility without replacing the template or instance table.
                GPUInstances[0].Mask = 0xff;
                for (Uint32 I = 0; I < Count; ++I)
                {
                    const auto& Matrix = Transforms[CustomIDs[I]];
                    Instances[I].Transform.SetRotation(&Matrix._11, 4);
                    Instances[I].Transform.SetTranslation(Matrix._41, Matrix._42, Matrix._43);
                    Instances[I].Mask = static_cast<Uint8>(GPUInstances[I].Mask);
                }
                BuildTLAS.Update = true;
                BuildTLAS.InstanceBufferIsPrebuilt = false;
                Context->BuildTLAS(BuildTLAS);
                const auto UpdatedExpected = TraceAndRead();
                Require(Expected != UpdatedExpected, "Mask update must change the test rays");
                GPU.UpdateInstanceData(Context, GPUInstances.data());
                Require(GPU.Prepare(Context, TransformBuffer, InstanceBuffer), "GPU visibility update preparation failed");
                BuildTLAS.InstanceBufferIsPrebuilt = true;
                Context->BuildTLAS(BuildTLAS);
                Require(UpdatedExpected == TraceAndRead(), "GPU visibility update mismatch");
            }
        }
        else
        {
            BuildTLAS.InstanceBufferIsPrebuilt = true;
            Context->BuildTLAS(BuildTLAS);
            Require(Expected == TraceAndRead(), "Empty GPU TLAS mismatch");
        }
        Context->WaitForIdle();
    }
    Require(Errors == 0, "GPU validation errors");
    std::cout << "PASS: CPU/GPU TLAS differential, 7 scenarios, hit IDs / material / distance / shadows\n";
    std::cout << "PASS: material classification, 48 rays, inline / TraceRay visibility / primary any-hit"
              << (UseHitObject ? " / HitObject + reorder, " : ", ") << (Use16Bit ? "uint16" : "uint32") << " indices\n";
    return 0;
}
catch (const std::exception& Error)
{
    std::cerr << Error.what() << '\n';
    return 1;
}
