#ifndef __STANDARD_BSDF_DATA_HLSLI__
#define __STANDARD_BSDF_DATA_HLSLI__

#include "../PathTracerHelpers.hlsli"
#include "../Utils/Packing.hlsli"

// v1.9 material parameters: exactly four packed words. Surface state and IOR
// belong to the Diligent surface adapter, not to this material record.
struct StandardBSDFData
{
    uint _diffuse_roughness;
    uint _specular_metallic;
    uint _transmission;
    uint _diffuseSpecularTransmission;

    lpfloat3 Diffuse() { return lpfloat3(Unpack_SRGB8(_diffuse_roughness)); }
    lpfloat3 Specular() { return lpfloat3(Unpack_SRGB8(_specular_metallic)); }
    lpfloat3 Transmission() { return lpfloat3(Unpack_SRGB8(_transmission)); }
    lpfloat Roughness() { return lpfloat(Unpack_A8(_diffuse_roughness)); }
    lpfloat Metallic() { return lpfloat(Unpack_A8(_specular_metallic)); }
    lpfloat DiffuseTransmission() { return lpfloat(f16tof32(_diffuseSpecularTransmission & 0xffffu)); }
    lpfloat SpecularTransmission() { return lpfloat(f16tof32(_diffuseSpecularTransmission >> 16)); }
    void SetRoughness(lpfloat value)
    {
        _diffuse_roughness = (_diffuse_roughness & 0x00ffffffu) | (uint(saturate(float(value)) * 255.0 + 0.5) << 24);
    }

    static StandardBSDFData make(lpfloat3 diffuse, lpfloat3 specular, lpfloat roughness, lpfloat metallic,
                                 lpfloat3 transmission, lpfloat diffuseTransmission, lpfloat specularTransmission)
    {
        StandardBSDFData result;
        result._diffuse_roughness = Pack_SRGB8_A8(diffuse, roughness);
        result._specular_metallic = Pack_SRGB8_A8(specular, metallic);
        result._transmission = Pack_SRGB8_A8(transmission, 0.0);
        result._diffuseSpecularTransmission = f32tof16(float(diffuseTransmission)) | (f32tof16(float(specularTransmission)) << 16);
        return result;
    }
};

#endif
