// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "DeviceContext.h"
#include "Query.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#if D3D12_SUPPORTED
#    include "../../../../DiligentCore/Platforms/Win32/interface/WinHPreface.h"
#    include <d3d12.h>
#    include "../../../../DiligentCore/Platforms/Win32/interface/WinHPostface.h"
#    include "CommandQueueD3D12.h"
#endif

namespace Diligent
{

// Timestamp pairs, not duration queries: NGX must not execute inside an active
// query. Completed frames are read asynchronously; a full ring drops a sample.
class RTXPTGpuProfiler
{
public:
    enum class Stage : Uint32
    {
        Total,
        PrefixEarly,
        Build,
        LightsBaker,
        SharcUpdate,
        SharcResolve,
        SubSampling,
        Fill,
        Guides,
        RRPrepare,
        RR,
        PostProcess,
        Merge,
        ToneMapping,
        ScenePrepare,
        SkinAndBLAS,
        TLAS,
        HostComposite,
        //RealtimeNative: every layer's NRD prepare, dispatch and merge.
        Denoise,
        Count
    };

    class Scope
    {
    public:
        Scope(RTXPTGpuProfiler* Profiler, IDeviceContext* Context, Stage Pass) :
            m_Profiler{Profiler}, m_Context{Context}, m_Stage{Pass}
        {
            if (m_Profiler)
                m_Profiler->Begin(m_Context, m_Stage);
        }
        ~Scope()
        {
            if (m_Profiler)
                m_Profiler->End(m_Context, m_Stage);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        RTXPTGpuProfiler* m_Profiler;
        IDeviceContext*   m_Context;
        Stage             m_Stage;
    };

    static constexpr size_t StageCount = static_cast<size_t>(Stage::Count);
    struct Result
    {
        Uint64 FrameNumber = 0;
        Uint64 SourceFrame = 0;
        //Negative means the stage was not measured, never a zero-cost stage.
        std::array<double, StageCount> Milliseconds;
        Result() { Milliseconds.fill(-1); }
    };
    const Result&      GetLatest() const { return m_Latest; }
    bool               IsAvailable() const { return m_Available; }
    Uint64             GetDroppedCount() const { return m_Dropped; }
    static const char* GetStageName(size_t Index) { return kNames.at(Index); }
    void               Poll() { Collect(); }

    Scope BeginFrame(IRenderDevice* Device, IDeviceContext* Context, bool Enabled, const std::string& CSVPath, Uint64 SourceFrame = 0)
    {
        return Scope{StartFrame(Device, Enabled, CSVPath, SourceFrame) ? this : nullptr, Context, Stage::Total};
    }

    //For hosts whose begin/end timestamps are separate commands. Pair a successful StartFrame with Begin/End(Total).
    bool StartFrame(IRenderDevice* Device, bool Enabled, const std::string& CSVPath, Uint64 SourceFrame = 0)
    {
        if (!Enabled)
            return false;
        if (!m_Initialized)
            Initialize(Device, CSVPath);
        if (!m_Available)
            return false;
        Collect();
        ++m_FrameNumber;
        for (Uint32 I = 0; I < kBufferedFrames; ++I)
        {
            Frame& Sample = m_Frames[I];
            if (!Sample.Pending)
            {
                m_Current          = static_cast<int>(I);
                Sample.Number      = m_FrameNumber;
                Sample.SourceFrame = SourceFrame;
                Sample.Ended.fill(false);
                return true;
            }
        }
        ++m_Dropped;
        return false;
    }

    void Begin(IDeviceContext* Context, Stage Pass)
    {
        if (m_Current >= 0)
        {
            auto&      Frame          = m_Frames[m_Current];
            const auto Index          = static_cast<size_t>(Pass);
            Frame.ContextIds[Index]   = Context->GetDesc().ContextId;
            Frame.ClockOffsets[Index] = 0;
#if D3D12_SUPPORTED
            if (m_D3D12)
            {
                auto*      Queue = static_cast<ICommandQueueD3D12*>(Context->LockCommandQueue());
                Uint64     GPU = 0, CPU = 0, Frequency = 0;
                auto*      Native = Queue->GetD3D12CommandQueue();
                const bool Valid  = SUCCEEDED(Native->GetTimestampFrequency(&Frequency)) &&
                    SUCCEEDED(Native->GetClockCalibration(&GPU, &CPU)) && Frequency != 0;
                Context->UnlockCommandQueue();
                LARGE_INTEGER QPCFrequency;
                QueryPerformanceFrequency(&QPCFrequency);
                Frame.ClockOffsets[Index] = Valid ? static_cast<double>(CPU) / QPCFrequency.QuadPart -
                        static_cast<double>(GPU) / Frequency :
                                                    -1;
            }
#endif
            Context->EndQuery(m_Frames[m_Current].Queries[static_cast<size_t>(Pass) * 2]);
        }
    }

    void End(IDeviceContext* Context, Stage Pass)
    {
        if (m_Current < 0)
            return;
        Frame&       Sample = m_Frames[m_Current];
        const size_t Index  = static_cast<size_t>(Pass);
        Context->EndQuery(Sample.Queries[Index * 2 + 1]);
        Sample.Ended[Index] = true;
        if (Pass == Stage::Total)
        {
            Sample.Pending = true;
            m_Current      = -1;
        }
    }

private:
    static constexpr size_t                               kStageCount     = static_cast<size_t>(Stage::Count);
    static constexpr Uint32                               kBufferedFrames = 8;
    static constexpr Uint32                               kReportFrames   = 120;
    static constexpr std::array<const char*, kStageCount> kNames          = {
        "Total", "PrefixEarly", "BUILD", "LightsBaker", "SHaRCUpdate", "SHaRCResolve", "SubSampling", "FILL", "Guides", "RRPrepare", "RR", "PostProcess", "Merge", "ToneMapping", "ScenePrepare", "SkinAndBLAS", "TLAS", "HostComposite", "NRD"};
    struct Frame
    {
        std::array<RefCntAutoPtr<IQuery>, kStageCount * 2> Queries;
        std::array<bool, kStageCount>                      Ended{};
        std::array<Uint32, kStageCount>                    ContextIds{};
        std::array<double, kStageCount>                    ClockOffsets{};
        Uint64                                             Number      = 0;
        Uint64                                             SourceFrame = 0;
        bool                                               Pending     = false;
    };

    void Initialize(IRenderDevice* Device, const std::string& CSVPath)
    {
        m_Initialized = true;
        m_D3D12       = Device->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D12;
        if (!Device->GetDeviceInfo().Features.TimestampQueries)
        {
            std::cerr << "RTXPT GPU profiling unavailable: timestamp queries are disabled\n";
            return;
        }
        QueryDesc Desc;
        Desc.Type = QUERY_TYPE_TIMESTAMP;
        Desc.Name = "RTXPT GPU stage timestamp";
        for (Frame& Sample : m_Frames)
            for (auto& Query : Sample.Queries)
            {
                Device->CreateQuery(Desc, &Query);
                if (!Query)
                    return;
            }
        if (!CSVPath.empty())
        {
            m_CSV.open(CSVPath);
            if (!m_CSV)
                std::cerr << "Cannot open RTXPT GPU timing CSV: " << CSVPath << '\n';
            else
            {
                m_CSV << "Frame";
                for (const char* Name : kNames)
                    m_CSV << ',' << Name << "_ms";
                for (const char* Name : kNames)
                    m_CSV << ',' << Name << "_queue," << Name << "_begin_s," << Name << "_end_s";
                m_CSV << '\n'
                      << std::fixed << std::setprecision(9);
            }
        }
        m_Available = true;
    }

    void Collect()
    {
        for (Frame& Sample : m_Frames)
        {
            if (!Sample.Pending)
                continue;
            bool Ready = true;
            for (size_t I = 0; I < kStageCount; ++I)
                if (Sample.Ended[I])
                    Ready &= Sample.Queries[I * 2]->GetData(nullptr, 0) && Sample.Queries[I * 2 + 1]->GetData(nullptr, 0);
            if (!Ready)
                continue;
            if (m_CSV)
                m_CSV << Sample.Number;
            std::array<double, kStageCount> Starts{}, Stops{};
            Result                          Completed;
            Completed.FrameNumber = Sample.Number;
            Completed.SourceFrame = Sample.SourceFrame;
            Starts.fill(-1);
            Stops.fill(-1);
            for (size_t I = 0; I < kStageCount; ++I)
            {
                double Milliseconds = -1;
                if (Sample.Ended[I])
                {
                    QueryDataTimestamp Start, Stop;
                    Sample.Queries[I * 2]->GetData(&Start, sizeof(Start));
                    Sample.Queries[I * 2 + 1]->GetData(&Stop, sizeof(Stop));
                    if (Start.Frequency != 0 && Start.Frequency == Stop.Frequency && Stop.Counter >= Start.Counter)
                    {
                        Milliseconds = 1000.0 * static_cast<double>(Stop.Counter - Start.Counter) / static_cast<double>(Start.Frequency);
                        if (Sample.ClockOffsets[I] != -1)
                        {
                            Starts[I] = static_cast<double>(Start.Counter) / Start.Frequency + Sample.ClockOffsets[I];
                            Stops[I]  = static_cast<double>(Stop.Counter) / Stop.Frequency + Sample.ClockOffsets[I];
                        }
                    }
                }
                if (m_CSV)
                    m_CSV << ',' << Milliseconds;
                Completed.Milliseconds[I] = Milliseconds;
                if (Milliseconds >= 0)
                {
                    m_Sum[I] += Milliseconds;
                    ++m_Count[I];
                }
            }
            if (m_CSV)
            {
                for (size_t I = 0; I < kStageCount; ++I)
                    m_CSV << ',' << Sample.ContextIds[I] << ',' << Starts[I] << ',' << Stops[I];
                m_CSV << '\n';
            }
            Sample.Pending = false;
            //Ring slots may complete out of order. Never replace a newer result with an older frame.
            if (Completed.FrameNumber > m_Latest.FrameNumber)
                m_Latest = Completed;
            if (++m_Collected % kReportFrames == 0)
            {
                std::cout << "RTXPT GPU ms (" << kReportFrames << " frames):";
                for (size_t I = 0; I < kStageCount; ++I)
                    if (m_Count[I] != 0)
                        std::cout << ' ' << kNames[I] << '=' << m_Sum[I] / m_Count[I];
                std::cout << " dropped=" << m_Dropped << std::endl;
                m_Sum.fill(0);
                m_Count.fill(0);
                if (m_CSV)
                    m_CSV.flush();
            }
        }
    }

    std::array<Frame, kBufferedFrames> m_Frames;
    std::array<double, kStageCount>    m_Sum{};
    std::array<Uint32, kStageCount>    m_Count{};
    std::ofstream                      m_CSV;
    Uint64                             m_FrameNumber = 0;
    Uint64                             m_Dropped     = 0;
    Uint64                             m_Collected   = 0;
    int                                m_Current     = -1;
    bool                               m_Initialized = false;
    bool                               m_Available   = false;
    bool                               m_D3D12       = false;
    Result                             m_Latest;
};

} // namespace Diligent
