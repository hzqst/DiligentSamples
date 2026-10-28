/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <cstddef>
#include <string>

#include "json.hpp"

namespace Diligent
{

struct RTXPTJsonLoadResult
{
    nlohmann::json Json;
    std::string    Error;
    bool           StrictParseFailed = false;
    bool           RelaxedParseUsed  = false;
    bool           CommentsIgnored   = false;
};

bool        LoadRTXPTRelaxedJsonFile(const std::string& FilePath, RTXPTJsonLoadResult& Result);
bool        ReadRTXPTFloatArray(const nlohmann::json& Object, const char* Key, float* Values, size_t Count);
float       ReadRTXPTOptionalFloat(const nlohmann::json& Object, const char* Key, float DefaultValue);
std::string ReadRTXPTOptionalString(const nlohmann::json& Object, const char* Key, const char* DefaultValue = "");

// Loads the JSON of a glTF model: the text of a .gltf file, or the JSON chunk of a binary .glb container.
bool LoadRTXPTGLTFJsonFile(const std::string& FilePath, RTXPTJsonLoadResult& Result);

} // namespace Diligent
