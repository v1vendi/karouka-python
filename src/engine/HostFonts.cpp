/*
 * The host's font registration: kr_system_add_font2, kr_system_add_font_file2 and
 * kr_system_set_default_font. Faces go to SystemUIFontProvider, the process-wide system font
 * provider; see karouka.h for the contract and SystemUIFontProvider.h for how they are matched.
 */

#include "HostFonts.h"
#include "HostInterface.h"
#include "StructSize.h"
#include "SystemUIFontProvider.h"

#include <AK/LexicalPath.h>
#include <LibCore/MappedFile.h>
#include <LibCore/Platform/ScopedAutoreleasePool.h>
#include <LibCore/System.h>
#include <LibGfx/Font/SystemFallbackFonts.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Karouka {

static kr_system* s_system;
static SystemUIFontProvider* s_provider;

void install_host_fonts(kr_system* system, SystemUIFontProvider& provider)
{
    s_system = system;
    s_provider = &provider;
}

void uninstall_host_fonts(kr_system* system)
{
    if (s_system == system)
        s_system = nullptr;
}

namespace {

struct Registration {
    Optional<FlyString> family;
    u32 collection_index { 0 };
    u32 flags { 0 };
};

// The members after struct_size, in order (StructSize.h).
constexpr StructMember font_desc_members[] = {
    KR_STRUCT_MEMBER(kr_font_desc, family),
    KR_STRUCT_MEMBER(kr_font_desc, collection_index),
    KR_STRUCT_MEMBER(kr_font_desc, flags),
};
static_assert(lists_every_member<kr_font_desc>(font_desc_members));

ErrorOr<Registration> registration_from(kr_font_desc const* host_desc)
{
    Registration registration;
    if (!host_desc)
        return registration;
    // The members past a short struct_size are their defaults.
    kr_font_desc sized_desc {};
    if (!read_sized(*host_desc, font_desc_members, sized_desc))
        return Error::from_string_literal("struct_size can't hold family");
    auto const* desc = &sized_desc;
    if (desc->family) {
        StringView family { desc->family, strlen(desc->family) };
        if (family.is_empty())
            return Error::from_string_literal("empty family name");
        registration.family = TRY(FlyString::from_utf8(family));
    }
    registration.collection_index = desc->collection_index;
    registration.flags = desc->flags;
    return registration;
}

// The formats @font-face takes (LibWeb's try_load_vector_font), tried in the same order. The
// TrueType/OpenType path copies the bytes into a buffer of its own; WOFF and WOFF2 decode into one.
ErrorOr<NonnullRefPtr<Gfx::Typeface const>> typeface_from_bytes(ReadonlyBytes bytes, u32 collection_index)
{
    if (auto typeface = Gfx::Typeface::try_load_from_temporary_memory(bytes, collection_index); !typeface.is_error())
        return typeface.release_value();
    if (auto typeface = WOFF::try_load_from_bytes(bytes, collection_index); !typeface.is_error())
        return typeface.release_value();
    if (collection_index == 0) {
        if (auto typeface = WOFF2::try_load_from_bytes(bytes); !typeface.is_error())
            return typeface.release_value();
    }
    return Error::from_string_literal("not a font this engine reads");
}

// Every document of the process that is not torn down.
Vector<GC::Root<Web::DOM::Document>> live_documents()
{
    return Web::HTML::main_thread_event_loop().documents_in_this_event_loop_matching([](Web::DOM::Document& document) {
        return !document.has_been_destroyed();
    });
}

// A document that resolved `family` before it was registered keeps the cascade it resolved (SF)
// in its FontComputer's memo. did_load_font, the call a loaded @font-face face makes, forgets the
// cascades naming the family and restyles the elements naming it.
void documents_forget_family(FlyString const& family)
{
    auto name = Utf16FlyString::from_fly_string(family);
    for (auto& document : live_documents())
        document->font_computer().did_load_font(name);
}

// A new default font changes what the generic families, and every family no @font-face rule
// defines, resolve to: a loaded document resolves each cascade it remembers again and restyles the
// elements whose cascade answers differently (patch 0037).
void documents_resolve_system_fonts_again()
{
    for (auto& document : live_documents())
        document->font_computer().did_change_system_fonts();
}

// A glyph fallback face changes no cascade (the cascade asks the provider at shaping time) but the
// text already shaped: every document lays out again.
void documents_reshape_text()
{
    for (auto& document : live_documents())
        document->set_needs_full_layout_tree_update();
}

kr_result register_typeface(NonnullRefPtr<Gfx::Typeface const> typeface, Registration const& registration)
{
    bool is_fallback = registration.flags & KR_FONT_FALLBACK;
    if (is_fallback) {
        s_provider->add_fallback_typeface(typeface);
        // The memo may hold "no font" for code points this face covers.
        Gfx::clear_system_fallback_font_cache();
    }
    auto family = s_provider->add_typeface(move(typeface), registration.family);
    documents_forget_family(family);
    if (is_fallback)
        documents_reshape_text();
    return KR_OK;
}

kr_result register_bytes(ReadonlyBytes bytes, Registration const& registration)
{
    auto typeface = typeface_from_bytes(bytes, registration.collection_index);
    if (typeface.is_error())
        return KR_ERROR_INVALID_ARGUMENT;
    return register_typeface(typeface.release_value(), registration);
}

bool is_live(kr_system* system)
{
    return system && system == s_system && s_provider;
}

}

}

using namespace Karouka;

kr_result kr_system_add_font2(kr_system* system, void const* data, size_t size, kr_font_desc const* desc)
{
    if (!is_live(system) || !data || size == 0)
        return KR_ERROR_INVALID_ARGUMENT;
    // A face's first registration reaches CoreText's font registry (an XPC proxy, URLs) and every
    // one a locale; each autoreleases into the caller's pool otherwise.
    Core::ScopedAutoreleasePool autorelease_pool;
    auto registration = registration_from(desc);
    if (registration.is_error())
        return KR_ERROR_INVALID_ARGUMENT;
    return register_bytes({ static_cast<u8 const*>(data), size }, registration.value());
}

kr_result kr_system_add_font_file2(kr_system* system, char const* path, kr_font_desc const* desc)
{
    if (!is_live(system) || !path || !*path)
        return KR_ERROR_INVALID_ARGUMENT;
    // As kr_system_add_font2, also when the host completes the load inside its request.
    Core::ScopedAutoreleasePool autorelease_pool;
    auto registration = registration_from(desc);
    if (registration.is_error())
        return KR_ERROR_INVALID_ARGUMENT;
    auto cwd = Core::System::getcwd();
    if (cwd.is_error())
        return KR_ERROR_IO;
    auto absolute = LexicalPath::absolute_path(cwd.value(), StringView { path, strlen(path) });

    // With a host io the host reads the file, like a page; it may answer later.
    if (HostIo::is_routed()) {
        auto url = URL::create_with_file_scheme(absolute);
        if (!url.has_value())
            return KR_ERROR_INVALID_ARGUMENT;
        auto result = HostIo::request(url->serialize().to_byte_string(), system, [url = *url, registration = registration.release_value()](kr_result status, ReadonlyBytes bytes) {
            if (status == KR_OK)
                status = register_bytes(bytes, registration);
            if (status != KR_OK)
                Karouka::log(KR_LOG_WARN, "could not register the font {} (error {})", url, to_underlying(status));
            return status;
        });
        return result.value_or(KR_OK);
    }

    // TrueType/OpenType straight from a mapping of the file (no copy); failing that, WOFF or WOFF2
    // decoded from a fresh mapping into a buffer (the failed load took the first one).
    auto mapped_file = Core::MappedFile::map(absolute);
    if (mapped_file.is_error())
        return KR_ERROR_IO;
    if (auto typeface = Gfx::Typeface::try_load_from_mapped_file(mapped_file.release_value(), registration.value().collection_index); !typeface.is_error())
        return register_typeface(typeface.release_value(), registration.value());
    mapped_file = Core::MappedFile::map(absolute);
    if (mapped_file.is_error())
        return KR_ERROR_IO;
    return register_bytes(mapped_file.value()->bytes(), registration.value());
}

kr_result kr_system_set_default_font(kr_system* system, char const* family)
{
    if (!is_live(system))
        return KR_ERROR_INVALID_ARGUMENT;
    // Resolving the generic families again autoreleases a locale.
    Core::ScopedAutoreleasePool autorelease_pool;
    Optional<FlyString> default_family;
    if (family) {
        auto name = FlyString::from_utf8(StringView { family, strlen(family) });
        if (name.is_error() || !s_provider->has_family(name.value()))
            return KR_ERROR_INVALID_ARGUMENT;
        default_family = name.release_value();
    }
    s_provider->set_default_family(default_family);
    // FontPlugin memoizes the family each generic resolves to and takes its default (last-resort)
    // font from ui-sans-serif's; setting its system family forgets the memo and points that one here.
    if (auto name = s_provider->default_family_name(); name.has_value())
        Web::Platform::FontPlugin::the().set_system_font_family(name.release_value());
    documents_resolve_system_fonts_again();
    return KR_OK;
}
