# Third-party software

`patches/ladybird.patch` modifies [Ladybird](https://github.com/LadybirdBrowser/ladybird)
(BSD 2-Clause, © the Ladybird contributors), and the `_karouka` module contains Ladybird's
libraries. A built wheel also bundles these libraries in `karouka/.dylibs/`, built by Ladybird's
vcpkg manifest, and Ladybird's `Base/res` (less its fonts) in `karouka/res/`:

| Component | Files in the wheel | License |
| --- | --- | --- |
| Skia | `libskia.dylib` | BSD 3-Clause |
| HarfBuzz | `libharfbuzz` | MIT ("Old MIT") |
| ICU | `libicudata`, `libicui18n`, `libicuuc` | Unicode License v3 |
| libavif | `libavif` | BSD 2-Clause |
| dav1d | `libdav1d` | BSD 2-Clause |
| libjpeg-turbo | `libjpeg` | IJG, BSD 3-Clause, zlib |
| libpng | `libpng16` | libpng License v2 |
| libwebp | `libwebp`, `libwebpdemux`, `libwebpmux`, `libsharpyuv` | BSD 3-Clause |
| libyuv | `libyuv.dylib` | BSD 3-Clause |
| Brotli | `libbrotlicommon`, `libbrotlidec`, `libbrotlienc` | MIT |
| zlib | `libz` | zlib |
| libxml2 | `libxml2` | MIT |
| Expat | `libexpat` | MIT |
| {fmt} | `libfmt` | MIT |
| mimalloc | `libmimalloc` | MIT |
| simdjson | `libsimdjson` | Apache 2.0 or MIT |
| simdutf | `libsimdutf` | Apache 2.0 or MIT |
| LibTomMath | `libtommath` | LibTom License (Unlicense) |

The repository's test fonts in `pages/fonts/` (Noto Sans, Noto Sans Arabic, Noto Sans Devanagari)
are under the SIL Open Font License 1.1.

The full license text of each vcpkg library is in a build's
`third_party/ladybird/Build/release/vcpkg_installed/arm64-osx-dynamic/share/<port>/copyright`.
