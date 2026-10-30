"""Compile the production allocation/build methods against deterministic device failures.

No GPU is needed. Method bodies are taken from RTXPTSceneFrame.cpp, so a regression
in the actual control flow fails these tests rather than a duplicate implementation.
Run with Python and clang++ on PATH. GPU correctness is covered separately.
"""
from pathlib import Path
import subprocess
import tempfile


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


MOCKS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include <cstdio>
using Uint32 = uint32_t;
using Uint64 = uint64_t;
constexpr int BIND_RAY_TRACING=1, BUFFER_MODE_UNDEFINED=0, USAGE_IMMUTABLE=1;
constexpr int VT_FLOAT32=1, RAYTRACING_GEOMETRY_FLAG_OPAQUE=1;
constexpr int RESOURCE_STATE_TRANSITION_MODE_TRANSITION=1, RAYTRACING_BUILD_AS_PREFER_FAST_TRACE=1;
constexpr int HIT_GROUP_BINDING_MODE_PER_GEOMETRY=1, TLAS_INSTANCE_DATA_SIZE=64;
constexpr int kRTXPTImmediateContextMask=1, kMinTLASInstances=16, kHitGroupStride=1;
struct float3 { float x,y,z; };
struct float4x4 {};
template<class T> struct RefCntAutoPtr {
    std::shared_ptr<T> ptr;
    struct Proxy {
        RefCntAutoPtr& owner; T* raw=nullptr;
        operator T**() { return &raw; }
        ~Proxy() { owner.ptr.reset(raw); }
    };
    Proxy operator&() { return {*this}; }
    operator bool() const { return !!ptr; }
    operator T*() const { return ptr.get(); }
    T* operator->() const { return ptr.get(); }
    T* RawPtr() const { return ptr.get(); }
    void Release() { ptr.reset(); }
};
struct BufferDesc { Uint64 Size=0; int Usage=0; };
BufferDesc MakeBufferDesc(const char*, Uint64 size, int, int, int) { return {size}; }
struct BufferData { const void* Data; Uint64 Size; };
struct IBuffer { BufferDesc desc; const auto& GetDesc() const { return desc; } };
struct ScratchSizes { Uint64 Build=256, Update=128; };
struct BLASTriangleDesc { const char* GeometryName; Uint32 MaxVertexCount,VertexValueType,VertexComponentCount,MaxPrimitiveCount; };
struct BottomLevelASDesc { int ImmediateContextMask; const char* Name; BLASTriangleDesc* pTriangles; Uint32 TriangleCount; };
struct IBottomLevelAS { bool built=false; ScratchSizes GetScratchBufferSizes() { return {}; } };
struct TopLevelASDesc { int ImmediateContextMask; const char* Name; Uint32 MaxInstanceCount; int Flags; };
struct ITopLevelAS { TopLevelASDesc desc; const auto& GetDesc() { return desc; } ScratchSizes GetScratchBufferSizes() { return {}; } };
struct BLASBuildTriangleData { const char* GeometryName; IBuffer* pVertexBuffer; Uint32 VertexStride,VertexCount,VertexValueType,VertexComponentCount,PrimitiveCount; int Flags; };
struct BuildBLASAttribs {
    IBottomLevelAS* pBLAS; int BLASTransitionMode, GeometryTransitionMode;
    const BLASBuildTriangleData* pTriangleData; Uint32 TriangleDataCount;
    IBuffer* pScratchBuffer; int ScratchBufferTransitionMode; bool Update;
};
struct TLASBuildInstanceData { IBottomLevelAS* pBLAS=nullptr; };
struct BuildTLASAttribs {
    ITopLevelAS* pTLAS; int TLASTransitionMode, BLASTransitionMode;
    const TLASBuildInstanceData* pInstances; Uint32 InstanceCount; IBuffer* pInstanceBuffer;
    int InstanceBufferTransitionMode, HitGroupStride, BindingMode;
    IBuffer* pScratchBuffer; int ScratchBufferTransitionMode; bool Update;
};
struct MockDevice {
    int calls=0, fail=0;
    bool Fails() { return ++calls==fail; }
    void CreateBuffer(BufferDesc desc, const BufferData*, IBuffer** out) { *out=Fails()?nullptr:new IBuffer{desc}; }
    void CreateBLAS(BottomLevelASDesc, IBottomLevelAS** out) { *out=Fails()?nullptr:new IBottomLevelAS; }
    void CreateTLAS(TopLevelASDesc desc, ITopLevelAS** out) { *out=Fails()?nullptr:new ITopLevelAS{desc}; }
};
struct IDeviceContext {
    int blasBuilds=0, tlasBuilds=0;
    void BuildBLAS(const BuildBLASAttribs& a) { assert(a.pBLAS && a.pScratchBuffer); a.pBLAS->built=true; ++blasBuilds; }
    void BuildTLAS(const BuildTLASAttribs& a) { assert(a.pTLAS && a.pInstanceBuffer && a.pScratchBuffer); ++tlasBuilds; }
};
Uint32 NextCapacity(Uint32 old, Uint32 count, Uint32 minimum) { return std::max({old*2,count,minimum}); }
enum class RTXPTErrorCode { Ok, SkinningPassFailed, JointPaletteFailed, SkinningBindingsFailed, SkinningDispatchFailed, BLASBuildFailed };
#define LOG_ERROR_MESSAGE(...) ((void)0)
struct RTXPTGeometryAsset { const char* Name="skin"; };
struct GeometryResidency { Uint32 VertexBase=0,SkinBase=0,NormalSkinBase=0; };
struct SkinnedInstance { Uint32 OutputBase=0,VertexCount=3; Uint64 PoseRevision=0; RefCntAutoPtr<IBottomLevelAS> BLAS; std::vector<Uint64> AnyHitMask; };
struct State { std::shared_ptr<RTXPTGeometryAsset> Geometry=std::make_shared<RTXPTGeometryAsset>(); std::shared_ptr<std::vector<float4x4>> Pose=std::make_shared<std::vector<float4x4>>(1); Uint64 PoseRevision=1; Uint32 MeshIndex=0; };
struct SkinJob { bool Reskin=true,Rebuild=false; State* pState; SkinnedInstance* pInstance; GeometryResidency* pGeometry; Uint32 TLASIndex=0; };
struct RTXPTSkinningBindings { IBuffer *pSourceVertices,*pSkinData,*pJointMatrices,*pSkinnedVertices; };
struct RTXPTSkinningDispatch { Uint32 SourceVertexBase,DestVertexBase,JointBase,VertexCount,SkinBase,NormalSkinBase; };
struct BufferHolder { IBuffer* Buffer=nullptr; bool Upload(MockDevice*, IDeviceContext*, const std::vector<float4x4>&, bool&) { return true; } };
struct MockSkinning { bool IsReady() { return true; } bool Initialize(MockDevice*, void*, void*, void*) { return true; } bool Bind(RTXPTSkinningBindings) { return true; } bool Dispatch(IDeviceContext*, RTXPTSkinningDispatch) { return true; } };
struct Fixture {
    MockDevice* Device;
    RefCntAutoPtr<IBuffer> BLASScratch, PlaceholderVertices, TLASInstanceBuffer, TLASScratch;
    RefCntAutoPtr<IBottomLevelAS> PlaceholderBLAS;
    RefCntAutoPtr<ITopLevelAS> TLAS;
    struct { int TLASCreateCount=0,TLASBuildCount=0,SkinningDispatchCount=0; } Stats;
    MockSkinning Skinning;
    BufferHolder JointMatrices,Vertices,SkinData,SkinnedVertices;
    struct { void *pEngineFactory=nullptr,*pStateCache=nullptr,*pPSOCache=nullptr; } CreateInfo;
    bool failSkinBuild=true;
    bool BuildMeshBLAS(IDeviceContext*, const RTXPTGeometryAsset&, const GeometryResidency&, Uint32, const std::vector<Uint64>&, IBuffer*, Uint32, RefCntAutoPtr<IBottomLevelAS>& blas) {
        if (!blas) blas.ptr=std::make_shared<IBottomLevelAS>();
        if (failSkinBuild) return false;
        blas->built=true; return true;
    }
'''

MAIN = r'''
};
int main() {
    for (int fail=1; fail<=3; ++fail) {
        MockDevice device; device.fail=fail; IDeviceContext context; Fixture f{&device}; bool changed=false;
        std::vector<TLASBuildInstanceData> instances(1);
        assert(!f.BuildTLAS(&context,instances,changed));
        assert(!f.TLAS && !changed && context.tlasBuilds==0);
        device.fail=0;
        assert(f.BuildTLAS(&context,instances,changed));
        assert(changed && context.tlasBuilds==1);
        auto old=f.TLAS.RawPtr(); instances.resize(100); device.fail=device.calls+2;
        assert(!f.BuildTLAS(&context,instances,changed));
        assert(f.TLAS.RawPtr()==old);
        device.fail=0; assert(f.BuildTLAS(&context,instances,changed));
    }
    for (int fail=1; fail<=3; ++fail) {
        MockDevice device; device.fail=fail; IDeviceContext context; Fixture f{&device};
        assert(!f.BuildPlaceholderBLAS(&context));
        assert(!f.PlaceholderBLAS && context.blasBuilds==0);
        device.fail=0; assert(f.BuildPlaceholderBLAS(&context));
        assert(f.PlaceholderBLAS->built && context.blasBuilds==1);
        assert(f.BuildPlaceholderBLAS(&context) && context.blasBuilds==1);
    }
    MockDevice device; IDeviceContext context; Fixture f{&device}; State state; SkinnedInstance instance; GeometryResidency geometry;
    std::vector<SkinJob> jobs{{true,false,&state,&instance,&geometry,0}};
    std::vector<TLASBuildInstanceData> instances(1); bool changed=false;
    assert(f.PrepareSkinnedInstances(&context,jobs,instances,changed)==RTXPTErrorCode::BLASBuildFailed);
    assert(instance.PoseRevision==0);
    jobs[0].Reskin=instance.PoseRevision!=state.PoseRevision; f.failSkinBuild=false;
    assert(f.PrepareSkinnedInstances(&context,jobs,instances,changed)==RTXPTErrorCode::Ok);
    assert(instance.PoseRevision==state.PoseRevision && instance.BLAS->built);
    std::puts("TLAS/placeholder allocation failures and identical-pose retry passed");
}
'''


def main():
    source = (Path(__file__).resolve().parent.parent / "src/RTXPTSceneFrame.cpp").read_text(encoding="utf-8")
    signatures = ["bool BuildBLAS(", "bool BuildPlaceholderBLAS(", "bool BuildTLAS(", "RTXPTErrorCode PrepareSkinnedInstances("]
    program = MOCKS + "\n".join(method(source, signature) for signature in signatures) + MAIN
    with tempfile.TemporaryDirectory(prefix="rtxpt-retry-") as directory:
        path = Path(directory)
        (path / "test.cpp").write_text(program, encoding="utf-8")
        executable = path / "test.exe"
        subprocess.run(["clang++", "-std=c++17", str(path / "test.cpp"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
