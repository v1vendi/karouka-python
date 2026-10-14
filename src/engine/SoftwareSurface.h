/*
 * The software backend (kr_system_desc.backend = KR_BACKEND_SOFTWARE): a view rasterizes
 * with Skia's CPU rasterizer straight into its IOSurface's memory, with no GPU context at all.
 */
#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/kmalloc.h>

namespace Core {
class IOSurfaceHandle;
}

namespace Gfx {
class PaintingSurface;
class SharedImageBuffer;
}

namespace Karouka {

// A raster painting surface over the buffer's IOSurface pixels (the bitmap LibGfx maps over them),
// which keeps that mapping alive while it lives.
NonnullRefPtr<Gfx::PaintingSurface> create_software_surface(Gfx::SharedImageBuffer&);

// Holds the IOSurface's lock while the CPU writes (or, read_only, reads) its pixels, so that a host
// sampling the surface on the GPU sees them (IOSurfaceLock). Released when it goes. A null handle
// (a view on the GPU) locks nothing. Also what kr_view_lock_pixels holds, on either backend.
class IOSurfaceCPUAccess {
    AK_ALLOC_WITH_KMALLOC;

public:
    IOSurfaceCPUAccess(Core::IOSurfaceHandle const*, bool read_only);
    ~IOSurfaceCPUAccess();
    IOSurfaceCPUAccess(IOSurfaceCPUAccess const&) = delete;
    IOSurfaceCPUAccess& operator=(IOSurfaceCPUAccess const&) = delete;

    // Whether the lock was taken (false for a null handle or a failed IOSurfaceLock).
    bool is_locked() const { return m_surface; }
    // The locked surface's first pixel and row pitch; null and 0 when not locked.
    void const* base_address() const;
    size_t bytes_per_row() const;

private:
    void* m_surface { nullptr };
    unsigned m_options { 0 };
};

}
