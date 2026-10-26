// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Adapted from RTXPT-fork 9503910 TLASCull.hlsl. No camera culling: secondary
// and shadow rays retain the complete scene. The original instance order is kept.
ByteAddressBuffer t_Templates;
StructuredBuffer<uint4> t_InstanceData;
StructuredBuffer<row_major float4x4> t_Transforms;
RWByteAddressBuffer u_Instances;

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint index = (group.y * 65535 + group.x) * 64 + lane;
    uint count, stride;
    t_InstanceData.GetDimensions(count, stride);
    if (index >= count) return;
    uint4 instance = t_InstanceData[index];
    row_major float4x4 transform = t_Transforms[instance.x];
    uint address = index * 64;
    // Diligent uses row-vector transforms; native AS descriptors store 3 rows
    // of the transposed affine transform, including translation in column 3.
    u_Instances.Store4(address,      asuint(float4(transform[0][0], transform[1][0], transform[2][0], transform[3][0])));
    u_Instances.Store4(address + 16, asuint(float4(transform[0][1], transform[1][1], transform[2][1], transform[3][1])));
    u_Instances.Store4(address + 32, asuint(float4(transform[0][2], transform[1][2], transform[2][2], transform[3][2])));
    uint4 nativeData = t_Templates.Load4(address + 48);
    nativeData.x = (instance.y & 0x00ffffffu) | ((instance.z & 0xffu) << 24);
    u_Instances.Store4(address + 48, nativeData);
}
