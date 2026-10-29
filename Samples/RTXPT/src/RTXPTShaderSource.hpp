#pragma once

#include "EngineFactory.h"
#include "RefCntAutoPtr.hpp"

namespace Diligent
{
//A host supplies its deployed runtime shaders independently of scene assets. Initialization is synchronous;
//nested scopes restore the previous source, and no process working directory is changed. The Sample uses its
//existing search paths when no host source is supplied. Passes retain the source through shader creation.
inline thread_local IShaderSourceInputStreamFactory* g_RTXPTHostShaderSource = nullptr;

class RTXPTShaderSourceScope
{
public:
    explicit RTXPTShaderSourceScope(IShaderSourceInputStreamFactory* Source) :
        m_Previous(g_RTXPTHostShaderSource), m_Source(Source)
    {
        g_RTXPTHostShaderSource = Source;
    }
    ~RTXPTShaderSourceScope() { g_RTXPTHostShaderSource = m_Previous; }
    RTXPTShaderSourceScope(const RTXPTShaderSourceScope&) = delete;
    RTXPTShaderSourceScope& operator=(const RTXPTShaderSourceScope&) = delete;

private:
    IShaderSourceInputStreamFactory*               m_Previous;
    RefCntAutoPtr<IShaderSourceInputStreamFactory> m_Source;
};

inline void CreateRTXPTShaderSourceFactory(IEngineFactory* Factory, const char* DefaultPaths, IShaderSourceInputStreamFactory** Source)
{
    if (g_RTXPTHostShaderSource)
    {
        g_RTXPTHostShaderSource->AddRef();
        *Source = g_RTXPTHostShaderSource;
    }
    else
        Factory->CreateDefaultShaderSourceStreamFactory(DefaultPaths, Source);
}
} // namespace Diligent
