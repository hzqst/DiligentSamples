#ifndef __PATH_TRACER_HANDLE_HIT_HLSLI__
#define __PATH_TRACER_HANDLE_HIT_HLSLI__

namespace PathTracer
{
    inline void HandleHit(inout PathState path,
                          SurfaceData surfaceData,
                          const float3 surfaceEmission,
                          const float3 rayOrigin,
                          const float3 rayDir,
                          const float rayTCurrent,
                          const WorkingContext workingContext)
    {
        UpdatePathTravelled(path, rayOrigin, rayDir, rayTCurrent, workingContext);

        float volumeAbsorption = 0.0;
#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0 || defined(__INTELLISENSE__)
        if (!path.interiorList.isEmpty())
        {
            const HomogeneousVolumeData volume = Bridge::loadHomogeneousVolumeData(path.interiorList.getTopMaterialID());
            const float3 transmittance = HomogeneousVolumeSampler::evalTransmittance(volume, rayTCurrent);
            volumeAbsorption = 1.0 - luminance(transmittance);
            UpdatePathThroughput(path, transmittance);
        }
#endif

#if RTXPT_SHARC_TRAIN
        SharcThroughputCheckpoint(path);
#endif
        const bool rejectedFalseHit = !HandleNestedDielectrics(surfaceData, path, workingContext);
        if (rejectedFalseHit)
        {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
            if (path.isTerminated())
                DenoisingLayersHandleMiss(path, 0.0.xxx, rayOrigin, rayDir, rayTCurrent, workingContext);
#endif
            return;
        }

        float3 referenceSurfaceEmission = surfaceEmission;
#if PATH_TRACER_MODE != PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
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
#endif

#if RTXPT_SHARC_TRAIN || RTXPT_SHARC_CONSUME
        // The separately stored emission must include the incoming path's MIS weight.
        SharcHitData sharcHit = SharcPrepareHit(surfaceData, referenceSurfaceEmission);
#endif
#if RTXPT_SHARC_CONSUME
        float3 cachedRadiance;
        if (SharcQueryHit(sharcHit, surfaceData, rayTCurrent, path, cachedRadiance))
        {
            const float3 radiance = path.GetThp() * cachedRadiance;
            AccumulatePathRadiance(workingContext, path, radiance,
                path.hasFlag(PathFlags::denoisingLayerBaseScatterDiff) ? 0.0 : Average(radiance), false,
                ShouldCollectGISecondaryRadiance(path));
            if (path.hasFlag(PathFlags::exportSpecHitTQueued))
                Bridge::ExportSpecHitTStop(path);
            path.terminate();
            return;
        }
#endif

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
#if PATH_TRACER_MODE == PATH_TRACER_MODE_REFERENCE && !RTXPT_SHARC_TRAIN
        Bridge::ExportSurface(path, surfaceData, path.GetSceneLength(), float3(0.0, 0.0, 0.0));
#endif
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        DenoisingLayersHandleHit(path,
                              rayOrigin,
                              rayDir,
                              rayTCurrent,
                              workingContext,
                              surfaceData,
                              volumeAbsorption,
                              surfaceEmission,
                              pathStopping);
#endif

        if (pathStopping || !path.isActive())
        {
#if RTXPT_SHARC_TRAIN
            SharcBackPropagate(referenceSurfaceEmission);
#endif
#if PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS
            if (pathStopping && path.isActive())
            {
                const PathState preScatterPath = path;
                SampleGenerator sgDirect = SampleGenerator_makeStateless(preScatterPath.GetPixelPos(),
                                                                         preScatterPath.getVertexIndex(),
                                                                         Bridge::getSampleIndex(),
                                                                         kSampleEffect_NEELightSampler);
                const NEEResult neeResult = HandleNEE(preScatterPath, surfaceData, sgDirect, workingContext);
                AccumulateNEERadiance(workingContext, path, preScatterPath, neeResult);
            }
#endif
#if PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS
            if (path.hasFlag(PathFlags::exportSpecHitTQueued))
            {
                Bridge::ExportSpecHitTStop(path);
                path.setFlag(PathFlags::exportSpecHitTQueued, false);
            }
#endif
            path.terminate();
            return;
        }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        return;
#else
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

#if RTXPT_SHARC_TRAIN
        SharcState sharcState = SharcLoadState();
        float random = Hash32ToFloat(Hash32Combine(path.GetId(), Bridge::getSampleIndex() + path.getVertexIndex() * 0x0366FE2F));
        bool keepTraining = SharcUpdateHit(GetSharcParameters(), sharcState, sharcHit, neeResult.GetRadianceAndSpecAvg().xyz, random);
        SharcSetThroughput(sharcState, path.GetThp());
        SharcStoreState(sharcState);
        path.SetThp(1.0.xxx);
        if (!keepTraining) { path.terminate(); return; }
#endif

        if (!scatterValid)
        {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS
            if (path.hasFlag(PathFlags::exportSpecHitTQueued))
            {
                Bridge::ExportSpecHitTStop(path);
                path.setFlag(PathFlags::exportSpecHitTQueued, false);
            }
#endif
            path.terminate();
            return;
        }

        const bool shouldTerminate = HasFinishedSurfaceBounces(path.getVertexIndex() + 1,
                                                               path.getCounter(PackedCounters::DiffuseBounces));
        if (shouldTerminate)
        {
            path.setTerminateAtNextBounce();
        }
#if !RTXPT_SHARC_TRAIN
        else if (!HandleRussianRoulette(path, preScatterPath, workingContext))
        {
            return;
        }
#endif
#endif
    }
}
#endif
