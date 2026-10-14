/*
 * The host's font registration (karouka.h kr_system_add_font2, kr_system_add_font_file2,
 * kr_system_set_default_font) over SystemUIFontProvider's hooks.
 */

#pragma once

#include <karouka/karouka.h>

namespace Karouka {

class SystemUIFontProvider;

// The live system and its provider; the font calls refuse any other system.
void install_host_fonts(kr_system*, SystemUIFontProvider&);
void uninstall_host_fonts(kr_system*);

}
