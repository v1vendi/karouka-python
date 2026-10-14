/*
 * The system font provider karouka installs in place of LibGfx's PathFontProvider.
 *
 * Font policy: karouka uses exactly one system font, the macOS system UI font (San Francisco, the
 * font CTFontCreateUIFontForLanguage(kCTFontUIFontSystem) gives), upright and italic, every
 * weight and width through its variation axes. No font directory is scanned, mapped or indexed.
 * Every family name the provider is asked for -- the generic families (serif, sans-serif,
 * monospace, cursive, fantasy, system-ui, ui-*), and any family the page names but does not
 * define with @font-face -- answers with SF. There is no system glyph fallback: a code point that
 * neither the page's fonts (its font-family list and @font-face rules) nor SF cover renders as
 * SF's .notdef. CJK, Arabic, emoji and other scripts SF lacks need a font the page ships.
 *
 * The host's fonts (karouka.h kr_system_add_font*): faces added with add_typeface answer
 * to their family name ahead of SF, and to local() by their full or PostScript name; faces added
 * with add_fallback_typeface are the glyph fallback after the page's fonts (a host's last-resort
 * font for code points nothing else covers); set_default_family puts a registered family in SF's
 * place for every generic family and every family name the page does not define. None exists until
 * a host registers one, and a registered face lives as long as the process (the provider is
 * process-wide and cannot be reinstalled).
 *
 * Fonts are matched on the StyleLayout thread as well as the main thread, so everything here runs
 * under one lock. Kept as a header included by Engine.cpp only.
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/Font/TypefaceSkia.h>

namespace Karouka {

class SystemUIFontProvider final : public Gfx::SystemFontProvider {
    AK_MAKE_NONCOPYABLE(SystemUIFontProvider);
    AK_MAKE_NONMOVABLE(SystemUIFontProvider);

public:
    AK_ALLOC_WITH_KMALLOC;

    SystemUIFontProvider() = default;
    virtual ~SystemUIFontProvider() override = default;

    // A face that answers to `family` (default: the family it names itself) ahead of SF, and to
    // local() by its full and PostScript names. Returns the family it answers to.
    FlyString add_typeface(NonnullRefPtr<Gfx::Typeface const> typeface, Optional<FlyString> family = {})
    {
        auto name = family.has_value() ? family.release_value() : typeface->family();
        auto local_names = typeface->local_font_names().release_value_but_fixme_should_propagate_errors();
        MutexLocker locker(m_mutex);
        for (auto& local_name : local_names)
            m_typefaces_by_local_name.set(move(local_name), typeface);
        m_typefaces_by_family.ensure(name).append(move(typeface));
        return name;
    }

    // A face the cascade falls back to, in the order added, for code points the page's fonts lack.
    // The caller clears LibGfx's fallback memo (Gfx::clear_system_fallback_font_cache), which may
    // hold "no font" for a code point this face covers.
    void add_fallback_typeface(NonnullRefPtr<Gfx::Typeface const> typeface)
    {
        MutexLocker locker(m_mutex);
        m_fallback_typefaces.append(move(typeface));
    }

    bool has_family(FlyString const& family)
    {
        MutexLocker locker(m_mutex);
        return m_typefaces_by_family.contains(family);
    }

    // A registered family in SF's place as the default (empty: SF again). The caller makes FontPlugin
    // forget the generic family names it memoized (FontPlugin::set_system_font_family).
    void set_default_family(Optional<FlyString> family)
    {
        MutexLocker locker(m_mutex);
        m_default_family = move(family);
    }

    // The family the generic families resolve to: the host's default, else SF's family name.
    Optional<FlyString> default_family_name()
    {
        MutexLocker locker(m_mutex);
        if (m_default_family.has_value())
            return m_default_family;
        if (auto typeface = system_ui_typeface(0))
            return typeface->family();
        return {};
    }

    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<Gfx::FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {}) override
    {
        RefPtr<Gfx::Typeface const> typeface;
        {
            MutexLocker locker(m_mutex);
            if (auto const* registered = registered_or_default_typefaces(family))
                typeface = closest_typeface(*registered, weight, width, slope);
            else
                typeface = system_ui_typeface(slope);
        }
        if (!typeface)
            return nullptr;
        return typeface->font(point_size, font_variation_settings.value_or_lazy_evaluated([&] { return default_font_variation_settings(point_size, weight, width); }), shape_features.value_or_lazy_evaluated([] { return default_shape_features(); }));
    }

    virtual void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Gfx::Typeface const&)> callback) override
    {
        // The callbacks run unlocked: they may ask this provider for fonts.
        Vector<NonnullRefPtr<Gfx::Typeface const>, 2> typefaces;
        {
            MutexLocker locker(m_mutex);
            if (auto const* registered = registered_or_default_typefaces(family_name)) {
                typefaces.extend(*registered);
            } else {
                for (unsigned slope : { 0u, 1u }) {
                    if (auto typeface = system_ui_typeface(slope))
                        typefaces.append(typeface.release_nonnull());
                }
            }
        }
        for (auto const& typeface : typefaces)
            callback(*typeface);
    }

    // Every generic family is SF, or the host's default family. (FontPlugin asks this before its own
    // list of family names, and takes the answer when it loads, so no other name is tried.)
    virtual Optional<FlyString> resolve_generic_family(StringView, u16, u8) override
    {
        return default_family_name();
    }

    // local() finds the host's faces by full or PostScript name; there are no other local fonts.
    virtual RefPtr<Gfx::Typeface> get_typeface_by_local_name(String const& name) override
    {
        MutexLocker locker(m_mutex);
        auto it = m_typefaces_by_local_name.find(name);
        if (it == m_typefaces_by_local_name.end())
            return nullptr;
        // LibGfx hands typefaces out non-const here; nothing mutates them.
        return const_cast<Gfx::Typeface*>(it->value.ptr());
    }

    // No system fallback (TypefaceSkia::find_typeface_for_code_point, the default): only faces a
    // host added as fallbacks. A null answer leaves the code point to the cascade's last-resort font.
    virtual RefPtr<Gfx::Font> get_font_for_code_point(u32 code_point, float point_size, u16, u16, u8, bool) override
    {
        RefPtr<Gfx::Typeface const> typeface;
        {
            MutexLocker locker(m_mutex);
            for (auto const& candidate : m_fallback_typefaces) {
                if (candidate->glyph_id_for_code_point(code_point) != 0) {
                    typeface = candidate;
                    break;
                }
            }
        }
        if (!typeface)
            return nullptr;
        return typeface->font(point_size);
    }

private:
    // The host's faces for `family`, else the host's default family's, else none (SF). Under m_mutex.
    Vector<NonnullRefPtr<Gfx::Typeface const>> const* registered_or_default_typefaces(FlyString const& family)
    {
        if (auto it = m_typefaces_by_family.find(family); it != m_typefaces_by_family.end())
            return &it->value;
        if (m_default_family.has_value()) {
            if (auto it = m_typefaces_by_family.find(*m_default_family); it != m_typefaces_by_family.end())
                return &it->value;
        }
        return nullptr;
    }

    // The face nearest the requested style: slope first, then width, then weight.
    static NonnullRefPtr<Gfx::Typeface const> closest_typeface(Vector<NonnullRefPtr<Gfx::Typeface const>> const& typefaces, unsigned weight, unsigned width, unsigned slope)
    {
        auto difference = [](unsigned a, unsigned b) { return a > b ? a - b : b - a; };
        auto distance = [&](Gfx::Typeface const& typeface) {
            return ((typeface.slope() == 0) != (slope == 0) ? 1'000'000u : 0u)
                + difference(typeface.width(), width) * 10'000u
                + difference(typeface.weight(), weight);
        };
        auto const* best = &typefaces.first();
        for (auto const& candidate : typefaces) {
            if (distance(*candidate) < distance(**best))
                best = &candidate;
        }
        return *best;
    }

    // SF upright (slope 0) or italic (any other slope), made on first use. One typeface serves
    // every weight and width: the font's variations (wght, wdth, opsz) select them, as LibWeb's
    // own system-ui lookup does.
    RefPtr<Gfx::Typeface const> system_ui_typeface(unsigned slope)
    {
        auto index = slope == 0 ? 0 : 1;
        auto& typeface = m_system_ui_typefaces[index];
        if (!typeface && !m_system_ui_failed[index]) {
            auto matched = Gfx::TypefaceSkia::match_system_ui(Gfx::SystemUIFontKind::System, 12.0f, 400, Gfx::FontWidth::Normal, static_cast<u8>(index));
            if (matched.is_error() || !matched.value())
                m_system_ui_failed[index] = true;
            else
                typeface = matched.release_value();
        }
        return typeface;
    }

    // PathFontProvider::get_font's defaults.
    static Gfx::FontVariationSettings default_font_variation_settings(float point_size, unsigned weight, unsigned width)
    {
        Gfx::FontVariationSettings settings;
        settings.set_weight(static_cast<float>(weight));
        settings.set_optical_sizing(point_size / 0.75f); // the pixel size
        switch (width) {
        case Gfx::FontWidth::UltraCondensed: settings.set_width(50); break;
        case Gfx::FontWidth::ExtraCondensed: settings.set_width(62.5); break;
        case Gfx::FontWidth::Condensed: settings.set_width(75); break;
        case Gfx::FontWidth::SemiCondensed: settings.set_width(87.5); break;
        case Gfx::FontWidth::Normal: settings.set_width(100); break;
        case Gfx::FontWidth::SemiExpanded: settings.set_width(112.5); break;
        case Gfx::FontWidth::Expanded: settings.set_width(125); break;
        case Gfx::FontWidth::ExtraExpanded: settings.set_width(150); break;
        case Gfx::FontWidth::UltraExpanded: settings.set_width(200); break;
        default: VERIFY_NOT_REACHED();
        }
        return settings;
    }

    static Gfx::ShapeFeatures default_shape_features()
    {
        // The features applied when every CSS property has its initial value.
        Gfx::ShapeFeatures features;
        features.append({ { 'c', 'l', 'i', 'g' }, 1 });
        features.append({ { 'k', 'e', 'r', 'n' }, 1 });
        features.append({ { 'l', 'i', 'g', 'a' }, 1 });
        return features;
    }

    Mutex m_mutex;
    RefPtr<Gfx::Typeface const> m_system_ui_typefaces[2];
    bool m_system_ui_failed[2] { false, false };
    HashMap<FlyString, Vector<NonnullRefPtr<Gfx::Typeface const>>, AK::ASCIICaseInsensitiveFlyStringTraits> m_typefaces_by_family;
    HashMap<String, NonnullRefPtr<Gfx::Typeface const>, AK::ASCIICaseInsensitiveStringTraits> m_typefaces_by_local_name;
    Vector<NonnullRefPtr<Gfx::Typeface const>> m_fallback_typefaces;
    Optional<FlyString> m_default_family;
};

}
