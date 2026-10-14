#include "TextBlobCache.h"

#include <gpu/ganesh/GrContextThreadSafeProxy.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <src/base/SkSpinlock.h>
#include <src/gpu/ganesh/GrContextThreadSafeProxyPriv.h>
#include <src/text/gpu/TextBlobRedrawCoordinator.h>

// TextBlobRedrawCoordinator's friend for Skia's tests (tests/TextBlobCacheTest.cpp, which isn't
// built into libskia): the way to its budget, which it keeps private. Only inline code: libskia
// exports none of TextBlobRedrawCoordinator's functions.
class GrTextBlobTestingPeer {
public:
    static void set_budget(sktext::gpu::TextBlobRedrawCoordinator& cache, size_t bytes)
    {
        SkAutoSpinlock lock { cache.fSpinLock };
        cache.fSizeBudget = bytes;
    }

    static size_t used_bytes(sktext::gpu::TextBlobRedrawCoordinator& cache)
    {
        SkAutoSpinlock lock { cache.fSpinLock };
        return cache.fCurrentSize;
    }
};

namespace Karouka {

// The cache belongs to the context's thread-safe proxy (GrContextThreadSafeProxy::priv(), inline;
// GrRecordingContext's own getter is protected and not exported).
static sktext::gpu::TextBlobRedrawCoordinator& text_blob_cache(GrDirectContext& context)
{
    return *context.threadSafeProxy()->priv().getTextBlobRedrawCoordinator();
}

void set_text_blob_cache_budget(GrDirectContext& context, size_t bytes)
{
    GrTextBlobTestingPeer::set_budget(text_blob_cache(context), bytes);
}

size_t text_blob_cache_used_bytes(GrDirectContext& context)
{
    return GrTextBlobTestingPeer::used_bytes(text_blob_cache(context));
}

}
