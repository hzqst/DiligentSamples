// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.

// Exercise the production skinning shader with the CPU's interleaved 32-byte
// vertices. Two distinct vertices catch both member-offset and array-stride bugs.
void CheckGeometryVertexLayout(IRenderDevice* Device, IDeviceContext* Context, IShaderSourceInputStreamFactory* Source)
{
    const float  Vertices[16] = {1, 2, 3, 0, 1, 0, 0.25f, 0.75f,
                                4, 5, 6, 1, 0, 0, 0.5f, 0.125f};
    const float  Skin[16]     = {0, 0, 0, 0, 1, 0, 0, 0,
                            0, 0, 0, 0, 1, 0, 0, 0};
    const float  Joint[16]    = {1, 0, 0, 0, 0, 1, 0, 0,
                             0, 0, 1, 0, 10, 20, 30, 1};
    const Uint32 Constants[4] = {0, 0, 0, 2};

    auto MakeBuffer = [&](const char* Name, Uint64 Size, Uint32 Stride, const void* Data, Diligent::BIND_FLAGS Bind) {
        BufferDesc Desc;
        Desc.Name              = Name;
        Desc.Size              = Size;
        Desc.ElementByteStride = Stride;
        Desc.Mode              = Stride != 0 ? BUFFER_MODE_STRUCTURED : BUFFER_MODE_UNDEFINED;
        Desc.BindFlags         = Bind;
        Desc.Usage             = Data != nullptr ? USAGE_IMMUTABLE : USAGE_DEFAULT;
        BufferData             Init{Data, Size};
        RefCntAutoPtr<IBuffer> Buffer;
        Device->CreateBuffer(Desc, Data != nullptr ? &Init : nullptr, &Buffer);
        Require(Buffer != nullptr, "Geometry test buffer creation failed");
        return Buffer;
    };
    auto Input     = MakeBuffer("Geometry layout input", sizeof(Vertices), 32, Vertices, BIND_SHADER_RESOURCE);
    auto SkinInput = MakeBuffer("Geometry layout skin", sizeof(Skin), 32, Skin, BIND_SHADER_RESOURCE);
    auto Joints    = MakeBuffer("Geometry layout joints", sizeof(Joint), 64, Joint, BIND_SHADER_RESOURCE);
    auto Uniforms  = MakeBuffer("Geometry layout constants", sizeof(Constants), 0, Constants, BIND_UNIFORM_BUFFER);
    auto Output    = MakeBuffer("Geometry layout output", sizeof(Vertices), 32, nullptr, BIND_UNORDERED_ACCESS);

    ShaderCreateInfo CI;
    CI.Desc                       = {"Geometry vertex layout regression", SHADER_TYPE_COMPUTE, false};
    CI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    CI.ShaderCompiler             = SHADER_COMPILER_DXC;
    CI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    CI.FilePath                   = "PathTracer/SkinnedVertexBuild.csh";
    CI.EntryPoint                 = "main";
    CI.pShaderSourceStreamFactory = Source;
    RefCntAutoPtr<IShader> Shader;
    Device->CreateShader(CI, &Shader);
    Require(Shader != nullptr, "Geometry vertex shader compilation failed");
    ComputePipelineStateCreateInfo PSOCI;
    PSOCI.PSODesc.Name                               = "Geometry vertex layout regression";
    PSOCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    PSOCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    PSOCI.pCS                                        = Shader;
    RefCntAutoPtr<IPipelineState> PSO;
    Device->CreateComputePipelineState(PSOCI, &PSO);
    Require(PSO != nullptr, "Geometry vertex pipeline creation failed");
    RefCntAutoPtr<IShaderResourceBinding> SRB;
    PSO->CreateShaderResourceBinding(&SRB, true);
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_SourceVertices")->Set(Input->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_SourceSkinData")->Set(SkinInput->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_JointMatrices")->Set(Joints->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "cbSkinningConstants")->Set(Uniforms);
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_SkinnedVertices")->Set(Output->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    Context->SetPipelineState(PSO);
    Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});

    BufferDesc ReadDesc;
    ReadDesc.Name           = "Geometry vertex readback";
    ReadDesc.Size           = sizeof(Vertices);
    ReadDesc.Usage          = USAGE_STAGING;
    ReadDesc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<IBuffer> Readback;
    Device->CreateBuffer(ReadDesc, nullptr, &Readback);
    Require(Readback != nullptr, "Geometry readback creation failed");
    Context->CopyBuffer(Output, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, sizeof(Vertices), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->Flush();
    Context->WaitForIdle();
    void* Mapped = nullptr;
    Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Mapped);
    Require(Mapped != nullptr, "Geometry readback mapping failed");
    std::array<float, 16> Actual;
    std::memcpy(Actual.data(), Mapped, sizeof(Vertices));
    Context->UnmapBuffer(Readback, MAP_READ);
    for (Uint32 Vertex = 0; Vertex < 2; ++Vertex)
        for (Uint32 Component = 0; Component < 8; ++Component)
            Near(Vertices[Vertex * 8 + Component] + (Component < 3 ? 10.0f * (Component + 1) : 0.0f), Actual[Vertex * 8 + Component]);
    std::cout << "Geometry vertex layout: two 32-byte vertices passed\n";
}
