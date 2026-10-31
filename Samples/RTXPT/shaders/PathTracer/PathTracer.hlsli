#ifndef __PATH_TRACER_HLSLI__
#define __PATH_TRACER_HLSLI__

#include "Config.h"
#include "PathTracerBridge.hlsli"
#include "PathTracerVisibility.hlsli"
#include "PathTracerHelpers.hlsli"
#include "Lighting/EnvMap.hlsli"
#include "Lighting/LightSampler.hlsli"
#include "BSDFs/BxDF.hlsli"
#include "Utils/StatelessSampleGenerators.hlsli"
#include "Rendering/Volumes/HomogeneousVolumeSampler.hlsli"
#include "PathTracerNestedDielectrics.hlsli"

#include "PathState.hlsli"
#include "PathPayload.hlsli"
#include "PathTracerSHaRC.hlsli"

#if PATH_TRACER_MODE != PATH_TRACER_MODE_REFERENCE || defined(__INTELLISENSE__)
#    include "PathTracerDenoisingLayers.hlsli"
#endif

static const float kVisibilityRayTMin = 0.0;
static const float kVisibilityRayTMax = 1e30;

// Production pipelines specialize these settings, as in RTXPT-fork. Retain
// runtime defaults for shader consumers that do not supply the permutation.
#ifndef RTXPT_NEE_FULL_SAMPLE_COUNT
#define RTXPT_NEE_FULL_SAMPLE_COUNT min(RTXPT_LIGHTING_MAX_SAMPLE_COUNT, g_Const.ptConsts.NEEFullSamples)
#endif
#ifndef RTXPT_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT
#define RTXPT_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT max(1u, min(RTXPT_LIGHTING_MAX_SAMPLE_COUNT, g_Const.ptConsts.NEECandidateSamples))
#endif

namespace PathTracer
{
    static const float kSpecularRoughnessThreshold = 0.25;

    inline PathState EmptyPathInitialize(uint2 pixelPos, float pixelConeSpreadAngle);

#if !RTXPT_USE_INLINE_VISIBILITY
    inline PathPayload MakeVisibilityPayload(uint2 pixelPos)
    {
        PathState visibilityPath = EmptyPathInitialize(pixelPos, 0.0);
        visibilityPath.setActive();
        visibilityPath.clearHit();
#if PATH_TRACER_MODE != PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        visibilityPath.SetL(float4(0.0, 0.0, 0.0, 0.0));
#endif
        visibilityPath.SetThp(float3(0.0, 0.0, 0.0));
        return PathPayload::pack(visibilityPath);
    }
#endif

    bool TraceVisibilityRay(float3 origin, float3 dir, float tMax)
    {
        if (tMax <= kVisibilityRayTMin)
            return false;

        RayDesc ray;
        ray.Origin    = origin;
        ray.Direction = dir;
        ray.TMin      = kVisibilityRayTMin;
        ray.TMax      = tMax;

#if RTXPT_USE_INLINE_VISIBILITY
        return Bridge::traceVisibilityRay(ray);
#else
        PathPayload payload = MakeVisibilityPayload(Bridge::getPixelPosition());
        TraceRay(t_SceneBVH,
                 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 0xFF,
                 RTXPT_VISIBILITY_RAY_INDEX,
                 RTXPT_HIT_GROUP_STRIDE,
                 RTXPT_VISIBILITY_RAY_INDEX,
                 ray,
                 payload);
        return !PathPayload::unpack(payload).isActive();
#endif
    }

    float3 MakeVisibilityOrigin(float3 hitPos, float3 faceNormal, float3 shadingNormal, float3 dir, float posErrorScale)
    {
        const float side = dot(shadingNormal, dir) >= 0.0 ? 1.0 : -1.0;
        return ComputeRayOrigin(hitPos, faceNormal * side, posErrorScale);
    }

    float ComputeShadowNoLFadeout(float3 lightDir, float3 vertexNormal, float shadowNoLFadeout)
    {
        return shadowNoLFadeout > 0.0 ?
            ComputeLowGrazingAngleFalloff(lightDir, vertexNormal, shadowNoLFadeout, 2.0 * shadowNoLFadeout) :
            1.0;
    }

#if PATH_TRACER_MODE != PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    float ComputeLightVsBSDFMISForLightSample(DirectLightSample sample, uint fullSamples, uint candidates, float thisPdf, float otherPdf, float bsdfPdf)
    {
        if (!sample.sampleableByBSDF || bsdfPdf <= 0.0)
            return 1.0;

        if (UseApproximateLightMIS())
            return LightVsBSDFMISApprox(bsdfPdf, candidates, fullSamples, true);
        const float lightPdf = (thisPdf + otherPdf) * float(fullSamples) * sample.solidAnglePdf;
        return EvalLightSamplingMIS(1.0, lightPdf, 1.0, bsdfPdf);
    }

    DirectLightSample GenerateDirectLightSample(SurfaceBSDFData bsdfData, float3 hitPos, float3 wo,
                                                uint2 pixelPos, bool coherent, uint candidateSamples, inout SampleGenerator sg)
    {
        uint localCount, globalCount;
        GetCandidateSampleCounts(Bridge::getLightingControl(), coherent && HasLocalLightSampling(Bridge::getLightingControl()),
                                 candidateSamples, localCount, globalCount);

        NEEWeightedReservoirSampler wrs = NEEWeightedReservoirSampler::make();
        for (uint candidateIndex = 0u; candidateIndex < candidateSamples; ++candidateIndex)
        {
            DirectLightSample candidate = GenerateDirectLightCandidate(hitPos, pixelPos, candidateIndex >= globalCount, sg);
            const float weight = EvalDirectLightCandidateWeight(candidate, bsdfData, wo);
            wrs.Add(sampleNext1D(sg), candidate, weight);
        }

        const float candidateProbability = wrs.CandidateProbability();
        if (candidateProbability <= 0.0)
            return DirectLightSample_make_empty();

        // Match RTXPT-fork GenerateLightSample: finish reservoir normalization here.
        DirectLightSample picked = wrs.candidate;
        picked.radianceOverPdf /= candidateProbability;
        return picked;
    }

    float3 SampleDirectLightNEE(SurfaceBSDFData bsdfData, float3 hitPos, float3 faceNormal, float3 vertexNormal,
                                float posErrorScale, float shadowNoLFadeout, float3 wo, uint2 pixelPos, inout SampleGenerator sg,
                                float fireflyFilterK, bool coherent, float3 pathThroughput, out float specularRadianceAvg)
    {
        specularRadianceAvg = 0.0;

        const uint fullSamples = RTXPT_NEE_FULL_SAMPLE_COUNT;
        if (fullSamples == 0u || Bridge::getTotalLightCount() == 0u)
            return float3(0.0, 0.0, 0.0);

        const uint candidateSamples = RTXPT_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT;
        float3 result = float3(0.0, 0.0, 0.0);
        for (uint sampleIndex = 0u; sampleIndex < fullSamples; ++sampleIndex)
        {
            DirectLightSample picked = GenerateDirectLightSample(bsdfData, hitPos, wo, pixelPos, coherent, candidateSamples, sg);
            if (!picked.Valid())
                continue;

            const LightingControlData ctrl = Bridge::getLightingControl();
            const bool isEmissiveTriangle = picked.index >= ctrl.EnvmapQuadNodeCount + ctrl.AnalyticLightCount;
            const float visibilityDistance =
                isEmissiveTriangle ? picked.distance * 0.9985 : picked.distance;
            const float3 visibilityOrigin = MakeVisibilityOrigin(hitPos, faceNormal, bsdfData.N, picked.dir, posErrorScale);
            const bool visible = TraceVisibilityRay(visibilityOrigin, picked.dir, visibilityDistance);
            if (!visible)
                continue;

            uint localCount, globalCount;
            GetCandidateSampleCounts(Bridge::getLightingControl(), coherent && HasLocalLightSampling(Bridge::getLightingControl()),
                                     candidateSamples, localCount, globalCount);
            float thisPdf, otherPdf, thisCount;
            ComputeLightSelectionPdfs(picked, localCount, globalCount, pixelPos, thisPdf, otherPdf, thisCount);
            const float wrsScale = EvalLightSamplingMIS(1.0, thisPdf, 1.0, otherPdf) / thisCount;
            // Recompute the selected PDF after visibility, as in RTXPT-fork ProcessLightSample.
            const float bsdfPdf = EvalBSDFPdf(bsdfData, wo, picked.dir);
            const float misWeight  = ComputeLightVsBSDFMISForLightSample(picked, fullSamples, candidateSamples, thisPdf, otherPdf, bsdfPdf);
            const float fadeOut    = ComputeShadowNoLFadeout(picked.dir, faceNormal, shadowNoLFadeout);
            const float4 bsdfValue = EvalBSDFValue(bsdfData, wo, picked.dir);
            const float3 incidentRadiance = picked.radianceOverPdf * (fadeOut * wrsScale * misWeight / float(fullSamples));
            float3 contribution = bsdfValue.rgb * incidentRadiance;
            const float specularContribution = bsdfValue.w * Average(incidentRadiance) * Average(pathThroughput);
            const float3 feedbackContribution = contribution * Average(pathThroughput);

            const float ffThreshold = g_Const.ptConsts.fireflyFilterThreshold;
            if (ffThreshold != 0.0)
            {
                const float proposalPdf = picked.selectionPdf * picked.solidAnglePdf;
                const float neeK = ComputeNewScatterFireflyFilterK(fireflyFilterK, proposalPdf, 1.0);
                const float filter = FireflyFilterShort(Average(contribution), ffThreshold, neeK);
                contribution *= filter;
            }

            // picked.index is the unified global light index for both analytic lights and emissive triangles.
            InsertFeedbackFromNEE(pixelPos, picked.index, feedbackContribution, sampleNext1D(sg), coherent);
            specularRadianceAvg += specularContribution;
            result += contribution;
        }

        return result;
    }
#endif

    inline bool ReferenceShouldRunNEE(const PathState preScatterPath, const WorkingContext workingContext)
    {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE
        const uint maxBounces    = max(workingContext.PtConsts.bounceCount, 1u);
        const uint maxNEEBounces = min(workingContext.PtConsts.maxNEEBounceCount, maxBounces);
        return preScatterPath.getVertexIndex() <= maxNEEBounces;
#else
        return true;
#endif
    }

    inline float3 ApplyEmissionFireflyFilter(float3 radiance, const PathState path, const WorkingContext workingContext)
    {
#if PATH_TRACER_MODE != PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        const float ffThreshold = workingContext.PtConsts.fireflyFilterThreshold;
        if (ffThreshold != 0.0)
            radiance = FireflyFilter(radiance, ffThreshold, path.GetFireflyFilterK());
#endif
        return radiance;
    }

    inline bool HasFinishedSurfaceBounces(uint vertexIndex, uint diffuseBounces)
    {
        if (g_Const.ptConsts.bounceCount < vertexIndex)
            return true;
        return diffuseBounces > g_Const.ptConsts.diffuseBounceCount;
    }

    inline PathState EmptyPathInitialize(uint2 pixelPos, float pixelConeSpreadAngle)
    {
        PathState path;
        path.SetId(PathIDFromPixel(pixelPos));
        path.flagsAndVertexIndex = 0;
        path.SetSceneLength(0.0);
        path.packedCounters = 0;

#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0 || defined(__INTELLISENSE__)
        for (uint i = 0; i < INTERIOR_LIST_SLOT_COUNT; ++i)
            path.interiorList.slots[i] = 0;
#endif

        path.SetOrigin(float3(0.0, 0.0, 0.0));
        path.SetDir(float3(0.0, 0.0, 0.0));
        path.SetThp(float3(1.0, 1.0, 1.0));
        path.setActive();
        path.setDeltaOnlyPath(true);
        path.rayCone = RayCone::make(0.0, pixelConeSpreadAngle);

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        path.SetImageXform(float3x3(1.0, 0.0, 0.0,
                                    0.0, 1.0, 0.0,
                                    0.0, 0.0, 1.0));
        path.setFlag(PathFlags::denoisingLayerOnDominantBranch, true);
        path.SetMotionVectorSceneLength(0.0);
        path.previousReflectionPlane = 0.0.xxxx;
#else
        path.SetL(float4(0.0, 0.0, 0.0, 0.0));
        path.SetFireflyFilterK_BsdfScatterPdf(1.0, 0.0);
        path.SetPackedMISInfo_ThpRuRuCorrection(NEEBSDFMISInfo::empty().Pack16bit(), 1.0);
#endif

        path.setDenoisingLayerIndex(0);
        path.stableBranchID = 1u;

        if (HasFinishedSurfaceBounces(path.getVertexIndex() + 1,
                                      path.getCounter(PackedCounters::DiffuseBounces)))
            path.setTerminateAtNextBounce();

        return path;
    }

    inline void StartPixel(const PathState path, const WorkingContext workingContext)
    {
#if PATH_TRACER_MODE != PATH_TRACER_MODE_REFERENCE || defined(__INTELLISENSE__)
        workingContext.DenoisingLayers.StartPixel(path.GetPixelPos());
#endif

#if PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE || PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        Bridge::ExportSurfaceInit(path.GetPixelPos());
#endif
    }

    inline void SetupPathPrimaryRay(inout PathState path, const Ray ray)
    {
        path.SetOrigin(ray.origin);
        path.SetDir(ray.dir);
    }

    inline void UpdatePathThroughput(inout PathState path, const float3 weight)
    {
        path.SetThp(path.GetThp() * weight);
    }

    inline bool ShouldCollectGISecondaryRadiance(const PathState path)
    {
        return false;
    }

    inline void UpdateSurfaceOutsideIoR(inout SurfaceData surfaceData, const float outsideIoR)
    {
        const float safeOutsideIoR  = max(outsideIoR, 1.0);
        surfaceData.shadingData.exteriorIoR = lpfloat(safeOutsideIoR);
        const float safeInteriorIoR = max(surfaceData.shadingData.interiorIoR, 1.0);
        surfaceData.bsdf.standardData.SetEta(surfaceData.shadingData.frontFacing ?
                                                 safeOutsideIoR / safeInteriorIoR :
                                                 safeInteriorIoR / safeOutsideIoR);
    }

    inline bool HandleNestedDielectrics(inout SurfaceData surfaceData,
                                        inout PathState path,
                                        const WorkingContext workingContext)
    {
#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0 || defined(__INTELLISENSE__)
        const bool thinSurface = surfaceData.shadingData.mtl.isThinSurface();
        const uint nestedPriority = surfaceData.shadingData.mtl.getNestedPriority();
        const float outsideIoR = ComputeOutsideIoR(path.interiorList,
                                                   surfaceData.shadingData.materialID,
                                                   surfaceData.shadingData.frontFacing);

        if (!thinSurface)
        {
            const bool trueIntersection = path.interiorList.isTrueIntersection(nestedPriority);

            if (!trueIntersection)
            {
                if (path.getCounter(PackedCounters::RejectedHits) < kMaxRejectedDielectricHits)
                {
                    path.incrementCounter(PackedCounters::RejectedHits);
                    path.interiorList.handleIntersection(surfaceData.shadingData.materialID,
                                                         nestedPriority,
                                                         surfaceData.shadingData.frontFacing);
                    path.SetOrigin(surfaceData.shadingData.computeNewRayOrigin(false));
                    path.decrementVertexIndex();
                    return false;
                }
#if !NESTED_DIELECTRICS_AVOID_TERMINATION
                else
                {
                    path.terminate();
                    return false;
                }
#endif
            }
        }

        UpdateSurfaceOutsideIoR(surfaceData, outsideIoR);
#endif
        return true;
    }

#if PATH_TRACER_MODE != PATH_TRACER_MODE_BUILD_DENOISING_LAYERS || defined(__INTELLISENSE__)
    inline BSDFSample MakeBSDFSample(uint lobe, float pdf, float lobeP, float3 weight, float3 wi)
    {
        BSDFSample bs;
        bs.lobe           = lobe;
        // Match upstream IBSDF::getDeltaLobeIndex(): a NON-delta lobe must map to the invalid sentinel
        // (0xFFFFFFFF), not 0. DenoisingLayersOnScatter feeds this into DenoisingLayersAdvanceBranchID on every
        // FILL scatter; for a non-delta bounce the advanced branchID must become cDenoisingLayerInvalidBranchID
        // so the path cleanly leaves the stable-plane branch. The previous value (0) produced a valid-looking
        // branchID on non-delta scatters, corrupting FILL branch tracking so transmission radiance was
        // committed onto a mis-tracked plane and dropped (black glass).
        bs.deltaLobeIndex = ((lobe & kBSDFLobeDelta) == 0u) ? 0xFFFFFFFFu :
            (((lobe & kBSDFLobeTransmission) == 0u) ? 1u : 0u);
        bs.pdf            = pdf;
        bs.lobeP          = lobeP;
        bs.weight         = weight;
        bs.wi             = wi;
        return bs;
    }

    inline void UpdateNestedDielectricsOnScatterTransmission(const ShadingData shadingData,
                                                             inout PathState path,
                                                             const WorkingContext workingContext)
    {
#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0 || defined(__INTELLISENSE__)
        if (!shadingData.mtl.isThinSurface())
        {
            const uint nestedPriority = shadingData.mtl.getNestedPriority();
            path.interiorList.handleIntersection(shadingData.materialID, nestedPriority, shadingData.frontFacing);
            path.setInsideDielectricVolume(!path.interiorList.isEmpty());
        }
#endif
    }

    inline NEEResult HandleNEE(const PathState preScatterPath,
                               const SurfaceData surfaceData,
                               inout SampleGenerator sgDirect,
                               const WorkingContext workingContext)
    {
        NEEResult result = NEEResult::empty();

        const bool enableNEE     = workingContext.PtConsts.NEEEnabled != 0u;
        const uint fullSamples   = RTXPT_NEE_FULL_SAMPLE_COUNT;
        if (!enableNEE || fullSamples == 0u)
            return result;
        if (!ReferenceShouldRunNEE(preScatterPath, workingContext))
            return result;
        const LightingControlData ctrl = Bridge::getLightingControl();
        if (ctrl.TotalLightCount == 0u || (ctrl.ImportanceSamplingType != 0u && ctrl.SamplingProxyCount == 0u) ||
            (FalcorBSDF::getLobes(surfaceData.bsdf.standardData) & kLobeTypeNonDelta) == 0u)
            return result;
        const bool coherent = IsScreenSpaceCoherent(ctrl, preScatterPath.rayCone.getWidth(), preScatterPath.GetSceneLength());

        result.BSDFMISInfo.LightSamplingEnabled = true;
        result.BSDFMISInfo.LightSamplingIsSSC   = coherent;
        result.BSDFMISInfo.CandidateSamples     = min(NEEBSDFMISInfo::SampleCountLimit(), RTXPT_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT);
        result.BSDFMISInfo.FullSamples          = min(NEEBSDFMISInfo::SampleCountLimit(), fullSamples);

        const float3 wo = surfaceData.shadingData.V;
        float specularRadianceAvg = 0.0;
        float3 directRadiance = SampleDirectLightNEE(surfaceData.bsdf.standardData,
                                                     surfaceData.shadingData.posW,
                                                     surfaceData.shadingData.faceNCorrected,
                                                     surfaceData.shadingData.vertexN,
                                                     surfaceData.shadingData.posErrorScale,
                                                     surfaceData.shadingData.shadowNoLFadeout,
                                                     wo,
                                                     preScatterPath.GetPixelPos(),
                                                     sgDirect,
                                                     preScatterPath.GetFireflyFilterK(),
                                                     coherent,
                                                     preScatterPath.GetThp(),
                                                     specularRadianceAvg);
        directRadiance *= preScatterPath.GetThp();

        if (any(directRadiance > 0.0))
        {
            result.AccumulateRadiance(directRadiance, specularRadianceAvg);
        }

        return result;
    }

    inline bool GenerateScatterRay(const BSDFSample bs,
                                   const SurfaceData surfaceData,
                                   inout PathState path,
                                   const WorkingContext workingContext)
    {
        const uint  lobe           = bs.lobe;
        const float pdf            = bs.pdf;
        const float lobeP          = bs.lobeP;
        const bool  isTransmission = (lobe & kBSDFLobeTransmission) != 0u;
        const float3 scatterOrigin = surfaceData.shadingData.computeNewRayOrigin(!isTransmission);

        path.clearScatterEventFlags();
        path.SetOrigin(scatterOrigin);
        path.SetDir(bs.wi);
        path.SetThp(path.GetThp() * bs.weight);
        path.SetFireflyFilterK_BsdfScatterPdf(ComputeNewScatterFireflyFilterK(path.GetFireflyFilterK(), pdf, lobeP), pdf);
        path.setScatterTransmission(isTransmission);
        path.setScatterSpecular((lobe & (kBSDFLobeSpecularReflection | kBSDFLobeSpecularTransmission | kBSDFLobeDeltaReflection | kBSDFLobeDeltaTransmission)) != 0u);
        path.setScatterDelta((lobe & kBSDFLobeDelta) != 0u);
        path.setDeltaOnlyPath(path.isDeltaOnlyPath() && ((lobe & kBSDFLobeDelta) != 0u));

        if (isTransmission)
            UpdateNestedDielectricsOnScatterTransmission(surfaceData.shadingData, path, workingContext);

        const bool isDiffuseBounce =
            ((lobe & (kBSDFLobeDiffuseReflection | kBSDFLobeDiffuseTransmission)) != 0u) ||
            surfaceData.bsdf.standardData.Roughness() > kSpecularRoughnessThreshold;
        if (isDiffuseBounce &&
            !(((lobe & kBSDFLobeDiffuseTransmission) != 0u) && ((path.getVertexIndex() % 2u) == 1u)))
            path.incrementCounter(PackedCounters::DiffuseBounces);

        if ((lobe & kBSDFLobeDelta) == 0u)
        {
            path.rayCone = RayCone::make(path.rayCone.getWidth(),
                                         min(path.rayCone.getSpreadAngle() +
                                                 ComputeRayConeSpreadAngleExpansionByScatterPDF(pdf),
                                             6.28318530717958647692));
        }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS || defined(__INTELLISENSE__)
        const bool onDominantDenoisingLayer = path.hasFlag(PathFlags::denoisingLayerOnDominantBranch);
        const bool isDiffuseForSpecHitT =
            ((lobe & (kBSDFLobeDiffuseReflection | kBSDFLobeDiffuseTransmission)) != 0u) ||
            surfaceData.bsdf.standardData.Roughness() > 0.35;
        if (onDominantDenoisingLayer && !isDiffuseForSpecHitT)
        {
            if (!surfaceData.shadingData.mtl.isPSDBlockMotionVectorsAtSurface())
            {
                path.setFlag(PathFlags::exportSpecHitTQueued, true);
                Bridge::ExportSpecHitTStart(path);
            }
        }
        else if (path.hasFlag(PathFlags::exportSpecHitTQueued))
        {
            const bool hasNonDeltaLobes =
                (surfaceData.bsdf.standardData.DiffuseTransmission() > 0.0) ||
                any(surfaceData.bsdf.standardData.Diffuse() > 0.0) ||
                surfaceData.bsdf.standardData.Roughness() > 0.0;
            const int maxHitTSpecBounces = 4;
            if (hasNonDeltaLobes || path.getCounter(PackedCounters::BouncesFromDenoisingLayer) > maxHitTSpecBounces)
            {
                Bridge::ExportSpecHitTStop(path);
                path.setFlag(PathFlags::exportSpecHitTQueued, false);
            }
        }

        DenoisingLayersOnScatter(path, bs, workingContext);
#endif
        path.setFlag(PathFlags::enableThreadReorder, true);

        return true;
    }

    inline bool GenerateScatterRay(const SurfaceData surfaceData,
                                   inout PathState path,
                                   const WorkingContext workingContext,
                                   out BSDFSample bs)
    {
        bs = MakeBSDFSample(0u, 0.0, 0.0, 0.xxx, 0.xxx);

        const float3 wo = surfaceData.shadingData.V;
        const SampleGeneratorVertexBase sgBase = SampleGeneratorVertexBase::make(path.GetPixelPos(), path.getVertexIndex(), Bridge::getSampleIndex());
        const uint diffuseBounces = path.getCounter(PackedCounters::DiffuseBounces);
        float3 preGeneratedSamples;

#if RTXPT_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF
        [branch]
        if (diffuseBounces < workingContext.PtConsts.diffuseBounceCount)
            preGeneratedSamples = SampleSequenceGenerator::Generate(3u, sgBase, kSampleEffect_ScatterBSDF).xyz;
        else
#endif
            preGeneratedSamples = UniformSampleSequenceGenerator::Generate(3u, sgBase, kSampleEffect_ScatterBSDF).xyz;

        float3 wi;
        float3 weight;
        float  pdf;
        uint   lobe;
        float  lobeP;
        if (!SampleBSDF(surfaceData.bsdf.standardData, wo, preGeneratedSamples, wi, weight, pdf, lobe, lobeP))
            return false;

        bs = MakeBSDFSample(lobe, pdf, lobeP, weight, wi);
        return GenerateScatterRay(bs, surfaceData, path, workingContext);
    }
#endif

    inline void AccumulatePathRadiance(const WorkingContext workingContext,
                                       inout PathState path,
                                       float3 radiance,
                                       const float specularRadianceAvg,
                                       bool denoisingLayerOnBranch,
                                       bool collectGISecondaryRadiance)
    {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        workingContext.DenoisingLayers.AccumulateStableRadiance(path.GetPixelPos(), radiance);
#elif PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE
        float4 newL = path.GetL();
        newL.rgb += radiance;
        path.SetL(newL);
#elif PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS
        if (!denoisingLayerOnBranch)
        {
            float4 newL = float4(radiance, specularRadianceAvg) * Bridge::getNoisyRadianceAttenuation();
            path.SetL(path.GetL() + newL);
        }
#else
#    error Unsupported PATH_TRACER_MODE.
#endif
    }

    inline void AccumulateNEERadiance(const WorkingContext workingContext,
                                      inout PathState path,
                                      const PathState preScatterPath,
                                      const NEEResult neeResult)
    {
        float4 neeRadianceAndSpecAvg = neeResult.GetRadianceAndSpecAvg();
        if (any(neeRadianceAndSpecAvg > 0.0))
        {
            const int bouncesFromDenoisingLayer = preScatterPath.getCounter(PackedCounters::BouncesFromDenoisingLayer) + 1;
            float specRadianceAvg = 0.0;
            if (!preScatterPath.hasFlag(PathFlags::denoisingLayerBaseScatterDiff))
            {
                const bool pathIsDeltaOnlyPath = preScatterPath.isDeltaOnlyPath();
                const bool specialCondition = (bouncesFromDenoisingLayer == 1) || (pathIsDeltaOnlyPath && bouncesFromDenoisingLayer <= 3);
                specRadianceAvg = specialCondition ? neeRadianceAndSpecAvg.w : Average(neeRadianceAndSpecAvg.rgb);
            }

            AccumulatePathRadiance(workingContext,
                                   path,
                                   neeRadianceAndSpecAvg.rgb,
                                   specRadianceAvg,
                                   false,
                                   ShouldCollectGISecondaryRadiance(preScatterPath));
        }
    }

    inline void CommitPixel(const PathState path, const WorkingContext workingContext)
    {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        // Stable radiance and stable-plane data are committed incrementally while tracing.
#elif PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE
        workingContext.OutputColor[path.GetPixelPos()] = float4(path.GetL().rgb, 1.0);
#elif PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS
        workingContext.DenoisingLayers.CommitDenoiserRadiance(path);
#else
#    error Unsupported PATH_TRACER_MODE.
#endif
    }

    inline bool HandleRussianRoulette(inout PathState path,
                                      const PathState preScatterPath,
                                      const WorkingContext workingContext)
    {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE
        if (preScatterPath.getVertexIndex() <= workingContext.PtConsts.minBounceCount)
            return true;

        SampleGenerator sgRR = SampleGenerator_makeStateless(preScatterPath.GetPixelPos(),
                                                             preScatterPath.getVertexIndex(),
                                                             Bridge::getSampleIndex(),
                                                             kSampleEffect_RussianRoulette);
        const float3 thp     = path.GetThp();
        const float  survive = clamp(max(thp.x, max(thp.y, thp.z)), 0.05, 1.0);
        if (sampleNext1D(sgRR) > survive)
        {
            path.terminate();
            return false;
        }

        path.SetThp(thp / survive);
#endif
        return true;
    }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE
    inline void UpdatePathTravelledLengthOnly(inout PathState path, const float rayTCurrent)
    {
        const float rayConeSpreadAngle = path.rayCone.getSpreadAngle();
        path.rayCone = RayCone::make(path.rayCone.getWidth() + rayConeSpreadAngle * rayTCurrent,
                                     rayConeSpreadAngle);
        path.SetSceneLength(min(path.GetSceneLength() + rayTCurrent, kMaxRayTravel));
    }
#endif

    inline void UpdatePathTravelled(inout PathState path,
                                    const float3 rayOrigin,
                                    const float3 rayDir,
                                    const float rayTCurrent,
                                    const WorkingContext workingContext)
    {
        path.incrementVertexIndex();
        UpdatePathTravelledLengthOnly(path, rayTCurrent);
    }

    inline void HandleMiss(inout PathState path,
                           const float3 rayOrigin,
                           const float3 rayDir,
                           const float rayTCurrent,
                           const WorkingContext workingContext)
    {
        UpdatePathTravelled(path, rayOrigin, rayDir, rayTCurrent, workingContext);

#if PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS
        if (path.hasFlag(PathFlags::exportSpecHitTQueued))
        {
            Bridge::ExportSpecHitTStop(path);
            path.setFlag(PathFlags::exportSpecHitTQueued, false);
        }
#endif

        EnvMapSampler envSampler = RTXPTCreateEnvMapSampler(Bridge::getEnvMapConstants());
        // RTXPT-fork defaults to MIP 2 after more than one diffuse bounce.
        const float environmentMip = path.getCounter(PackedCounters::DiffuseBounces) > 1u ? 2.0 : 0.0;
        float3 environmentEmission = envSampler.Eval(rayDir, environmentMip);

#if PATH_TRACER_MODE != PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        const NEEBSDFMISInfo prevMISInfo = NEEBSDFMISInfo::Unpack16bit(path.GetPackedMISInfo());
        const bool didEnvNEE =
            prevMISInfo.LightSamplingEnabled &&
            ((workingContext.PtConsts.environmentNEEEnabled & 1u) != 0u) && Bridge::getLightingControl().EnvmapQuadNodeCount > 0u;
        if (didEnvNEE && path.GetBsdfScatterPdf() > 0.0)
        {
            uint lightIndex = RTXPT_INVALID_LIGHT_INDEX;
            float solidAnglePdf = 0.0;
            if (!UseApproximateLightMIS())
            {
                uint width, height;
                t_EnvironmentLightLookup.GetDimensions(width, height);
                const uint2 texel = min(uint2(RTXPTDirToOctEqualArea(envSampler.ToLocal(rayDir)) * float2(width, height)), uint2(width, height) - 1u);
                lightIndex = t_EnvironmentLightLookup.Load(int3(texel, 0));
                solidAnglePdf = EnvironmentQuadSolidAnglePdf(Bridge::getEnvironmentLight(lightIndex));
            }
            environmentEmission *= ComputeLightVsBSDFMISForBSDF(path.GetPixelPos(), lightIndex, path.GetBsdfScatterPdf(),
                solidAnglePdf, prevMISInfo.CandidateSamples, prevMISInfo.FullSamples, prevMISInfo.LightSamplingIsSSC);
        }
        environmentEmission = ApplyEmissionFireflyFilter(environmentEmission, path, workingContext);
#endif

#if PATH_TRACER_MODE != PATH_TRACER_MODE_REFERENCE || defined(__INTELLISENSE__)
        DenoisingLayersHandleMiss(path, environmentEmission, rayOrigin, rayDir, rayTCurrent, workingContext);
#else
#if !RTXPT_SHARC_TRAIN
        Bridge::ExportNonSurface(path, rayOrigin + rayDir * rayTCurrent, float3(0.0, 0.0, 0.0));
#endif
#endif

#if RTXPT_SHARC_TRAIN
        SharcThroughputCheckpoint(path);
        SharcBackPropagate(environmentEmission);
#endif
        if (any(environmentEmission > 0.0))
        {
            const float3 radiance = path.GetThp() * environmentEmission;
            const float specRadianceAvg =
                path.hasFlag(PathFlags::denoisingLayerBaseScatterDiff) ? 0.0 : Average(radiance);
            AccumulatePathRadiance(workingContext,
                                   path,
                                   radiance,
                                   specRadianceAvg,
                                   path.hasFlag(PathFlags::denoisingLayerOnBranch),
                                   ShouldCollectGISecondaryRadiance(path));
        }

        path.clearHit();
        path.terminate();
    }

} // namespace PathTracer

#include "PathTracerHandleHit.hlsli"

#endif // __PATH_TRACER_HLSLI__
