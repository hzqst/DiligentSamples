// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Included by MaterialFileSourceTest.cpp to reuse its device and in-memory glTF fixture.

constexpr Uint32 ColorSamplesPerMaterial = 6;

static std::vector<float4> SampleColorMaterials(IRenderDevice* Device, IDeviceContext* Context, IEngineFactory* Factory, const RTXPTMaterials& Materials, bool Triangle)
{
    const std::string                              ShaderRoot = (std::filesystem::absolute(__FILE__).parent_path().parent_path() / "shaders").string();
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Streams;
    Factory->CreateDefaultShaderSourceStreamFactory(ShaderRoot.c_str(), &Streams);
    const std::string Prefix = "#define ENABLE_MATERIAL_TEXTURES 1\n#define NON_PATH_TRACING_PASS 1\n#define MATERIAL_TEXTURE_COUNT " +
        std::to_string(Materials.GetTextureCount()) + "\n#define COLOR_SAMPLES_PER_MATERIAL " + std::to_string(ColorSamplesPerMaterial) + "\n";
    const std::string Source = Prefix + (Triangle ? R"(
#define main UnusedTriangleBuild
#include "PathTracer/EmissiveTriangleBuild.hlsl"
#undef main
RWStructuredBuffer<float4> Output;
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID)
{
    MaterialPTData m = t_PTMaterialData[id.x];
    Output[id.x] = float4(computeTriangleRadiance(m, float2(0,0), float2(4,0), float2(0,4)), 1);
}
)" :
                                                    R"(
#include "PathTracer/BSDFs/MaterialBridge.hlsli"
RWStructuredBuffer<float4> Output;
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID)
{
    MaterialPTData m = Bridge::getMaterial(id.x);
    const uint offset = id.x * COLOR_SAMPLES_PER_MATERIAL;
    Output[offset + 0] = Bridge::getBaseColor(m, float2(0.5,0.5));
    Output[offset + 1] = float4(Bridge::getEmission(m, float2(0.5,0.5)), 1);
    Output[offset + 2] = t_BindlessTextures[NonUniformResourceIndex(m.metallicRoughnessTextureIndex)].SampleLevel(s_MaterialSampler, float3(0.5,0.5,0), 0);
    Output[offset + 3] = t_BindlessTextures[NonUniformResourceIndex(m.baseColorTextureIndex)].SampleLevel(s_MaterialSampler, float3(0.5,0.5,0), 1);
    Output[offset + 4] = t_BindlessTextures[NonUniformResourceIndex(m.metallicRoughnessTextureIndex)].SampleLevel(s_MaterialSampler, float3(0.5,0.5,0), 1);
    Output[offset + 5] = float4(Bridge::getMetallicRoughness(m, float2(0.5,0.5)), 0, 1);
}
)");
    ShaderCreateInfo  ShaderCI;
    ShaderCI.Desc.Name                  = "material color space regression";
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.HLSLVersion                = {6, 5};
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.Source                     = Source.c_str();
    ShaderCI.pShaderSourceStreamFactory = Streams;
    RefCntAutoPtr<IShader> Shader;
    Device->CreateShader(ShaderCI, &Shader);
    Require(Shader != nullptr, "color sampling shader");

    ComputePipelineStateCreateInfo CI;
    CI.PSODesc.Name                               = "material color sampling";
    CI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    CI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    SamplerDesc Sampler;
    Sampler.MinFilter = Sampler.MagFilter = Sampler.MipFilter = FILTER_TYPE_LINEAR;
    const ImmutableSamplerDesc Immutable{SHADER_TYPE_COMPUTE, "s_MaterialSampler", Sampler};
    CI.PSODesc.ResourceLayout.ImmutableSamplers    = &Immutable;
    CI.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    CI.pCS                                         = Shader;
    RefCntAutoPtr<IPipelineState> PSO;
    Device->CreateComputePipelineState(CI, &PSO);
    Require(PSO != nullptr, "color sampling pipeline");

    BufferDesc Desc;
    Desc.Name              = "color samples";
    Desc.Mode              = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride = sizeof(float4);
    Desc.Size              = sizeof(float4) * Materials.GetStats().MaterialCount * (Triangle ? 1 : ColorSamplesPerMaterial);
    Desc.BindFlags         = BIND_UNORDERED_ACCESS;
    RefCntAutoPtr<IBuffer> Output, Readback;
    Device->CreateBuffer(Desc, nullptr, &Output);
    Desc.Usage          = USAGE_STAGING;
    Desc.BindFlags      = BIND_NONE;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    Device->CreateBuffer(Desc, nullptr, &Readback);
    Require(Output && Readback, "color sampling buffers");
    RefCntAutoPtr<IShaderResourceBinding> SRB;
    PSO->CreateShaderResourceBinding(&SRB, true);
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_PTMaterialData")->Set(Materials.GetMaterialBuffer()->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_BindlessTextures")->SetArray(Materials.GetTextureBindings(), 0, Materials.GetTextureCount());
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "Output")->Set(Output->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    Context->SetPipelineState(PSO);
    Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->DispatchCompute({Materials.GetStats().MaterialCount, 1, 1});
    Context->CopyBuffer(Output, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->WaitForIdle();
    void* Data = nullptr;
    Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
    Require(Data != nullptr, "map color samples");
    std::vector<float4> Result(static_cast<size_t>(Desc.Size / sizeof(float4)));
    std::memcpy(Result.data(), Data, static_cast<size_t>(Desc.Size));
    Context->UnmapBuffer(Readback, MAP_READ);
    return Result;
}

static void TestMaterialColorSpace(IRenderDevice* Device, IDeviceContext* Context, IEngineFactory* Factory, const std::filesystem::path& Temp)
{
    // 2x2 RGBA8 PNGs: uniform 128; alternating black/white columns, with alpha 128 throughout.
    const Bytes  Gray    = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 2, 8, 6, 0, 0, 0, 114, 182, 13, 36, 0, 0, 0, 14, 73, 68, 65, 84, 120, 156, 99, 104, 128, 2, 6, 24, 3, 0, 72, 18, 8, 1, 79, 247, 104, 84, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
    const Bytes  Pattern = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 2, 8, 6, 0, 0, 0, 114, 182, 13, 36, 0, 0, 0, 20, 73, 68, 65, 84, 120, 156, 99, 96, 96, 96, 104, 248, 255, 255, 127, 3, 3, 140, 1, 0, 59, 229, 7, 251, 151, 209, 166, 103, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
    MemorySource Memory;
    Memory.Root                       = (Temp / "RTXPTColorSpace-memory").string();
    Memory.Files                      = MakeSceneFiles();
    auto Model                        = nlohmann::json::parse(Memory.Files[ModelFile]);
    Model["images"]                   = {{{"uri", "gray.png"}}, {{"uri", "pattern.png"}}};
    Model["textures"]                 = {{{"source", 0}}, {{"source", 1}}};
    Model["materials"]                = nlohmann::json::array();
    constexpr Uint32 FixtureMaterials = 4;
    for (Uint32 i = 0; i < FixtureMaterials; ++i)
    {
        const Uint32 Texture = i == 1 ? 1 : 0;
        Model["materials"].push_back({{"name", "color" + std::to_string(i)},
                                      {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", Texture}}}, {"metallicRoughnessTexture", {{"index", Texture}}}, {"baseColorFactor", {0.5, 0.25, 0.125, 0.5}}, {"metallicFactor", 0.75}, {"roughnessFactor", 0.25}}},
                                      {"emissiveTexture", {{"index", Texture}}},
                                      {"emissiveFactor", {0.5, 0.25, 0.125}}});
    }
    Memory.Files[ModelFile]                        = ToBytes(Model.dump());
    Memory.Files["gray.png"]                       = Gray;
    Memory.Files["pattern.png"]                    = Pattern;
    Memory.Files["external-color.png"]             = Gray;
    Memory.Files["external-data.png"]              = Gray;
    Memory.Files["Materials/color2.material.json"] = ToBytes(R"({"BaseTexture":{"path":"external-color.png","sRGB":true},"EmissiveTexture":{"path":"external-color.png","sRGB":true},"BaseOrDiffuseColor":[0.5,0.25,0.125],"Opacity":0.5,"EmissiveColor":[0.5,0.25,0.125]})");
    Memory.Files["Materials/color3.material.json"] = ToBytes(R"({"BaseTexture":{"path":"external-data.png","sRGB":false},"EmissiveTexture":{"path":"external-data.png","sRGB":false},"BaseOrDiffuseColor":[0.5,0.25,0.125],"Opacity":0.5,"EmissiveColor":[0.5,0.25,0.125]})");
    RTXPTSceneLoadOptions Options;
    Options.RayTracing = false;
    Options.FileSource = Memory.Source();
    RTXPTScene Scene;
    Require(Scene.LoadScene(Device, Context, Memory.Root, SceneFile, Options), "color scene load");
    RTXPTMaterials Materials;
    Require(Materials.Upload(Device, Context, Scene.GetSceneGraphData(), Memory.Root, Options.FileSource), "color material upload");
    constexpr Uint32 ExpectedBindings = 6; // Two glTF images, each with color/data views, plus two external textures.
    Require(Materials.GetTextureCount() == ExpectedBindings, "reuse bindings for repeated color/data uses");
    const auto      Samples     = SampleColorMaterials(Device, Context, Factory, Materials, false);
    const auto      Triangles   = SampleColorMaterials(Device, Context, Factory, Materials, true);
    constexpr float LinearGray  = 0.2158605f;
    constexpr float EncodedGray = 128.0f / 255.0f;
    constexpr float Tolerance   = 0.006f; // Includes RGBA8 mip quantization and hardware sRGB approximation.
    const auto      Near        = [Tolerance](float Expected, float Actual, const std::string& Label) {
        Require(std::abs(Expected - Actual) < Tolerance, Label + ": expected " + std::to_string(Expected) + ", got " + std::to_string(Actual));
    };
    for (Uint32 i = 0; i < FixtureMaterials; ++i)
    {
        const float Color = i == 1 ? 0.5f : i == 3 ? EncodedGray : LinearGray;
        const float Data  = i == 1 ? 0.5f : EncodedGray;
        const auto* S     = &Samples[i * ColorSamplesPerMaterial];
        Near(Color * 0.5f, S[0].x, "base color " + std::to_string(i));
        Near(Color * 0.25f, S[0].y, "base color green");
        Near(Color * 0.125f, S[0].z, "base color blue");
        Near(EncodedGray * 0.5f, S[0].w, "linear alpha");
        Near(Color * 0.5f, S[1].x, "surface emission");
        Near(Color * 0.25f, S[1].y, "surface emission green");
        Near(Color * 0.125f, S[1].z, "surface emission blue");
        Near(Color * 0.5f, Triangles[i].x, "triangle emission mip");
        Near(Color * 0.25f, Triangles[i].y, "triangle emission green");
        Near(Color * 0.125f, Triangles[i].z, "triangle emission blue");
        Near(Data, S[2].x, "linear data");
        Near(Color, S[3].x, "color mip");
        Near(EncodedGray, S[3].w, "color mip alpha");
        Near(Data, S[4].x, "data mip");
        // External material factors are independently authored and covered by the file-source test.
        if (i < 2)
        {
            Near(Data * 0.75f, S[5].x, "metallic factor");
            Near(Data * 0.25f, S[5].y, "roughness factor");
        }
    }
    std::cout << "PASS: glTF color/data reuse, sRGB filtering/mips, alpha, emission and external textures\n";
}
