/*
 * The software backend's surfaces; see SoftwareSurface.h.
 */
#include "SoftwareSurface.h"

#include <LibCore/IOSurface.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>

#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>

namespace Karouka {

NonnullRefPtr<Gfx::PaintingSurface> create_software_surface(Gfx::SharedImageBuffer& buffer)
{
    // The bitmap wraps the IOSurface's base address, BGRA8 premultiplied at its bytes per row: what
    // Ladybird's WebContent paints into on the CPU when it has no GPU context.
    return Gfx::PaintingSurface::wrap_bitmap(*buffer.bitmap());
}

IOSurfaceCPUAccess::IOSurfaceCPUAccess(Core::IOSurfaceHandle const* handle, bool read_only)
    : m_options(read_only ? kIOSurfaceLockReadOnly : 0)
{
    if (!handle)
        return;
    auto* surface = static_cast<IOSurfaceRef>(handle->core_foundation_pointer());
    if (surface && IOSurfaceLock(surface, m_options, nullptr) == kIOReturnSuccess)
        m_surface = surface;
}

void const* IOSurfaceCPUAccess::base_address() const
{
    return m_surface ? IOSurfaceGetBaseAddress(static_cast<IOSurfaceRef>(m_surface)) : nullptr;
}

size_t IOSurfaceCPUAccess::bytes_per_row() const
{
    return m_surface ? IOSurfaceGetBytesPerRow(static_cast<IOSurfaceRef>(m_surface)) : 0;
}

IOSurfaceCPUAccess::~IOSurfaceCPUAccess()
{
    if (m_surface)
        IOSurfaceUnlock(static_cast<IOSurfaceRef>(m_surface), m_options, nullptr);
}

}
