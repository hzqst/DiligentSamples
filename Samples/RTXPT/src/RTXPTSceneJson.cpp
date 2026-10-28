/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RTXPTSceneJson.hpp"

#include <cctype>
#include <climits>
#include <fstream>
#include <sstream>

#include "BasicTypes.h"
#include "DebugUtilities.hpp"
#include "FileSystem.hpp"

namespace Diligent
{
namespace
{

// glTF 2.0 binary container (.glb): a header (magic, version, total length), then chunks of
// (length, type, data). The first chunk holds the glTF JSON.
constexpr Uint32 kGLBMagic           = 0x46546C67; // "glTF"
constexpr Uint32 kGLBJsonChunkType   = 0x4E4F534A; // "JSON"
constexpr size_t kGLBHeaderSize      = 12;
constexpr size_t kGLBChunkHeaderSize = 8;

// Reads the file in binary mode: JSON text is unaffected (CR is whitespace), and GLB bytes stay intact.
bool ReadDiskFile(const std::string& FilePath, std::string& Data, std::string& Error)
{
    std::ifstream File{FilePath, std::ios::binary};
    if (!File)
    {
        Error = "Unable to open file: " + FilePath;
        return false;
    }

    std::ostringstream Strm;
    Strm << File.rdbuf();
    Data = Strm.str();
    return true;
}

Uint32 ReadLittleEndianUint32(const std::string& Data, size_t Offset)
{
    Uint32 Value = 0;
    for (size_t i = 0; i < sizeof(Value); ++i)
        Value |= static_cast<Uint32>(static_cast<unsigned char>(Data[Offset + i])) << (i * CHAR_BIT);
    return Value;
}

bool IsGLBContainer(const std::string& Data)
{
    return Data.size() >= kGLBHeaderSize && ReadLittleEndianUint32(Data, 0) == kGLBMagic;
}

// Returns false when the first chunk is not a JSON chunk that fits in the data.
bool ReadGLBJsonChunk(const std::string& Data, std::string& JsonText)
{
    if (Data.size() < kGLBHeaderSize + kGLBChunkHeaderSize)
        return false;

    const size_t ChunkLength = ReadLittleEndianUint32(Data, kGLBHeaderSize);
    const Uint32 ChunkType   = ReadLittleEndianUint32(Data, kGLBHeaderSize + sizeof(Uint32));
    const size_t ChunkStart  = kGLBHeaderSize + kGLBChunkHeaderSize;
    if (ChunkType != kGLBJsonChunkType || ChunkLength > Data.size() - ChunkStart)
        return false;

    JsonText = Data.substr(ChunkStart, ChunkLength);
    return true;
}

std::string RemoveTrailingJsonCommas(const std::string& Source)
{
    std::string Output;
    Output.reserve(Source.size());

    bool InString = false;
    bool Escaped  = false;

    for (size_t i = 0; i < Source.size(); ++i)
    {
        const char Ch = Source[i];
        if (InString)
        {
            Output.push_back(Ch);
            if (Escaped)
                Escaped = false;
            else if (Ch == '\\')
                Escaped = true;
            else if (Ch == '"')
                InString = false;
            continue;
        }

        if (Ch == '"')
        {
            InString = true;
            Output.push_back(Ch);
            continue;
        }

        if (Ch == ',')
        {
            size_t Next = i + 1;
            while (Next < Source.size() && std::isspace(static_cast<unsigned char>(Source[Next])) != 0)
                ++Next;
            if (Next < Source.size() && (Source[Next] == ']' || Source[Next] == '}'))
                continue;
        }

        Output.push_back(Ch);
    }

    return Output;
}

bool ParseRelaxedJson(const std::string& Text, const std::string& FilePath, RTXPTJsonLoadResult& Result)
{
    Result.Json            = nlohmann::json::parse(Text, nullptr, false, true);
    Result.CommentsIgnored = true;
    if (!Result.Json.is_discarded())
        return true;

    Result.StrictParseFailed      = true;
    const std::string RelaxedText = RemoveTrailingJsonCommas(Text);
    Result.Json                   = nlohmann::json::parse(RelaxedText, nullptr, false, true);
    Result.RelaxedParseUsed       = true;
    Result.CommentsIgnored        = true;
    if (!Result.Json.is_discarded())
        return true;

    Result.Error = "Invalid JSON file after relaxed parsing: " + FilePath;
    LOG_ERROR_MESSAGE(Result.Error);
    return false;
}

} // namespace

bool RTXPTFileSource::Exists(const std::string& FilePath) const
{
    return FileExists ? FileExists(FilePath.c_str()) : FileSystem::FileExists(FilePath.c_str());
}

bool RTXPTFileSource::Read(const std::string& FilePath, std::string& Data, std::string& Error) const
{
    if (!ReadWholeFile)
        return ReadDiskFile(FilePath, Data, Error);

    std::vector<unsigned char> Bytes;
    if (!ReadWholeFile(FilePath.c_str(), Bytes, Error))
    {
        if (Error.empty())
            Error = "Unable to read file: " + FilePath;
        return false;
    }

    Data.assign(Bytes.begin(), Bytes.end());
    return true;
}

bool LoadRTXPTRelaxedJsonFile(const std::string& FilePath, RTXPTJsonLoadResult& Result, const RTXPTFileSource& Source)
{
    Result = {};

    std::string Text;
    std::string Error;
    if (!Source.Read(FilePath, Text, Error))
    {
        Result.Error = Error;
        LOG_ERROR_MESSAGE(Result.Error);
        return false;
    }

    return ParseRelaxedJson(Text, FilePath, Result);
}

bool LoadRTXPTGLTFJsonFile(const std::string& FilePath, RTXPTJsonLoadResult& Result, const RTXPTFileSource& Source)
{
    Result = {};

    std::string Data;
    std::string Error;
    if (!Source.Read(FilePath, Data, Error))
    {
        Result.Error = Error;
        LOG_ERROR_MESSAGE(Result.Error);
        return false;
    }

    if (!IsGLBContainer(Data))
        return ParseRelaxedJson(Data, FilePath, Result);

    // The JSON chunk of a GLB is strict JSON.
    std::string JsonText;
    if (ReadGLBJsonChunk(Data, JsonText))
    {
        Result.Json = nlohmann::json::parse(JsonText, nullptr, false);
        if (!Result.Json.is_discarded())
            return true;
    }

    Result.Error = "Invalid GLB JSON chunk: " + FilePath;
    LOG_ERROR_MESSAGE(Result.Error);
    return false;
}

bool ReadRTXPTFloatArray(const nlohmann::json& Object, const char* Key, float* Values, size_t Count)
{
    const auto It = Object.find(Key);
    if (It == Object.end() || !It->is_array() || It->size() < Count)
        return false;

    for (size_t Idx = 0; Idx < Count; ++Idx)
    {
        if (!(*It)[Idx].is_number())
            return false;
        Values[Idx] = (*It)[Idx].get<float>();
    }
    return true;
}

float ReadRTXPTOptionalFloat(const nlohmann::json& Object, const char* Key, float DefaultValue)
{
    const auto It = Object.find(Key);
    return It != Object.end() && It->is_number() ? It->get<float>() : DefaultValue;
}

std::string ReadRTXPTOptionalString(const nlohmann::json& Object, const char* Key, const char* DefaultValue)
{
    const auto It = Object.find(Key);
    return It != Object.end() && It->is_string() ? It->get<std::string>() : std::string{DefaultValue};
}

} // namespace Diligent
