// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// The external textures of .material.json files are read through RTXPTFileSource: a scene that exists only in a
// file source loads with the same materials as the same files on disk, which the default source reads.
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "../src/RTXPTMaterials.hpp"
#include "../src/RTXPTScene.hpp"
#include "../src/RTXPTSceneGraph.hpp"
#include "../src/RTXPTSceneJson.hpp"
using namespace Diligent;

static unsigned                Errors = 0;
static void DILIGENT_CALL_TYPE Message(DEBUG_MESSAGE_SEVERITY Severity, const Char* Text, const Char*, const Char* File, int)
{
    // The corrupt texture is expected to make the texture loader report errors; any other error fails the test.
    const bool TextureLoader = File != nullptr && std::strstr(File, "TextureLoader") != nullptr;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR && !TextureLoader) ++Errors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING) std::cerr << Text << '\n';
}
static void Require(bool OK, const std::string& Text)
{
    if (!OK) throw std::runtime_error{Text};
}

using Bytes = std::vector<unsigned char>;
static Bytes ToBytes(const std::string& Text) { return Bytes{Text.begin(), Text.end()}; }

// A 1 x 1 RGBA PNG.
static const Bytes PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0x0F, 0x04, 0x00,
    0x09, 0xFB, 0x03, 0xFD, 0xFB, 0x5E, 0x6B, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
constexpr Uint32 PNGWidth = 1;
constexpr Uint32 DDSWidth = 2;

// A DDS_HEADER_DXT10 R8G8B8A8_UNORM texture, DDSWidth x 1, so its binding is told apart from the PNG's.
static Bytes MakeDDS()
{
    Bytes Data;
    auto  Put = [&](Uint32 Value) {
        for (int i = 0; i < 4; ++i) Data.push_back(static_cast<unsigned char>(Value >> (8 * i)));
    };
    Put(0x20534444); // "DDS "
    Put(124);        // dwSize
    Put(0x100F);     // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    Put(1);          // dwHeight
    Put(DDSWidth);   // dwWidth
    Put(DDSWidth * 4);
    Put(0); // dwDepth
    Put(1); // dwMipMapCount
    for (int i = 0; i < 11; ++i) Put(0);
    Put(32);         // ddspf.dwSize
    Put(0x4);        // DDPF_FOURCC
    Put(0x30315844); // "DX10"
    for (int i = 0; i < 5; ++i) Put(0);
    Put(0x1000); // DDSCAPS_TEXTURE
    for (int i = 0; i < 4; ++i) Put(0);
    Put(28); // DXGI_FORMAT_R8G8B8A8_UNORM
    Put(3);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    Put(0);
    Put(1); // arraySize
    Put(0);
    for (Uint32 i = 0; i < DDSWidth * 4; ++i) Data.push_back(0xFF);
    return Data;
}

// The triangle and its bin: three positions and three 16-bit indices padded to 4 bytes.
static Bytes MakeTriangleBin()
{
    const float  Positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const Uint16 Indices[]   = {0, 1, 2, 0};
    Bytes        Data(sizeof(Positions) + sizeof(Indices));
    std::memcpy(Data.data(), Positions, sizeof(Positions));
    std::memcpy(Data.data() + sizeof(Positions), Indices, sizeof(Indices));
    return Data;
}

// Material order in the glTF; each has a .material.json case of its own.
enum MaterialCase : Uint32
{
    MaterialNoJson = 0, // no .material.json: the glTF material as it is
    MaterialDDS,        // BaseTexture textures/dds.png, beside which textures/dds.dds exists
    MaterialPNG,        // BaseTexture textures/png.png without a .dds
    MaterialMissing,    // BaseTexture textures/missing.png is absent: the glTF binding is kept
    MaterialCorrupt,    // BaseTexture textures/corrupt.png is not an image, and the glTF material has no texture
    MaterialCount
};
constexpr float  BaseColor[]   = {0.5f, 0.25f, 0.125f, 1.0f};
constexpr float  JsonRoughness = 0.25f;
constexpr float  JsonMetallic  = 0.75f;
constexpr char   SceneFile[]   = "scene.json";
constexpr char   ModelFile[]   = "triangle.gltf";
constexpr char   DDSPath[]     = "textures/dds.dds";
constexpr char   DDSPNGPath[]  = "textures/dds.png";
constexpr char   PNGPath[]     = "textures/png.png";
constexpr char   PNGDDSPath[]  = "textures/png.dds";
constexpr char   MissingPath[] = "textures/missing.png";
constexpr char   NoJsonFile[]  = "Materials/nojson.material.json";
constexpr Uint32 GLTFTextures  = 1;

static std::map<std::string, Bytes> MakeSceneFiles()
{
    const std::string Model = R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
    "nodes": [{"name": "triangle", "mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
    "materials": [
        {"name": "nojson", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.25, 0.125, 1], "baseColorTexture": {"index": 0}}},
        {"name": "dds"}, {"name": "png"},
        {"name": "missing", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}},
        {"name": "corrupt"}],
    "textures": [{"source": 0}], "images": [{"uri": "white.png"}],
    "buffers": [{"uri": "triangle.bin", "byteLength": 44}],
    "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
                    {"buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963}],
    "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                  {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]})";
    return {
        {SceneFile, ToBytes(R"({"models": ["triangle.gltf"], "graph": [{"name": "triangle", "model": 0}]})")},
        {ModelFile, ToBytes(Model)},
        {"triangle.bin", MakeTriangleBin()},
        {"white.png", PNG},
        {"Materials/dds.material.json", ToBytes(R"({"BaseTexture": {"path": "textures/dds.png", "sRGB": true}, "Roughness": 0.25, "Metalness": 0.75})")},
        {"Materials/png.material.json", ToBytes(R"({"BaseTexture": {"path": "textures/png.png", "sRGB": true}})")},
        {"Materials/missing.material.json", ToBytes(R"({"BaseTexture": {"path": "textures/missing.png"}})")},
        {"Materials/corrupt.material.json", ToBytes(R"({"BaseTexture": {"path": "textures/corrupt.png"}})")},
        {DDSPNGPath, PNG},
        {DDSPath, MakeDDS()},
        {PNGPath, PNG},
        {"textures/corrupt.png", ToBytes("not an image")},
    };
}

static std::string Normalize(std::string Path)
{
    std::replace(Path.begin(), Path.end(), '\\', '/');
    return Path;
}

// Files under a root that does not exist on disk, so a read that bypasses the source cannot succeed. Records the
// paths it is asked about, relative to the root.
struct MemorySource
{
    std::string                  Root;
    std::map<std::string, Bytes> Files;
    std::vector<std::string>     Probed;
    std::vector<std::string>     Read;

    std::string Relative(const char* Path) const
    {
        const std::string Full   = Normalize(Path);
        const std::string Prefix = Normalize(Root) + '/';
        return Full.compare(0, Prefix.size(), Prefix) == 0 ? Full.substr(Prefix.size()) : Full;
    }
    bool WasProbed(const std::string& Path) const { return std::find(Probed.begin(), Probed.end(), Path) != Probed.end(); }
    bool WasRead(const std::string& Path) const { return std::find(Read.begin(), Read.end(), Path) != Read.end(); }

    RTXPTFileSource Source()
    {
        RTXPTFileSource Source;
        Source.FileExists = [this](const char* Path) {
            Probed.push_back(Relative(Path));
            return Files.count(Probed.back()) != 0;
        };
        Source.ReadWholeFile = [this](const char* Path, Bytes& Data, std::string& Error) {
            Read.push_back(Relative(Path));
            const auto It = Files.find(Read.back());
            if (It == Files.end())
            {
                Error = Read.back() + " is not in the source";
                return false;
            }
            Data = It->second;
            return true;
        };
        return Source;
    }
};

static void TestReadBytes(const std::filesystem::path& Directory)
{
    // Bytes that text handling would change: NUL, CR LF, and a byte above 0x7F.
    const Bytes Data = {'a', 0x00, '\r', '\n', 0xFF, 'z'};

    std::filesystem::create_directories(Directory);
    const std::string Path = (Directory / "bytes.bin").string();
    {
        std::ofstream File{Path, std::ios::binary};
        File.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
    }

    const RTXPTFileSource Disk;
    Bytes                 Out;
    std::string           Error;
    Require(Disk.ReadBytes(Path, Out, Error) && Out == Data, "ReadBytes reads the disk exactly without callbacks");
    std::string Text;
    Require(Disk.Read(Path, Text, Error) && Bytes(Text.begin(), Text.end()) == Data, "Read keeps the bytes as text");
    Require(!Disk.ReadBytes((Directory / "absent.bin").string(), Out, Error) && Error.find("absent.bin") != std::string::npos,
            "ReadBytes reports a missing disk file");

    RTXPTFileSource Callback;
    Callback.ReadWholeFile = [&](const char*, Bytes& Result, std::string&) {
        Result = Data;
        return true;
    };
    Out.clear();
    Require(Callback.ReadBytes(Path + ".ignored", Out, Error) && Out == Data, "ReadBytes returns the callback's bytes");

    Callback.ReadWholeFile = [](const char*, Bytes&, std::string&) {
        return false;
    };
    Error.clear();
    Require(!Callback.ReadBytes("refused.bin", Out, Error) && Error.find("refused.bin") != std::string::npos,
            "ReadBytes names the file when the callback fails without an error");

    std::filesystem::remove_all(Directory);
}

struct UploadResult
{
    std::vector<MaterialPTData> Materials;
    std::vector<Uint32>         TextureWidths;
};

static UploadResult LoadAndUpload(IRenderDevice* Device, IDeviceContext* Context, const std::string& Root, const RTXPTSceneLoadOptions& Options)
{
    RTXPTScene Scene;
    Require(Scene.LoadScene(Device, Context, Root, SceneFile, Options), "scene load");
    RTXPTMaterials Materials;
    Require(Materials.Upload(Device, Scene.GetSceneGraphData(), Scene.GetAssetsRoot(), Options.FileSource), "material upload");
    Require(Materials.GetStats().MaterialCount == MaterialCount, "one material record per glTF material");

    UploadResult Result;
    for (Uint32 i = 0; i < Materials.GetTextureCount(); ++i)
    {
        auto* View = static_cast<ITextureView*>(Materials.GetTextureBindings()[i]);
        Result.TextureWidths.push_back(View->GetTexture()->GetDesc().Width);
    }

    IBuffer*   Source = Materials.GetMaterialBuffer();
    BufferDesc Desc;
    Desc.Name           = "material readback";
    Desc.Size           = Source->GetDesc().Size;
    Desc.Usage          = USAGE_STAGING;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<IBuffer> Readback;
    Device->CreateBuffer(Desc, nullptr, &Readback);
    Require(Readback != nullptr, "readback buffer");
    Context->CopyBuffer(Source, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->WaitForIdle();
    void* Data = nullptr;
    Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
    Require(Data != nullptr, "map material readback");
    Result.Materials.resize(static_cast<size_t>(Desc.Size / sizeof(MaterialPTData)));
    std::memcpy(Result.Materials.data(), Data, Result.Materials.size() * sizeof(MaterialPTData));
    Context->UnmapBuffer(Readback, MAP_READ);
    return Result;
}

static bool HasBaseColorTexture(const MaterialPTData& Data) { return (Data.flags & kMaterialFlag_HasBaseColorTexture) != 0u; }

static void TestMaterials(IRenderDevice* Device, IDeviceContext* Context, const std::filesystem::path& Temp)
{
    MemorySource Memory;
    Memory.Root  = (Temp / "RTXPTMaterialFileSourceTest-memory").string();
    Memory.Files = MakeSceneFiles();
    Require(!std::filesystem::exists(Memory.Root), "the memory root is not on disk");

    RTXPTSceneLoadOptions Options;
    Options.FileSource        = Memory.Source();
    Options.RayTracing        = false;
    const UploadResult Result = LoadAndUpload(Device, Context, Memory.Root, Options);
    const auto&        M      = Result.Materials;

    // The glTF texture, then the external textures in material order: dds.dds and png.png.
    Require(Result.TextureWidths == std::vector<Uint32>{PNGWidth, DDSWidth, PNGWidth}, "bindless table: glTF texture, dds.dds, png.png");

    Require(Memory.WasProbed(NoJsonFile), "the optional material file is looked for");
    Require(HasBaseColorTexture(M[MaterialNoJson]) && M[MaterialNoJson].baseColorTextureIndex == 0, "without a material file the glTF texture is used");
    Require(std::memcmp(&M[MaterialNoJson].baseColorFactor, BaseColor, sizeof(BaseColor)) == 0, "without a material file the glTF factors are used");

    Require(Memory.WasRead(DDSPath) && !Memory.WasRead(DDSPNGPath), "the .dds beside the authored .png is read instead of it");
    Require(HasBaseColorTexture(M[MaterialDDS]) && M[MaterialDDS].baseColorTextureIndex == GLTFTextures, "the .dds is bound");
    Require(M[MaterialDDS].roughnessFactor == JsonRoughness && M[MaterialDDS].metallicFactor == JsonMetallic, "the material file overrides the factors");

    Require(Memory.WasProbed(PNGDDSPath) && Memory.WasRead(PNGPath), "the authored .png is read when the source has no .dds");
    Require(HasBaseColorTexture(M[MaterialPNG]) && M[MaterialPNG].baseColorTextureIndex == GLTFTextures + 1, "the .png is bound");

    Require(Memory.WasRead(MissingPath), "the missing texture is asked for");
    Require(HasBaseColorTexture(M[MaterialMissing]) && M[MaterialMissing].baseColorTextureIndex == 0, "a texture that cannot be read keeps the glTF binding");

    Require(!HasBaseColorTexture(M[MaterialCorrupt]) && M[MaterialCorrupt].baseColorTextureIndex == 0, "a texture that cannot be decoded leaves the slot to the factors");

    // The same files on disk, read by the default source, give the same materials.
    const std::filesystem::path DiskRoot = Temp / "RTXPTMaterialFileSourceTest-disk";
    std::filesystem::remove_all(DiskRoot);
    for (const auto& File : Memory.Files)
    {
        const std::filesystem::path Path = DiskRoot / File.first;
        std::filesystem::create_directories(Path.parent_path());
        std::ofstream Out{Path, std::ios::binary};
        Out.write(reinterpret_cast<const char*>(File.second.data()), static_cast<std::streamsize>(File.second.size()));
    }
    RTXPTSceneLoadOptions DiskOptions;
    DiskOptions.RayTracing  = false;
    const UploadResult Disk = LoadAndUpload(Device, Context, DiskRoot.string(), DiskOptions);
    std::filesystem::remove_all(DiskRoot);

    Require(Disk.TextureWidths == Result.TextureWidths, "the disk gives the same bindless table");
    Require(Disk.Materials.size() == M.size() && std::memcmp(Disk.Materials.data(), M.data(), M.size() * sizeof(MaterialPTData)) == 0,
            "the disk gives the same material records");
}

int main(int argc, char** argv)
try
{
    Require(argc == 3, "MaterialFileSourceTest <D3D12|Vk> <engine DLL>");
    SetDebugMessageCallback(Message);
    const std::filesystem::path Temp = std::filesystem::temp_directory_path();
    TestReadBytes(Temp / "RTXPTMaterialFileSourceTest-bytes");

    std::string DLL = argv[2];
    SetDllDirectoryA(DLL.substr(0, DLL.find_last_of("/\\")).c_str());
    auto Lib = LoadLibraryA(argv[2]);
    Require(Lib != nullptr, "engine load");
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    if (std::string{argv[1]} == "Vk")
    {
        auto Get = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Lib, "GetEngineFactoryVk"));
        Require(Get != nullptr, "Vk factory");
        auto* F = Get();
        F->SetMessageCallback(Message);
        EngineVkCreateInfo CI;
        CI.EnableValidation = true;
        F->CreateDeviceAndContextsVk(CI, &Device, &Context);
    }
    else
    {
        auto Get = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Lib, "GetEngineFactoryD3D12"));
        Require(Get != nullptr, "D3D12 factory");
        auto* F = Get();
        F->SetMessageCallback(Message);
        Require(F->LoadD3D12(), "load D3D12");
        EngineD3D12CreateInfo CI;
        CI.EnableValidation = true;
        F->CreateDeviceAndContextsD3D12(CI, &Device, &Context);
    }
    Require(Device && Context, "device creation");

    TestMaterials(Device, Context, Temp);
    Require(Errors == 0, "errors were reported");
    std::cout << "PASS: ReadBytes, and material textures read through the file source match the disk\n";
    return 0;
}
catch (const std::exception& E)
{
    std::cerr << "FAIL: " << E.what() << '\n';
    return 1;
}
