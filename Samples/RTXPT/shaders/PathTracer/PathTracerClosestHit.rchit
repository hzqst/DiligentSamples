#include "Config.h"

#define ENABLE_HIT_BRIDGE 1
#include "PathTracer.hlsli"
#include "BSDFs/MaterialBridge.hlsli"

using ActiveRayPayload = PathPayload;

namespace PathTracer
{
    inline float3 MakeFallbackTangent(float3 normal)
    {
        const float3 axis = abs(normal.x) > 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        return normalize(cross(axis, normal));
    }

    inline SurfaceData LoadCurrentSurfaceData(BuiltInTriangleIntersectionAttributes Attributes,
                                              out float3 surfaceEmission)
    {
        const float3 rayDir = WorldRayDirection();

        float3 worldNormal  = -rayDir;
        float3 faceNormal   = -rayDir;
        float3 worldPos     = WorldRayOrigin() + rayDir * RayTCurrent();
        float3 tangent      = MakeFallbackTangent(normalize(worldNormal));
        float  tangentSign  = 1.0;
        float  roughness    = 1.0;
        float3 baseColor    = float3(1.0, 1.0, 1.0);
        float  metallic     = 0.0;
        float  transmissionFactor        = 0.0;
        float  diffuseTransmissionFactor = 0.0;
        bool   thinSurface               = true;
        float  shadowNoLFadeout                 = 0.0;
        float3 vertexNormal                     = worldNormal;
        uint   materialID                       = 0u;
        bool   frontFacing                      = true;
        float  ior                              = 1.5;
        uint   materialFlags                    = 0u;
        uint   nestedPriority                   = 14u;
        uint   psdExclude                       = 0u;
        uint   psdBlockMotionVectorsAtSurface   = 0u;
        uint   psdDominantDeltaLobeP1           = 0u;
        float  emissiveLightPdf                 = 0.0;
        uint   neeTriangleLightIndex            = 0xFFFFFFFFu;
        surfaceEmission                         = float3(0.0, 0.0, 0.0);

        const SubInstanceData subInstance = Bridge::getSubInstanceData();
        const MaterialPTData  material    = Bridge::getMaterial(subInstance.MaterialID);
        materialID                        = subInstance.MaterialID;

        GeometryVertexData V0;
        GeometryVertexData V1;
        GeometryVertexData V2;
        Bridge::getTriangleVertices(subInstance, PrimitiveIndex(), V0, V1, V2);

        const float2 texCoord = Bridge::interpolateTexCoord(V0, V1, V2, Attributes.barycentrics);

        const float3 geometricNormal = Bridge::computeGeometricNormal(V0, V1, V2);
        frontFacing                  = dot(-rayDir, geometricNormal) >= 0.0;
        faceNormal                   = frontFacing ? geometricNormal : -geometricNormal;
        worldPos                     = Bridge::computeWorldHitPosition(V0, V1, V2, Attributes.barycentrics);
        worldNormal                  = Bridge::interpolateNormal(V0, V1, V2, Attributes.barycentrics);
        if (dot(worldNormal, worldNormal) < 1e-6)
            worldNormal = faceNormal;
        if (dot(worldNormal, faceNormal) < 0.0)
            worldNormal = -worldNormal;
        vertexNormal = worldNormal;

        const float3 tangentNormal = Bridge::ignoreMeshTangentSpace(material) ? float3(0.0, 0.0, 1.0) :
            Bridge::getTangentNormal(material, texCoord);
        if (abs(tangentNormal.x) + abs(tangentNormal.y) > 1e-5)
        {
            const float4 worldTangent = Bridge::computeWorldTangent(V0, V1, V2, worldNormal);
            const float3 T            = worldTangent.xyz;
            const float3 B            = cross(worldNormal, T) * worldTangent.w;
            const float3 mappedNormal = T * tangentNormal.x + B * tangentNormal.y + worldNormal * tangentNormal.z;
            const float  lenSq        = dot(mappedNormal, mappedNormal);
            if (lenSq > 1e-8)
            {
                worldNormal = mappedNormal * rsqrt(lenSq);
                if (dot(worldNormal, faceNormal) < 0.0)
                    worldNormal = -worldNormal;
            }
        }

        const float4 worldTangent = Bridge::computeWorldTangent(V0, V1, V2, worldNormal);
        tangent                   = worldTangent.xyz;
        tangentSign               = worldTangent.w;

        const float2 metalRough = Bridge::getMetallicRoughness(material, texCoord);
        metallic                = metalRough.x;
        roughness               = metalRough.y;
        const float4 baseColorWithAlpha = Bridge::getBaseColor(material, texCoord);
        baseColor                       = baseColorWithAlpha.rgb;
        transmissionFactor              = Bridge::getTransmission(material, texCoord);
        diffuseTransmissionFactor       = Bridge::getDiffuseTransmission(material, texCoord);
        thinSurface                     = Bridge::isThinSurface(material);
        shadowNoLFadeout                = Bridge::loadShadowNoLFadeout(materialID);
        // RTXPT-fork single-sided emissives: only front-facing hits emit (PathTracerBridgeDonut.hlsli),
        // keeping the BSDF-hit emission consistent with the one-sided NEE TriangleLight.
        surfaceEmission                 = frontFacing ? Bridge::getEmission(material, texCoord) : float3(0.0, 0.0, 0.0);
        // One-sided area-light pdf: only the front face can be sampled by NEE, so back-face hits carry no
        // emissive light pdf (and emit nothing via the gate above), keeping BSDF/NEE MIS consistent.
        if (!UseApproximateLightMIS() && frontFacing && (material.flags & kMaterialFlagEmissiveAreaLight) != 0u)
        {
            // Full MIS looks up the same environment / analytic / triangle table as NEE.
            neeTriangleLightIndex = Bridge::getLightingControl().EnvmapQuadNodeCount + Bridge::getAnalyticLightCount() + subInstance.emissiveTriangleOffset + PrimitiveIndex();

            const float3 wp0   = mul(ObjectToWorld3x4(), float4(V0.position, 1.0));
            const float3 wp1   = mul(ObjectToWorld3x4(), float4(V1.position, 1.0));
            const float3 wp2   = mul(ObjectToWorld3x4(), float4(V2.position, 1.0));
            const float3 ng    = cross(wp1 - wp0, wp2 - wp0);
            const float  ngLen = length(ng);
            const float  area  = 0.5 * ngLen;
            if (area > 1e-9)
            {
                const float3 normal   = ng / ngLen;
                const float  cosTheta = abs(dot(normal, -rayDir));
                if (cosTheta > 2e-9)
                    emissiveLightPdf = min(kMaxSolidAnglePdf, (1.0 / area) * (RayTCurrent() * RayTCurrent()) / cosTheta);
            }
        }
        ior                             = Bridge::loadIoR(materialID);
        materialFlags                   = material.flags;
        // Match RTXPT-fork PathTracerBridgeDonut::loadSurface: stored nested priority is
        // 1 + authored, clamped to kMaxNestedPriority. Authored priorities therefore map to
        // the [1, kMaxNestedPriority] range because InteriorList reserves 0 for empty slots
        // (and remaps a stored 0 to kMaxNestedPriority). Omitting the +1 made an authored
        // priority of 0 collapse to the *highest* priority instead of the lowest, so a
        // priority-0 dielectric (e.g. "Thin Smooth Transmission", "Smooth Glass") wrongly
        // outranked opaque geometry behind it and false-hit-rejected it (seeing the envmap
        // straight through the glass instead of the refracted wall).
        nestedPriority                  = min(InteriorList::kMaxNestedPriority, 1u + material.nestedPriority);
        psdExclude                      = Bridge::isPSDExclude(material) ? 1u : 0u;
        psdBlockMotionVectorsAtSurface  = Bridge::isPSDBlockMotionVectorsAtSurface(material) ? 1u : 0u;
        psdDominantDeltaLobeP1          = Bridge::getPSDDominantDeltaLobeP1(material);

        worldNormal = normalize(worldNormal);
        tangent     = normalize(tangent);

        DenoisingLayerMaterialState mtl;
        mtl.flags                          = materialFlags;
        mtl.nestedPriority                 = nestedPriority;
        mtl.activeLobes                    = kLobeTypeAll;
        mtl.psdExclude                     = psdExclude;
        mtl.psdBlockMotionVectorsAtSurface = psdBlockMotionVectorsAtSurface;
        mtl.psdDominantDeltaLobeP1         = psdDominantDeltaLobeP1;

        ShadingData shadingData;
        shadingData.posW        = worldPos;
        shadingData.N           = worldNormal;
        shadingData.V           = normalize(-rayDir);
        shadingData.T           = tangent;
        shadingData.B           = normalize(cross(worldNormal, tangent) * tangentSign);
        shadingData.materialID  = materialID;
        shadingData.frontFacing = frontFacing;
        shadingData.mtl         = mtl;
        shadingData.faceNCorrected    = faceNormal;
        shadingData.vertexN           = vertexNormal;
        shadingData.shadowNoLFadeout  = shadowNoLFadeout;
        shadingData.emission          = surfaceEmission;
        const float3 objectExtent = max(abs(V0.position), max(abs(V1.position), abs(V2.position)));
        const float3 extentW = mul(abs((float3x3)ObjectToWorld3x4()), objectExtent);
        shadingData.posErrorScale = max(max(abs(worldPos.x), max(abs(worldPos.y), abs(worldPos.z))),
                                       max(extentW.x, max(extentW.y, extentW.z)));
        shadingData.exteriorIoR = lpfloat(1.0);
        shadingData.interiorIoR = lpfloat(ior);

        ActiveBSDF bsdf;
        bsdf.standardData = MakeStandardBSDFData(worldNormal,
                                                 baseColor,
                                                 metallic,
                                                 roughness,
                                                 ior,
                                                 1.0,
                                                 transmissionFactor,
                                                 diffuseTransmissionFactor,
                                                 thinSurface,
                                                 frontFacing,
                                                 mtl.getActiveLobes(),
                                                 mtl.isPSDExclude(),
                                                 mtl.isPSDBlockMotionVectorsAtSurface(),
                                                 mtl.getPSDDominantDeltaLobeP1());

        return SurfaceData::make(shadingData,
                                 bsdf,
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
                                 worldPos,
#endif
                                 neeTriangleLightIndex,
                                 0xFFFFFFFFu,
                                  emissiveLightPdf);
    }

    inline void HandleHit(inout PathState path,
                          BuiltInTriangleIntersectionAttributes Attributes,
                          const WorkingContext workingContext)
    {
        float3 surfaceEmission;
        SurfaceData surfaceData = LoadCurrentSurfaceData(Attributes, surfaceEmission);
        const float3 rayOrigin    = WorldRayOrigin();
        const float3 rayDir       = WorldRayDirection();
        const float  rayTCurrent  = RayTCurrent();

#if PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE && !RTXPT_SHARC_TRAIN
        UpdatePathTravelled(path, rayOrigin, rayDir, rayTCurrent, workingContext);

#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0 || defined(__INTELLISENSE__)
        if (!path.interiorList.isEmpty())
        {
            const HomogeneousVolumeData volume = Bridge::loadHomogeneousVolumeData(path.interiorList.getTopMaterialID());
            const float3 transmittance = HomogeneousVolumeSampler::evalTransmittance(volume, rayTCurrent);
            UpdatePathThroughput(path, transmittance);
        }
#endif

        const bool rejectedFalseHit = !HandleNestedDielectrics(surfaceData, path, workingContext);
        if (rejectedFalseHit)
            return;

        float3 referenceSurfaceEmission = surfaceEmission;
        const NEEBSDFMISInfo prevMISInfo = NEEBSDFMISInfo::Unpack16bit(path.GetPackedMISInfo());
        const bool didEmissiveNEE =
            prevMISInfo.LightSamplingEnabled &&
            prevMISInfo.FullSamples > 0u &&
            Bridge::getEmissiveTriangleCount() > 0u;
        if (didEmissiveNEE && path.GetBsdfScatterPdf() > 0.0)
        {
            referenceSurfaceEmission *= ComputeLightVsBSDFMISForBSDF(path.GetPixelPos(),
                                                                          surfaceData.neeTriangleLightIndex,
                                                                          path.GetBsdfScatterPdf(),
                                                                          surfaceData.emissiveLightPdf,
                                                                          prevMISInfo.CandidateSamples,
                                                                          prevMISInfo.FullSamples,
                                                                          prevMISInfo.LightSamplingIsSSC);
        }
        referenceSurfaceEmission = ApplyEmissionFireflyFilter(referenceSurfaceEmission, path, workingContext);

        if (any(referenceSurfaceEmission > 0.0))
        {
            const float3 radiance = path.GetThp() * referenceSurfaceEmission;
            const float specRadianceAvg =
                path.hasFlag(PathFlags::denoisingLayerBaseScatterDiff) ? 0.0 : Average(radiance);
            AccumulatePathRadiance(workingContext,
                                   path,
                                   radiance,
                                   specRadianceAvg,
                                   path.hasFlag(PathFlags::denoisingLayerOnBranch),
                                   ShouldCollectGISecondaryRadiance(path));
        }

        const bool pathStopping = path.isTerminatingAtNextBounce();
        Bridge::ExportSurface(path, surfaceData, path.GetSceneLength(), float3(0.0, 0.0, 0.0));

        if (pathStopping)
        {
            path.terminate();
            return;
        }

        UpdatePathThroughput(path, path.GetThpRuRuCorrection().xxx);

        const PathState preScatterPath = path;
        BSDFSample bs;
        const bool scatterValid = GenerateScatterRay(surfaceData, path, workingContext, bs);

        SampleGenerator sgDirect = SampleGenerator_makeStateless(preScatterPath.GetPixelPos(),
                                                                 preScatterPath.getVertexIndex(),
                                                                 Bridge::getSampleIndex(),
                                                                 kSampleEffect_NEELightSampler);
        NEEResult neeResult = HandleNEE(preScatterPath, surfaceData, sgDirect, workingContext);
        path.SetPackedMISInfo_ThpRuRuCorrection(neeResult.BSDFMISInfo.Pack16bit(), path.GetThpRuRuCorrection());
        AccumulateNEERadiance(workingContext, path, preScatterPath, neeResult);

        if (!scatterValid)
        {
            path.terminate();
            return;
        }

        const bool shouldTerminate = HasFinishedSurfaceBounces(path.getVertexIndex() + 1,
                                                               path.getCounter(PackedCounters::DiffuseBounces));
        if (shouldTerminate)
        {
            path.setTerminateAtNextBounce();
        }
        else if (!HandleRussianRoulette(path, preScatterPath, workingContext))
        {
            return;
        }
#else
        HandleHit(path, surfaceData, surfaceEmission, rayOrigin, rayDir, rayTCurrent, workingContext);
        return;
#endif
    }
}

[shader("closesthit")]
void main(inout ActiveRayPayload Payload,
          in BuiltInTriangleIntersectionAttributes Attributes)
{
    PathState path = PathPayload::unpack(Payload);
    PathTracer::WorkingContext workingContext = GetWorkingContext();
    PathTracer::HandleHit(path, Attributes, workingContext);
    Payload = PathPayload::pack(path);
}

// RTXPT-Port Phase R2 (done): emissive triangles feed area-light NEE + MIS with GPU-baked textured radiance
// (EmissiveTriangleBuild.hlsl), are one-sided like RTXPT-fork's TriangleLight (front-face gated NEE/MIS/emission),
// and are importance-sampled per-triangle proportional to power via the GPU proxy build (LightProxyBuild.hlsl).
// Deviations from RTXPT-fork: the per-triangle proxy scatter uses a single atomic running offset instead of
// upstream's prefix-sum + task system (identical proxy distribution); emissive triangles live in a separate
// buffer rather than the unified AnalyticLightData light buffer; and the proxy build runs on scene/lights
// change rather than every frame, so animated emissive radiance is not re-weighted per frame.
