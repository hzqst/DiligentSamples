#ifndef RTXPT_DENOISING_HIT_DISTANCE_HLSLI
#define RTXPT_DENOISING_HIT_DISTANCE_HLSLI

static const uint kDenoisingHitDistanceDiffuse = 1u;
static const uint kDenoisingHitDistanceSpecular = 2u;
static const uint kDenoisingHitDistanceDiffuseHit = 1u;
static const uint kDenoisingHitDistanceDiffuseMiss = 2u;
static const uint kDenoisingHitDistanceSpecularHit = 4u;
static const uint kDenoisingHitDistanceSpecularMiss = 8u;
static const uint kDenoisingHitDistanceInvalid = 16u;
// Both NRD inputs are stored in RGBA16F. Infinity would be sanitized back to missing data.
static const float kDenoisingHitDistanceFar = 65504.0;

struct DenoisingHitDistance
{
    float DiffuseDistance;
    float SpecularDistance;
    uint DiffuseSamples;
    uint SpecularSamples;
    float PendingDistance;
    uint PendingLobe;
    uint Flags;
    uint Padding;

#if !defined(__cplusplus)
    void Begin(uint lobe)
    {
        PendingDistance = 0.0;
        PendingLobe = lobe;
    }

    void AddSegment(float distance)
    {
        if (PendingLobe == 0u)
            return;
        const float accumulatedDistance = PendingDistance + distance;
        if (!isfinite(distance) || distance < 0.0 || !isfinite(accumulatedDistance))
        {
            Flags |= kDenoisingHitDistanceInvalid;
            Begin(0u);
            return;
        }
        // Keep finite geometry distances in FP32: REBLUR normalizes before packing,
        // while RELAX's frontend clamps its raw distance to the FP16 output range.
        PendingDistance = accumulatedDistance;
    }

    void Complete(bool environmentMiss)
    {
        if (PendingLobe == 0u)
            return;
        const float distance = environmentMiss ? kDenoisingHitDistanceFar : PendingDistance;
        // Zero remains missing/invalid; do not fabricate a positive contact distance.
        if (distance > 0.0)
        {
            if (PendingLobe == kDenoisingHitDistanceDiffuse)
            {
                ++DiffuseSamples;
                DiffuseDistance += (distance - DiffuseDistance) / float(DiffuseSamples);
                Flags |= environmentMiss ? kDenoisingHitDistanceDiffuseMiss : kDenoisingHitDistanceDiffuseHit;
            }
            else
            {
                // NRD 4.17 SpecHitDistAveraging selects the minimum nonzero sample,
                // independently of radiance, PDF and the number of skipped lobes.
                SpecularDistance = SpecularSamples == 0u ? distance : min(SpecularDistance, distance);
                ++SpecularSamples;
                Flags |= environmentMiss ? kDenoisingHitDistanceSpecularMiss : kDenoisingHitDistanceSpecularHit;
            }
        }
        Begin(0u);
    }
#endif
};

#endif
