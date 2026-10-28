/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "json.hpp"

namespace Diligent
{

// Where scene files are read from: the scene and material JSON files, the glTF models and the files they reference.
// The callbacks have the signatures of the glTF loader's file callbacks (GLTF::ModelCreateInfo::FileExistsCallback and
// ReadWholeFileCallback), which receive them for the models. Paths are the assets root joined with the relative paths
// of the scene, with the platform's slashes. A source without callbacks reads the disk.
struct RTXPTFileSource
{
    using FileExistsCallbackType    = std::function<bool(const char* FilePath)>;
    using ReadWholeFileCallbackType = std::function<bool(const char* FilePath, std::vector<unsigned char>& Data, std::string& Error)>;

    FileExistsCallbackType    FileExists    = nullptr;
    ReadWholeFileCallbackType ReadWholeFile = nullptr;

    bool Exists(const std::string& FilePath) const;

    // Returns false and sets Error when the file cannot be read.
    bool Read(const std::string& FilePath, std::string& Data, std::string& Error) const;
};

struct RTXPTJsonLoadResult
{
    nlohmann::json Json;
    std::string    Error;
    bool           StrictParseFailed = false;
    bool           RelaxedParseUsed  = false;
    bool           CommentsIgnored   = false;
};

bool        LoadRTXPTRelaxedJsonFile(const std::string& FilePath, RTXPTJsonLoadResult& Result, const RTXPTFileSource& Source = {});
bool        ReadRTXPTFloatArray(const nlohmann::json& Object, const char* Key, float* Values, size_t Count);
float       ReadRTXPTOptionalFloat(const nlohmann::json& Object, const char* Key, float DefaultValue);
std::string ReadRTXPTOptionalString(const nlohmann::json& Object, const char* Key, const char* DefaultValue = "");

// Loads the JSON of a glTF model: the text of a .gltf file, or the JSON chunk of a binary .glb container.
bool LoadRTXPTGLTFJsonFile(const std::string& FilePath, RTXPTJsonLoadResult& Result, const RTXPTFileSource& Source = {});

} // namespace Diligent
