# Built as a subdirectory of the pinned Ladybird tree (patch 0003), so the engine compiles with
# Ladybird's exact toolchain, flags and library targets. build.sh drives it.
# The viewer's .mm compiles as CXX, like Ladybird's own .mm files: clang selects Objective-C++ from
# the extension. (enable_language(OBJCXX) here would reroute Ladybird's .mm files too.)

# karouka's own sources, as an object library: its target dependencies on the Lagom libraries (below)
# order these compiles after the headers those libraries generate, and order nothing else. A
# dependency of libkarouka itself would make its link wait for the 24 Lagom dylibs, which nothing
# loads any more.
add_library(karouka_engine OBJECT
    engine/BindingModel.cpp
    engine/Engine.cpp
    engine/EngineChannel.cpp
    engine/ScriptTypes.cpp
    engine/HostEventLoop.cpp
    engine/HostFonts.cpp
    engine/HostInterface.cpp
    engine/HostMetal.mm
    engine/ShaderCache.cpp
    engine/ShaderImage.cpp
    engine/TextBlobCache.cpp
    engine/WebFontGlyphs.cpp
    engine/SoftwareSurface.cpp
    engine/DisplayListCensus.cpp
    engine/LegacyEntryPoints.cpp
    engine/LocalCanvas.cpp
    engine/LocalImageCodecPlugin.cpp
    engine/BindingExpression.cpp
    engine/BindingDom.cpp
)
target_include_directories(karouka_engine PUBLIC "${KAROUKA_SOURCE_DIR}/include")
target_compile_definitions(karouka_engine PRIVATE
    KAROUKA_BUILDING
    KAROUKA_DEFAULT_RESOURCE_ROOT="${LADYBIRD_SOURCE_DIR}/Base/res"
)
set_target_properties(karouka_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(karouka SHARED $<TARGET_OBJECTS:karouka_engine>)
target_include_directories(karouka PUBLIC "${KAROUKA_SOURCE_DIR}/include")

# _karouka, the CPython extension module (src/python/module.cpp, docs/PYTHON.md). LibWeb is
# local to libkarouka, so the module can't reach it through the dylib: it is a second image linked
# from the same objects (karouka_engine and the Lagom object file list below), exporting only
# PyInit__karouka. It is built against KAROUKA_PYTHON's headers and named with its EXT_SUFFIX, and
# its Python symbols are left to the interpreter that loads it (-undefined dynamic_lookup).
# KAROUKA_PYTHON's path goes next to the module (_karouka.python). It is a
# cache variable, so another CPython 3.14 can build the module: build.sh sets it from KAROUKA_PYTHON.
set(KAROUKA_PYTHON "/opt/homebrew/bin/python3" CACHE FILEPATH "The CPython 3.14 interpreter the _karouka module is built for")
execute_process(
    COMMAND "${KAROUKA_PYTHON}" -c "import sys, sysconfig; print(sysconfig.get_path('include')); print(sysconfig.get_config_var('EXT_SUFFIX')); print(sys.implementation.name, '%d.%d' % sys.version_info[:2], 'free-threaded' if sysconfig.get_config_var('Py_GIL_DISABLED') else 'gil')"
    OUTPUT_VARIABLE karouka_python_config OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE karouka_python_status
)
if (NOT karouka_python_status EQUAL 0)
    message(FATAL_ERROR "karouka: the _karouka module needs a CPython 3.14 interpreter; ${KAROUKA_PYTHON} doesn't run (set KAROUKA_PYTHON)")
endif()
string(REPLACE "\n" ";" karouka_python_config "${karouka_python_config}")
list(GET karouka_python_config 0 KAROUKA_PYTHON_INCLUDE)
list(GET karouka_python_config 1 KAROUKA_PYTHON_EXT_SUFFIX)
list(GET karouka_python_config 2 karouka_python_kind)
if (NOT karouka_python_kind STREQUAL "cpython 3.14 gil")
    message(FATAL_ERROR "karouka: the _karouka module needs a CPython 3.14 interpreter with the GIL; ${KAROUKA_PYTHON} is ${karouka_python_kind}")
endif()
message(STATUS "karouka: _karouka for ${KAROUKA_PYTHON} (${karouka_python_kind}, ${KAROUKA_PYTHON_EXT_SUFFIX})")
# the DOM bindings are generated from LibWeb's IDL (src/python/gen/generate_python_bindings.py),
# reading the same IDL files with the same pruning as LibWeb's JS bindings: idl_files.cmake's
# libweb_js_bindings() lines, less LibWeb's LIBWEB_PRUNED_FAMILIES and LIBWEB_PRUNED_IDL (as
# generate_js_bindings in Meta/CMake/libweb_generators.cmake decides), the generated IDL
# (LIBWEB_ALL_GENERATED_IDL) from LibWeb's build directory.
set(karouka_libweb_dir "${LADYBIRD_SOURCE_DIR}/Libraries/LibWeb")
get_directory_property(karouka_pruned_families DIRECTORY "${karouka_libweb_dir}" DEFINITION LIBWEB_PRUNED_FAMILIES)
get_directory_property(karouka_pruned_idl DIRECTORY "${karouka_libweb_dir}" DEFINITION LIBWEB_PRUNED_IDL)
get_directory_property(karouka_generated_idl DIRECTORY "${karouka_libweb_dir}" DEFINITION LIBWEB_ALL_GENERATED_IDL)
get_directory_property(karouka_libweb_binary_dir DIRECTORY "${karouka_libweb_dir}" DEFINITION CMAKE_CURRENT_BINARY_DIR)
file(STRINGS "${karouka_libweb_dir}/idl_files.cmake" karouka_idl_lines REGEX "^libweb_js_bindings\\(")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${karouka_libweb_dir}/idl_files.cmake")
set(karouka_idl_files)
set(karouka_pruned_idl_files)
foreach (line IN LISTS karouka_idl_lines)
    string(REGEX REPLACE "^libweb_js_bindings\\(([^ )]+).*$" "\\1" class "${line}")
    set(idl_pruned FALSE)
    foreach (family IN LISTS karouka_pruned_families)
        if ("${class}" MATCHES "^${family}/")
            set(idl_pruned TRUE)
        endif()
    endforeach()
    if ("${class}" IN_LIST karouka_pruned_idl)
        set(idl_pruned TRUE)
    endif()
    get_filename_component(basename "${class}" NAME)
    if (idl_pruned)
        list(APPEND karouka_pruned_idl_files "${karouka_libweb_dir}/${class}.idl")
    elseif ("${basename}.idl" IN_LIST karouka_generated_idl)
        list(APPEND karouka_idl_files "${karouka_libweb_binary_dir}/${class}.idl")
    else()
        list(APPEND karouka_idl_files "${karouka_libweb_dir}/${class}.idl")
    endif()
endforeach()
list(REMOVE_DUPLICATES karouka_idl_files)
set(karouka_python_bindings_dir "${CMAKE_CURRENT_BINARY_DIR}/python_bindings")
set(karouka_python_bindings_chunks 8)
set(karouka_python_bindings
    "${karouka_python_bindings_dir}/PythonBindings.h"
    "${karouka_python_bindings_dir}/PythonBindingsTable.cpp")
foreach (chunk RANGE 0 7)
    list(APPEND karouka_python_bindings "${karouka_python_bindings_dir}/PythonBindings${chunk}.cpp")
endforeach()
list(JOIN karouka_idl_files "\n" karouka_idl_file_list)
file(GENERATE OUTPUT "${karouka_python_bindings_dir}/idl_files.txt" CONTENT "${karouka_idl_file_list}")
set(karouka_pruned_idl_arguments ${karouka_pruned_idl_files})
list(TRANSFORM karouka_pruned_idl_arguments PREPEND "--pruned-idl-file=")
file(GLOB karouka_bindings_generator_sources "${LADYBIRD_SOURCE_DIR}/Meta/Generators/libweb_bindings/*.py")
add_custom_command(
    OUTPUT ${karouka_python_bindings}
    COMMAND "${Python3_EXECUTABLE}" "${KAROUKA_SOURCE_DIR}/src/python/gen/generate_python_bindings.py"
            --ladybird-meta "${LADYBIRD_SOURCE_DIR}/Meta" -o "${karouka_python_bindings_dir}"
            --chunks ${karouka_python_bindings_chunks}
            ${karouka_pruned_idl_arguments} "@${karouka_python_bindings_dir}/idl_files.txt"
    DEPENDS "${KAROUKA_SOURCE_DIR}/src/python/gen/generate_python_bindings.py" ${karouka_bindings_generator_sources}
            "${LADYBIRD_SOURCE_DIR}/Meta/Utils/webidl_parser.py" ${karouka_idl_files} ${karouka_pruned_idl_files}
            "${karouka_python_bindings_dir}/idl_files.txt"
    COMMENT "Generating karouka's Python bindings"
    VERBATIM
)
add_custom_target(karouka_python_bindings DEPENDS ${karouka_python_bindings})
add_dependencies(karouka_python_bindings generate_bindings)
add_library(_karouka MODULE python/module.cpp python/dom.cpp python/js.cpp python/app.mm ${karouka_python_bindings} $<TARGET_OBJECTS:karouka_engine>)
add_dependencies(_karouka karouka_python_bindings)
target_include_directories(_karouka PRIVATE "${karouka_python_bindings_dir}" "${KAROUKA_SOURCE_DIR}/src/python")
# the app shell (App, Window: NSApplication, NSWindow + CAMetalLayer), ARC as the viewer.
set_source_files_properties(python/app.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
target_include_directories(_karouka PRIVATE "${KAROUKA_SOURCE_DIR}/include")
target_include_directories(_karouka SYSTEM PRIVATE "${KAROUKA_PYTHON_INCLUDE}")
set_target_properties(_karouka PROPERTIES
    PREFIX ""
    SUFFIX "${KAROUKA_PYTHON_EXT_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/lib"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
)
target_link_options(_karouka PRIVATE
    "LINKER:-undefined,dynamic_lookup"
    "LINKER:-exported_symbol,_PyInit__karouka"
)
# objc_msgSend is called through the stubs ld synthesizes for HostMetal.mm's selectors, resolved after
# its other symbols: under dynamic_lookup it would be left for dyld to find. -u binds it to libobjc.
target_link_libraries(_karouka PRIVATE objc "-framework Cocoa" "-framework QuartzCore" "-framework IOSurface")
target_link_options(_karouka PRIVATE "LINKER:-u,_objc_msgSend")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/karouka/lib/_karouka.python" CONTENT "${KAROUKA_PYTHON}\n")
# the karouka package (src/python/karouka: the API over _karouka, and python -m karouka) next
# to the module, so the one lib dir on PYTHONPATH has both. Built with the module.
file(GLOB karouka_python_package CONFIGURE_DEPENDS "${KAROUKA_SOURCE_DIR}/src/python/karouka/*.py")
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/karouka/lib/karouka/.stamp"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}/karouka/lib/karouka"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/karouka/lib/karouka"
    COMMAND "${CMAKE_COMMAND}" -E copy ${karouka_python_package} "${CMAKE_BINARY_DIR}/karouka/lib/karouka/"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/karouka/lib/karouka/.stamp"
    DEPENDS ${karouka_python_package}
    VERBATIM
)
add_custom_target(karouka_python_package DEPENDS "${CMAKE_BINARY_DIR}/karouka/lib/karouka/.stamp")
add_dependencies(_karouka karouka_python_package)
# Only PyInit__karouka exported, and nothing but Python's own symbols left for the interpreter to
# bind (and AK's weak _ak_assertion_handler, -U as in libkarouka): dynamic_lookup would otherwise
# turn a missing engine symbol into an import-time error.
add_custom_command(TARGET _karouka POST_BUILD
    COMMAND sh -c "exports=$(nm -gU \"$1\" | awk '{ print $NF }'); if [ \"$exports\" != _PyInit__karouka ]; then echo \"karouka: $1 exports $exports, not only _PyInit__karouka\" >&2; exit 1; fi; if nm -m -u \"$1\" | grep 'dynamically looked up' | grep -v ' external __*Py' | grep -v ' weak external _ak_assertion_handler '; then echo \"karouka: $1 leaves the symbols above for the interpreter to bind\" >&2; exit 1; fi" sh $<TARGET_FILE:_karouka>
    VERBATIM
)

# DrawEmbedderContent replayed by LibCompositing's player on the Metal and software backends.
# It reaches Lagom internals, which no image exports, so it is linked from the Lagom objects as the
# images are (below), with ShaderImage.cpp compiled in.
add_executable(karouka_embedder_content_test "${KAROUKA_SOURCE_DIR}/tests/EmbedderContentTest.cpp" engine/ShaderImage.cpp)
target_include_directories(karouka_embedder_content_test PRIVATE engine "${KAROUKA_SOURCE_DIR}/include")
add_dependencies(karouka_embedder_content_test karouka_engine)
set_target_properties(karouka_embedder_content_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# libkarouka and _karouka (and the test above): the images linked from karouka_engine's or their own
# objects and the Lagom objects.
set(karouka_images karouka _karouka karouka_embedder_content_test)
set(karouka_image_link_options "")
set(karouka_image_link_depends "")
set(karouka_image_dependencies "")
# the Lagom libraries are linked into libkarouka from their object files instead of as 24
# separate dylibs. Each non-shared-cache dylib costs dyld ~0.35 ms per launch (mapping, code
# signature, and a trie walk per imported symbol), and the 24 imported ~10k symbols from each other.
# The objects are the ones the shared-library targets already build, so nothing in Ladybird is
# recompiled and the build dir stays the shared configuration every checkout uses.
# The order is the dylibs' dependency order (dependencies first): ld64 runs static initializers in
# link order, so they run in the same order dyld ran them across the dylibs.
# the objects reach the linker through a -filelist, and the link depends on them as files
# (LINK_DEPENDS), not on the libraries as targets: a target dependency would link each library's
# dylib first. The filelist comes before karouka's own objects on the link line, so the Lagom
# initializers run before karouka's, as they did from the dylibs.
# LibMedia and LibMediaClient are static libraries already; they are in the list so that their
# usage requirements don't drag the Lagom dylibs back onto the link line.
set(KAROUKA_LAGOM_LIBS
    AK LibUnicode LibTextCodec LibCore LibCrypto LibCompress LibFileSystem LibThreading LibRegex
    LibURL LibIPC LibGfx LibHTTP LibWebCommon LibCompositing LibGC LibImageDecoderClient
    LibImageDecoders LibSyntax LibJS LibRequests LibWasm LibXML LibMediaClient LibMedia LibWeb
)
# a LibWeb built with Embedder/Prune/WebAssembly.cmake (patch 0063) doesn't link LibWasm,
# so its objects stay off the link too.
get_target_property(karouka_libweb_deps LibWeb LINK_LIBRARIES)
if (NOT "LibWasm" IN_LIST karouka_libweb_deps)
    list(REMOVE_ITEM KAROUKA_LAGOM_LIBS LibWasm)
endif()
# Every Lagom library a merged one links must be merged too, or its dylib would load next to the
# merged copy (two copies of its globals). Their other dependencies (vcpkg dylibs, Rust static
# libraries, frameworks) are linked as they are.
set(karouka_lagom_deps "")
# the literals an empty page reads (src/libkarouka_literals.c, recorded with the order file).
# ld keeps the first copy of equal literals and the order file can't place them,
# so this object goes first on the link line: the read ones lie together at the start of __cstring
# and __const. It has no initializers, so the Lagom ones still run first.
add_library(karouka_literals OBJECT "${KAROUKA_SOURCE_DIR}/src/libkarouka_literals.c")
list(APPEND karouka_image_dependencies karouka_literals)
list(APPEND karouka_image_link_depends $<TARGET_OBJECTS:karouka_literals>)
set(karouka_lagom_objects "$<JOIN:$<TARGET_OBJECTS:karouka_literals>,\n>\n")
foreach(lib IN LISTS KAROUKA_LAGOM_LIBS)
    string(APPEND karouka_lagom_objects "$<JOIN:$<TARGET_OBJECTS:${lib}>,\n>\n")
    list(APPEND karouka_image_link_depends $<TARGET_OBJECTS:${lib}>)
    # Headers, generated Export.h files and definitions, without linking the library.
    target_link_libraries(karouka_engine PRIVATE $<COMPILE_ONLY:${lib}>)
    target_link_libraries(karouka_embedder_content_test PRIVATE $<COMPILE_ONLY:${lib}>)
    add_dependencies(karouka_engine ${lib})
    # the module's DOM proxies (python/dom.cpp) call LibWeb's C++ too.
    target_link_libraries(_karouka PRIVATE $<COMPILE_ONLY:${lib}>)
    # AK's -U,_ak_assertion_handler (a weak hook an embedder may define), for one.
    get_target_property(opts ${lib} LINK_OPTIONS)
    if (opts)
        list(APPEND karouka_image_link_options ${opts})
    endif()
    get_target_property(deps ${lib} LINK_LIBRARIES)
    foreach(dep IN LISTS deps)
        if (dep MATCHES "^(Lib[A-Za-z]+|AK)$" AND NOT dep IN_LIST KAROUKA_LAGOM_LIBS)
            message(FATAL_ERROR "karouka: ${lib} links ${dep}, which isn't in KAROUKA_LAGOM_LIBS")
        endif()
        if (dep IN_LIST KAROUKA_LAGOM_LIBS OR dep STREQUAL "GenericClangPlugin")
            continue()
        endif()
        if (TARGET ${dep} AND dep MATCHES "_rust$")
            # An imported Rust crate (Meta/CMake/rust_crate.cmake). Its usage requirements link
            # AK, LibGfx, LibUnicode and LibRegex, and add a panic-hook source the merged objects
            # already contain, so link its archive and keep only its -u options.
            get_target_property(location ${dep} IMPORTED_LOCATION)
            get_target_property(dep_opts ${dep} INTERFACE_LINK_OPTIONS)
            list(APPEND karouka_lagom_deps "${location}")
            if (dep_opts)
                list(APPEND karouka_image_link_options ${dep_opts})
            endif()
            list(APPEND karouka_image_dependencies ${dep})
            add_dependencies(karouka_engine ${dep})
        else()
            list(APPEND karouka_lagom_deps "${dep}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES karouka_lagom_deps)
set(KAROUKA_LAGOM_OBJECTS_FILE "${CMAKE_CURRENT_BINARY_DIR}/lagom-objects.txt")
file(GENERATE OUTPUT "${KAROUKA_LAGOM_OBJECTS_FILE}" CONTENT "${karouka_lagom_objects}")
# file(GENERATE) rewrites it only when the list changes, so a removed object relinks too.
list(APPEND karouka_image_link_depends "${KAROUKA_LAGOM_OBJECTS_FILE}")
# Their headers (Skia's, for one) for karouka's own sources.
foreach(dep IN LISTS karouka_lagom_deps ITEMS skia)
    if (TARGET ${dep})
        target_link_libraries(karouka_engine PRIVATE $<COMPILE_ONLY:${dep}>)
    endif()
endforeach()
# Each image exports only its entry points (libkarouka the C API, _karouka PyInit__karouka), so the
# merged Lagom code is local to the image: the linker resolves it internally, dead-strips what the
# image never reaches, and dyld has no weak C++ symbols to coalesce.
target_link_options(karouka PRIVATE "LINKER:-exported_symbol,_kr_*")
# lay out first the functions an empty page runs, in the order it first runs them, so the
# __text pages a page touches are few and adjacent instead of ~1000 scattered over 37 MB. The order
# file is recorded with src/tools/ordertrace.cpp, which
# reads the link map for the object files of static functions whose names repeat. Names the image
# no longer has are ignored, so a stale order file costs pages, not correctness.
set(KAROUKA_ORDER_FILE "${KAROUKA_SOURCE_DIR}/src/libkarouka.order")
target_link_options(karouka PRIVATE
    "LINKER:-order_file,${KAROUKA_ORDER_FILE}"
    "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/libkarouka.map"
)
set_property(TARGET karouka APPEND PROPERTY LINK_DEPENDS "${KAROUKA_ORDER_FILE}")
foreach(image IN LISTS karouka_images)
    set(options ${karouka_image_link_options})
    if (image STREQUAL "_karouka")
        # dynamic_lookup leaves AK's weak hook undefined already; a -U there only makes ld warn.
        list(FILTER options EXCLUDE REGEX "^LINKER:-U,")
    endif()
    target_link_options(${image} PRIVATE
        ${options}
        "LINKER:-filelist,${KAROUKA_LAGOM_OBJECTS_FILE}"
        "LINKER:-dead_strip"
        "LINKER:-dead_strip_dylibs"
    )
    set_property(TARGET ${image} APPEND PROPERTY LINK_DEPENDS ${karouka_image_link_depends})
    add_dependencies(${image} ${karouka_image_dependencies})
    target_link_libraries(${image} PRIVATE ${karouka_lagom_deps} skia "-framework Metal")
    # A Lagom dylib on the image's load list would mean two copies of that library's globals.
    add_custom_command(TARGET ${image} POST_BUILD
        COMMAND sh -c "if otool -L \"$1\" | grep liblagom-; then echo \"karouka: $1 links a Lagom dylib\" >&2; exit 1; fi" sh $<TARGET_FILE:${image}>
        VERBATIM
    )
endforeach()
# an image has one thread-local template, and dyld mallocs a full copy of it for every thread
# that touches any of the image's thread-locals. Merged, the Lagom libraries' thread-locals share that
# template, so one large one (LibRequests' 256 KiB read buffer, before patch 0023) costs every thread.
add_custom_command(TARGET karouka POST_BUILD
    COMMAND sh -c "size -m \"$1\" | awk '$2 == \"__thread_data:\" || $2 == \"__thread_bss:\" { n += $3 } END { if (n > 65536) { print \"karouka: \" n \" bytes of thread-locals in libkarouka, copied for every thread\" > \"/dev/stderr\"; exit 1 } }'" sh $<TARGET_FILE:karouka>
    VERBATIM
)
# The host's Metal device and queue (HostMetal.mm): manual retain/release, as LibGfx's MetalContext.mm.
set_source_files_properties(engine/HostMetal.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++")
set_target_properties(karouka PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/lib"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
)

add_executable(karouka_viewer viewer/main.mm)
target_link_libraries(karouka_viewer PRIVATE karouka
    "-framework Cocoa" "-framework Metal" "-framework QuartzCore" "-framework IOSurface" "-framework CoreVideo"
)
set_source_files_properties(viewer/main.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
target_compile_definitions(karouka_viewer PRIVATE KAROUKA_SOURCE_DIR="${KAROUKA_SOURCE_DIR}")
set_target_properties(karouka_viewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# a counting interposer for mimalloc (src/tools/miprof.cpp), inserted with
# DYLD_INSERT_LIBRARIES. Nothing links it.
add_library(karouka_miprof SHARED tools/miprof.cpp)
target_link_libraries(karouka_miprof PRIVATE mimalloc)
set_target_properties(karouka_miprof PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/lib"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
)

# records the order libkarouka's functions first run in (src/tools/ordertrace.cpp), for
# the order file, inserted with DYLD_INSERT_LIBRARIES. Nothing links it.
add_library(karouka_ordertrace SHARED tools/ordertrace.cpp)
set_target_properties(karouka_ordertrace PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/lib"
)

# Tests. The model test compiles the model alone, against AK only; the C
# test drives the public ABI through a real system and view, compiled as C.
add_executable(karouka_binding_model_test "${KAROUKA_SOURCE_DIR}/tests/BindingModelTest.cpp" engine/BindingModel.cpp)
target_include_directories(karouka_binding_model_test PRIVATE "${KAROUKA_SOURCE_DIR}/include" engine)
target_link_libraries(karouka_binding_model_test PRIVATE AK)

add_executable(karouka_binding_c_test "${KAROUKA_SOURCE_DIR}/tests/binding_c_test.c")
target_link_libraries(karouka_binding_c_test PRIVATE karouka)

set_target_properties(karouka_binding_model_test karouka_binding_c_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# the data-bind-* expression language (AK only) and the DOM propagation, end to end in C.
add_executable(karouka_binding_expression_test "${KAROUKA_SOURCE_DIR}/tests/BindingExpressionTest.cpp" engine/BindingExpression.cpp engine/BindingModel.cpp)
target_include_directories(karouka_binding_expression_test PRIVATE "${KAROUKA_SOURCE_DIR}/include" engine)
target_link_libraries(karouka_binding_expression_test PRIVATE AK)
add_executable(karouka_binding_dom_test "${KAROUKA_SOURCE_DIR}/tests/binding_dom_test.c")
target_link_libraries(karouka_binding_dom_test PRIVATE karouka)
set_target_properties(karouka_binding_expression_test karouka_binding_dom_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# the `engine` page <-> host channel, end to end in C.
add_executable(karouka_engine_channel_test "${KAROUKA_SOURCE_DIR}/tests/engine_channel_test.c")
target_link_libraries(karouka_engine_channel_test PRIVATE karouka)
set_target_properties(karouka_engine_channel_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# paints that change nothing in the viewport are not rasterized.
add_executable(karouka_render_skip_test "${KAROUKA_SOURCE_DIR}/tests/render_skip_test.c")
target_link_libraries(karouka_render_skip_test PRIVATE karouka)
set_target_properties(karouka_render_skip_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# Skia's programs persist across processes in kr_system_desc.shader_cache_path.
add_executable(karouka_shader_cache_test "${KAROUKA_SOURCE_DIR}/tests/shader_cache_test.c")
target_link_libraries(karouka_shader_cache_test PRIVATE karouka)
set_target_properties(karouka_shader_cache_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# a frame rasterized into its damage holds the pixels of a whole raster.
add_executable(karouka_partial_raster_test "${KAROUKA_SOURCE_DIR}/tests/partial_raster_test.c")
target_link_libraries(karouka_partial_raster_test PRIVATE karouka)
set_target_properties(karouka_partial_raster_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# kr_view_title follows the loaded page, empty for a page without <title>.
add_executable(karouka_title_reset_test "${KAROUKA_SOURCE_DIR}/tests/title_reset_test.c")
target_link_libraries(karouka_title_reset_test PRIVATE karouka)
set_target_properties(karouka_title_reset_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# kr_view_set_focused paints selections in InactiveSelection, stops the caret,
# and fires blur and focus.
add_executable(karouka_view_focus_test "${KAROUKA_SOURCE_DIR}/tests/view_focus_test.c")
target_link_libraries(karouka_view_focus_test PRIVATE karouka)
set_target_properties(karouka_view_focus_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# kr_view_reload and kr_view_get_dependencies.
add_executable(karouka_reload_test "${KAROUKA_SOURCE_DIR}/tests/reload_test.c")
target_link_libraries(karouka_reload_test PRIVATE karouka)
set_target_properties(karouka_reload_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# <script> types the host runs (kr_view_set_script_type_callback), in classic-script order.
add_executable(karouka_script_type_test "${KAROUKA_SOURCE_DIR}/tests/script_type_test.c")
target_link_libraries(karouka_script_type_test PRIVATE karouka)
set_target_properties(karouka_script_type_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# off-thread script compiles and stylesheet parses land at the same frame on every run.
add_executable(karouka_script_frame_test "${KAROUKA_SOURCE_DIR}/tests/script_frame_test.c")
target_link_libraries(karouka_script_frame_test PRIVATE karouka)
set_target_properties(karouka_script_frame_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# the pool starts one thread for one off-thread job, none for none; it retires idle ones.
add_executable(karouka_pool_threads_test "${KAROUKA_SOURCE_DIR}/tests/pool_threads_test.c")
target_link_libraries(karouka_pool_threads_test PRIVATE karouka)
set_target_properties(karouka_pool_threads_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# kr_system_create2 fails after kr_system_destroy.
add_executable(karouka_system_lifecycle_test "${KAROUKA_SOURCE_DIR}/tests/system_lifecycle_test.c")
target_link_libraries(karouka_system_lifecycle_test PRIVATE karouka)
set_target_properties(karouka_system_lifecycle_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# bytes per subsystem (kr_system_memory_usage).
add_executable(karouka_memory_usage_test "${KAROUKA_SOURCE_DIR}/tests/memory_usage_test.c")
target_link_libraries(karouka_memory_usage_test PRIVATE karouka)
set_target_properties(karouka_memory_usage_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# kr_view_stats is versioned by struct_size; old-header hosts are not written past.
add_executable(karouka_view_stats_test "${KAROUKA_SOURCE_DIR}/tests/view_stats_test.c")
target_link_libraries(karouka_view_stats_test PRIVATE karouka)
set_target_properties(karouka_view_stats_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# the pumps an idle 1 s kr_view_advance_time takes (kr_view_stats.event_loop_pumps).
add_executable(karouka_host_clock_pumps_test "${KAROUKA_SOURCE_DIR}/tests/host_clock_pumps_test.c")
target_link_libraries(karouka_host_clock_pumps_test PRIVATE karouka)
set_target_properties(karouka_host_clock_pumps_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# kr_system_desc, kr_view_desc, kr_memory_stats, kr_font_desc, kr_input_event,
# kr_metal_output and kr_engine_callbacks are versioned by struct_size.
add_executable(karouka_struct_size_test "${KAROUKA_SOURCE_DIR}/tests/struct_size_test.c")
target_link_libraries(karouka_struct_size_test PRIVATE karouka "-framework Metal" objc)
target_compile_definitions(karouka_struct_size_test PRIVATE KAROUKA_SOURCE_DIR="${KAROUKA_SOURCE_DIR}")
set_target_properties(karouka_struct_size_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# the software backend (KR_BACKEND_SOFTWARE) renders into the IOSurface without a GPU.
add_executable(karouka_software_backend_test "${KAROUKA_SOURCE_DIR}/tests/software_backend_test.c")
target_link_libraries(karouka_software_backend_test PRIVATE karouka "-framework IOSurface")
set_target_properties(karouka_software_backend_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# kr_view_set_output2 refuses a texture that breaks a kr_metal_output rule.
add_executable(karouka_output_texture_test "${KAROUKA_SOURCE_DIR}/tests/output_texture_test.c")
target_link_libraries(karouka_output_texture_test PRIVATE karouka "-framework Metal" objc)
set_target_properties(karouka_output_texture_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)

# an OpenGL host reads kr_view_iosurface through CGLTexImageIOSurface2D, on each backend.
add_executable(karouka_gl_interop_test "${KAROUKA_SOURCE_DIR}/tests/gl_interop_test.c")
target_link_libraries(karouka_gl_interop_test PRIVATE karouka "-framework IOSurface" "-framework OpenGL" "-framework Metal" objc)
set_target_properties(karouka_gl_interop_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/karouka/bin"
    BUILD_RPATH "${CMAKE_BINARY_DIR}/karouka/lib;${CMAKE_BINARY_DIR}/lib"
)
