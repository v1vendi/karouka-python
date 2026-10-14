#!/usr/bin/env python3
"""karouka's Python bindings generator (docs/PYTHON.md § Bindings).

Reads the same IDL files, with the same pruning, as Ladybird's JS bindings generator
(Meta/Generators/generate_libweb_bindings.py), through its parser and C++ type mapping, and emits
CPython glue: one heap type per interface, `_karouka.<IDL name>`, subclassing its IDL parent, whose
attributes, operations and constants call the same C++ the JS glue calls.

Outputs, into --output-path:
  PythonBindings.h          DomType (one enumerator per interface) and DomTypeOf<C++ class>
  PythonBindings<N>.cpp     the glue, in chunks compiled in parallel
  PythonBindingsTable.cpp   the type table dom.cpp builds the types from, and the window's names
  PythonBindings.report     what the generator left out, and why (members of unsupported types)

The C++ side of the conversions lives in src/python/bindings.h (hand-written): the generator emits
only the member-specific glue. A member whose C++ call does not resolve at compile time (each call
tries the JS glue's forms: no realm, the global object, the realm, the VM) still gets an entry,
which raises NotImplementedError, and _karouka._bindings_report() lists it.

Hand-written overrides (src/python/dom.cpp, `s_overrides`) replace or add members by name: what the
IDL can't express in Python (camel-cased CSS properties, interface objects on window) and what this
generator doesn't convert yet (callbacks, dictionaries).
"""

import argparse
import re
import sys

from dataclasses import replace
from io import StringIO
from pathlib import Path
from typing import List
from typing import Optional

parser = argparse.ArgumentParser()
parser.add_argument("--ladybird-meta", required=True, type=Path, help="third_party/ladybird/Meta")
parser.add_argument("-o", "--output-path", required=True, type=Path)
parser.add_argument("--chunks", type=int, default=8)
parser.add_argument("--depfile", type=Path)
parser.add_argument("--pruned-idl-file", action="append", default=[], type=Path)
parser.add_argument("paths", nargs="+", help="IDL files, or @FILE with one per line")
arguments = parser.parse_args()

sys.path.insert(0, str(arguments.ladybird_meta.resolve()))

from Generators.libweb_bindings.context import GenerationContext  # noqa: E402
from Generators.libweb_bindings.cpp_types import TypeOptionality  # noqa: E402
from Generators.libweb_bindings.cpp_types import converter_function_name  # noqa: E402
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type as ladybird_cpp_type_for_idl_type  # noqa: E402
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface  # noqa: E402
from Generators.libweb_bindings.cpp_types import idl_implementation_cpp_name  # noqa: E402
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface  # noqa: E402
from Generators.libweb_bindings.cpp_types import is_numeric_type  # noqa: E402
from Generators.libweb_bindings.cpp_types import is_string_type  # noqa: E402
from Generators.libweb_bindings.glue_headers import bindings_glue_header_for_interface  # noqa: E402
from Generators.libweb_bindings.intrinsics import parse_exposure_set  # noqa: E402
from Generators.libweb_bindings.pruning import close_over_aliases  # noqa: E402
from Generators.libweb_bindings.pruning import prune_module  # noqa: E402
from Generators.libweb_bindings.pruning import pruned_definition_names  # noqa: E402
from Utils.utils import string_to_cpp_enum_name  # noqa: E402
from Utils.webidl_parser import IDLParameterizedType  # noqa: E402
from Utils.webidl_parser import IDLType  # noqa: E402
from Utils.webidl_parser import IDLUnionType  # noqa: E402
from Utils.webidl_parser import Interface  # noqa: E402
from Utils.webidl_parser import Operation  # noqa: E402
from Utils.webidl_parser import parse_module  # noqa: E402


def read_paths(paths: List[str]) -> List[Path]:
    result: List[Path] = []
    for path in paths:
        if path.startswith("@"):
            result.extend(Path(line) for line in Path(path[1:]).read_text().splitlines() if line)
        else:
            result.append(Path(path))
    return result


input_paths = read_paths(arguments.paths)
modules = [parse_module(path, path.read_text(encoding="utf-8")) for path in input_paths]
pruned_modules = [parse_module(path, path.read_text(encoding="utf-8")) for path in arguments.pruned_idl_file]
pruned_names = close_over_aliases(pruned_definition_names(pruned_modules), modules)
modules = [prune_module(module, pruned_names) for module in modules]
context = GenerationContext(modules)
modules = context.modules

report: List[str] = []
enums_used: dict[str, set[str]] = {}  # chunk-local: enumeration names whose string conversion it needs

# ---- Which IDL types convert ----

# Unconvertible today: dictionaries need a per-dictionary converter, buffer sources and records a
# form in Python. `any`/`object` are JSValues, Promise an awaitable JSValue, callback functions any
# callable (src/python/js.cpp).
UNSUPPORTED_NAMES = {"record", "ArrayBuffer", "ArrayBufferView", "BufferSource",
                     "AllowSharedBufferSource", "DataView", "bigint", "symbol",
                     "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array",
                     "Uint32Array", "Float16Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array",
                     "Observable"}


def why_unsupported(idl_type: IDLType, returning: bool = True) -> Optional[str]:
    """Why a value of the type can't cross yet; None when it can. An argument (returning=False) of a
    union type needs one alternative Python converts (BlobPart's USVString or Blob; a buffer source
    raises TypeError at run time), and a dictionary argument goes through LibWeb's own converter."""
    idl_type = context.resolve_typedef(idl_type)
    if isinstance(idl_type, IDLUnionType):
        reasons = [why_unsupported(member, returning) for member in idl_type.member_types]
        if returning:
            return next((reason for reason in reasons if reason), None)
        return None if any(reason is None for reason in reasons) else reasons[0]
    if isinstance(idl_type, IDLParameterizedType):
        if idl_type.name in ("sequence", "FrozenArray"):
            return why_unsupported(idl_type.parameters[0], returning)
        if idl_type.name == "Promise":
            return None
        return idl_type.name
    name = idl_type.name
    if name in UNSUPPORTED_NAMES:
        return name
    if name in ("undefined", "boolean", "WindowProxy", "any", "object") or is_string_type(name) or is_numeric_type(name):
        return None
    if context.enumeration(idl_type) is not None:
        return None
    interface = context.interface(idl_type)
    if interface is not None:
        if interface.is_callback_interface or interface.is_namespace:
            return f"callback interface {name}"
        return None
    if context.callback_function(idl_type) is not None:
        return None
    if context.dictionary(idl_type) is not None:
        return f"dictionary {name}" if returning else None
    return name


def collect_enums(idl_type: IDLType, into: set[str]) -> None:
    """Enumerations (and dictionaries, into CURRENT_CHUNK's) whose conversion the chunk defines."""
    idl_type = context.resolve_typedef(idl_type)
    for nested in idl_type.nested_types():
        nested = context.resolve_typedef(nested)
        enumeration = context.enumeration(nested)
        if enumeration is not None:
            into.add(enumeration.name)
        dictionary = context.dictionary(nested)
        if dictionary is not None and CURRENT_CHUNK[0] is not None:
            CURRENT_CHUNK[0].dictionaries.add(dictionary.name)
            if dictionary.name in DEFINING_MODULE:
                CURRENT_CHUNK[0].binding_stems.add(DEFINING_MODULE[dictionary.name])


# ---- Interfaces ----

def cpp_type_for_idl_type(*args, **kwargs) -> str:
    """Ladybird's C++ type, with enumeration names qualified: the JS glue lives in Web::Bindings."""
    name = ladybird_cpp_type_for_idl_type(*args, **kwargs)
    def qualify(match) -> str:
        word = match.group(1)
        if word not in context.enumerations and word not in context.dictionaries:
            return word
        if CURRENT_CHUNK[0] is not None and word in DEFINING_MODULE:
            CURRENT_CHUNK[0].binding_stems.add(DEFINING_MODULE[word])
        return f"Bindings::{word}"
    name = re.sub(r"(?<![:\w])(\w+)", qualify, name)
    # LibWeb's namespaces, fully qualified: ::Crypto (LibCrypto) would make Crypto:: ambiguous.
    return re.sub(r"(?<![:\w])(\w+)::", lambda m: f"Web::{m.group(1)}::" if m.group(1) in LIBWEB_NAMESPACES else m.group(0), name)


CURRENT_CHUNK: list = [None]
# The module (its LibWeb/Bindings/<stem>.h) that declares each enumeration and dictionary.
DEFINING_MODULE = {definition.name: module.path.stem for module in modules
                   for definition in list(module.enumerations) + list(module.dictionaries)}


def referenced_interfaces(idl_type: IDLType, chunk) -> None:
    """Includes the implementation header of every interface a member's type names."""
    for nested in context.resolve_typedef(idl_type).nested_types():
        interface = context.interface(nested)
        if interface is not None and not interface.is_callback_interface and not interface.is_namespace:
            chunk.includes.add(implementation_header_for_interface(interface))


def has_interface_object(interface: Interface) -> bool:
    if interface.is_namespace:
        return False
    if interface.is_callback_interface:
        return bool(interface.constants)
    return True


def window_exposed(interface: Interface) -> bool:
    exposed = interface.extended_attributes.get("Exposed")
    return exposed is not None and "Window" in parse_exposure_set(interface.name, exposed)


interfaces = {name: interface for name, interface in context.interfaces.items() if has_interface_object(interface)}
ordered: List[Interface] = []
seen: set[str] = set()


def visit(interface: Interface) -> None:
    if interface.name in seen:
        return
    seen.add(interface.name)
    parent = interfaces.get(interface.parent_name)
    if parent is not None:
        visit(parent)
    ordered.append(interface)


for name in sorted(interfaces):
    visit(interfaces[name])

index_of = {interface.name: index for index, interface in enumerate(ordered)}
LIBWEB_NAMESPACES = {fully_qualified_name_for_interface(i).split("::")[0] for i in context.interfaces.values()} | {"Bindings"}


def ident(*parts: str) -> str:
    """A C++ identifier from IDL names; CSS descriptor attributes have dashes (margin-top)."""
    return "_".join("".join(c if c.isalnum() or c == "_" else f"_x{ord(c):x}_" for c in part) for part in parts)


# Glue headers of the modules karouka keeps. A mixin member's glue lives with the mixin's module
# (Window's fetch() in Fetch/BindingsGlue.h), not the interface's, so a member gets them all.
KEPT_GLUE_HEADERS = ["LibWeb/Animations/BindingsGlue.h", "LibWeb/CSS/BindingsGlue.h", "LibWeb/DOM/BindingsGlue.h",
                     "LibWeb/Fetch/BindingsGlue.h", "LibWeb/HTML/BindingsGlue.h", "LibWeb/Streams/BindingsGlue.h",
                     "LibWeb/TrustedTypes/BindingsGlue.h", "LibWeb/WebAudio/BindingsGlue.h"]


def glue_header(interface: Interface) -> Optional[str]:
    """The LibWeb glue header of an [ImplementedInBindings] member's interface; None when it has none."""
    try:
        return bindings_glue_header_for_interface(interface)
    except RuntimeError:
        return None


def cpp_class(interface: Interface) -> str:
    return f"Web::{fully_qualified_name_for_interface(interface)}"


def call_alternatives(interface: Interface, member, method: str, call_arguments: List[str], receiver: str = "impl->") -> List[str]:
    """The forms the JS glue tries, in its order (operations.py, attributes.py). A static member's
    receiver is `I::`; a [CreatesPromise] operation's C++ takes the promise the glue made last."""
    if "CreatesPromise" in member.extended_attributes:
        call_arguments = call_arguments + ["promise"]
    args = ", ".join(call_arguments)
    with_args = (lambda first: f"{first}, {args}" if args else first)
    if "ImplementedInBindings" in member.extended_attributes:
        return [f"Bindings::{method}({with_args('*impl')})", f"Bindings::{method}({with_args('realm, *impl')})"]
    calls = [
        f"{receiver}{method}({args})",
        f"{receiver}{method}({with_args('realm.global_object()')})",
        f"{receiver}{method}({with_args('realm')})",
        f"{receiver}{method}({with_args('realm.vm()')})",
    ]
    if "NeedsCallerRealm" in member.extended_attributes or "PassesRealm" in member.extended_attributes:
        calls[0], calls[2] = calls[2], calls[0]
    return calls


def caller_realm(member) -> str:
    """Whether the member is [NeedsCallerRealm]: its C++ works in the caller's realm, which for a
    Python call is the document's, entered by Call with no running JS function. The table marks
    these so tests/python/test_caller_realm.py can check that its page calls every one."""
    return "true" if "NeedsCallerRealm" in member.extended_attributes else "false"


def write_dispatch(out: StringIO, calls: List[str], unbound: str, indent: str = "    ", result: str = "result_to_py(*context, ") -> None:
    """if constexpr over the call forms: the first that compiles is the call; none raises."""
    out.write(f"{indent}if constexpr (!std::is_base_of_v<GC::Cell, I>)\n{indent}    return {unbound};\n")
    for number, call in enumerate(calls):
        out.write(f"{indent}else if constexpr (requires {{ {call}; }})\n")
        out.write(f"{indent}    return {result}[&]() -> decltype(auto) {{ return {call}; }});\n")
    out.write(f"{indent}else\n{indent}    return {unbound};\n")


def write_bound_concept(out: StringIO, name: str, calls: List[str], parameters: List[str]) -> None:
    params = ", ".join(["I* impl", "JS::Realm& realm"] + parameters)
    alternatives = " || ".join(f"requires({params}) {{ {call}; }}" for call in calls)
    out.write(f"template<typename I>\nconcept {name} = std::is_base_of_v<GC::Cell, I> && ({alternatives});\n")


class Chunk:
    def __init__(self, number: int):
        self.number = number
        self.out = StringIO()
        self.includes: set[str] = set()
        self.enums: set[str] = set()
        self.binding_stems: set[str] = set()
        self.dictionaries: set[str] = set()


def default_expression(parameter, cpp_type: str) -> Optional[str]:
    value = parameter.default_value
    if value is None:
        return None
    idl_type = context.resolve_typedef(parameter.type)
    enumeration = context.enumeration(idl_type)
    if enumeration is not None and value.startswith('"'):
        return f"Bindings::{enumeration.name}::{string_to_cpp_enum_name(value.strip(chr(34)))}"
    if value == "null":
        return "{}"
    if value in ("true", "false"):
        return value
    if value.startswith('"'):
        if "Utf16" in cpp_type:
            return f"{cpp_type}({value}_utf16)" if "Fly" not in cpp_type else f"{cpp_type}({value}_utf16_fly_string)"
        return f"{cpp_type}::from_utf8_without_validation({value}sv)" if cpp_type == "String" else None
    if value == "[]":
        return "{}"  # an empty sequence
    if value == "{}":
        return None
    try:
        float(value)
        return f"static_cast<{cpp_type}>({value})"
    except ValueError:
        return None


def omitted_argument(parameter) -> Optional[str]:
    """The C++ value of an optional argument Python can't pass yet: what JS passes for undefined,
    or its default ({} for a dictionary: its members' defaults)."""
    if parameter.default_value is None:
        return "{}"
    idl_type = context.resolve_typedef(parameter.type)
    cpp_type = cpp_type_for_idl_type(idl_type, context, extended_attributes=parameter.extended_attributes)
    if isinstance(idl_type, IDLUnionType):
        members = idl_type.flattened_member_types()
        dictionary = next((m for m in members if context.dictionary(m) is not None), None)
        if parameter.default_value == "{}" and dictionary is not None:
            return f"{cpp_type} {{ {cpp_type_for_idl_type(dictionary, context)} {{}} }}"
        value = parameter.default_value
        if value in ("true", "false") and any(m.name == "boolean" for m in members):
            return f"{cpp_type} {{ {value} }}"
        numeric = next((m for m in members if is_numeric_type(m.name)), None)
        if numeric is not None and re.fullmatch(r"-?[0-9.]+", value):
            return f"{cpp_type} {{ static_cast<{cpp_type_for_idl_type(numeric, context)}>({value}) }}"
        string = next((m for m in members if is_string_type(m.name)), None)
        if string is not None and value.startswith('"') and value == '""':
            return f"{cpp_type} {{ {cpp_type_for_idl_type(string, context)} {{}} }}"
        return None
    if context.dictionary(idl_type) is None and parameter.default_value != "null":
        return None
    return f"{cpp_type} {{}}"


def default_dictionary(parameter) -> Optional[IDLType]:
    """The dictionary type of an optional argument whose default is `{}` (itself, or a union's)."""
    if not parameter.optional or parameter.default_value != "{}":
        return None
    idl_type = context.resolve_typedef(parameter.type)
    members = idl_type.flattened_member_types() if isinstance(idl_type, IDLUnionType) else [idl_type]
    return next((m for m in members if context.dictionary(context.resolve_typedef(m)) is not None), None)


def write_arguments(out: StringIO, chunk: Chunk, where: str, parameters) -> Optional[List[str]]:
    """Converts args[i] into a{i}; returns the call arguments, or None (reported) when unsupported."""
    names: List[str] = []
    for number, parameter in enumerate(parameters):
        reason = why_unsupported(parameter.type, returning=False)
        if reason:
            report.append(f"{where}: argument {parameter.name} is {reason}")
            return None
        collect_enums(parameter.type, chunk.enums)
        if parameter.variadic:
            out.write(f"    {variadic_type(parameter)} a{number};\n")
            out.write(f"    for (Py_ssize_t i = {number}; i < nargs; ++i) {{\n")
            element = cpp_type_for_idl_type(parameter.type, context, extended_attributes=parameter.extended_attributes)
            out.write(f"        Optional<{element}> element;\n")
            out.write(f'        if (!convert(*context, args[i], element, "{where}"))\n            return nullptr;\n')
            out.write("        a%d.append(element.release_value());\n    }\n" % number)
            names.append(f"a{number}")
            break
        if parameter.optional and parameter.default_value is None:
            cpp_type = cpp_type_for_idl_type(parameter.type, context, TypeOptionality.OptionalArgument,
                                             parameter.extended_attributes)
            out.write(f"    Optional<{cpp_type}> a{number}_storage;\n")
            out.write(f"    if (nargs > {number} && args[{number}] != Py_None) {{\n")
            out.write(f'        if (!convert(*context, args[{number}], a{number}_storage, "{where}"))\n            return nullptr;\n')
            out.write(f"    }} else {{\n        a{number}_storage = {cpp_type} {{}};\n    }}\n")
        else:
            cpp_type = cpp_type_for_idl_type(parameter.type, context, extended_attributes=parameter.extended_attributes)
            out.write(f"    Optional<{cpp_type}> a{number}_storage;\n")
            dictionary = default_dictionary(parameter)
            if dictionary is not None:
                # `= {}`: LibWeb's converter of undefined, as JS passes it (the members' defaults).
                dictionary_type = cpp_type_for_idl_type(dictionary, context)
                out.write(f"    if (nargs > {number} && args[{number}] != Py_None) {{\n")
                out.write(f'        if (!convert(*context, args[{number}], a{number}_storage, "{where}"))\n            return nullptr;\n')
                out.write(f"    }} else {{\n        Optional<{dictionary_type}> empty;\n")
                out.write(f'        if (!convert(*context, Py_None, empty, "{where}"))\n            return nullptr;\n')
                out.write(f"        a{number}_storage = {cpp_type} {{ empty.release_value() }};\n    }}\n")
            elif parameter.optional:
                default = default_expression(parameter, cpp_type)
                if default is None:
                    report.append(f"{where}: argument {parameter.name} has default {parameter.default_value}")
                    return None
                out.write(f"    if (nargs > {number} && args[{number}] != Py_None) {{\n")
                out.write(f'        if (!convert(*context, args[{number}], a{number}_storage, "{where}"))\n            return nullptr;\n')
                initializer = f"{cpp_type} {{}}" if default == "{}" else f"{cpp_type}({default})"
                out.write(f"    }} else {{\n        a{number}_storage = {initializer};\n    }}\n")
            else:
                out.write(f'    if (!convert(*context, args[{number}], a{number}_storage, "{where}"))\n        return nullptr;\n')
        out.write(f"    auto& a{number} = *a{number}_storage;\n")
        names.append(f"a{number}")
    return names


def variadic_type(parameter) -> str:
    """A variadic argument's C++ list: GC::RootVector for JS values and GC pointers, as LibWeb takes
    them (TrustedTypePolicy.createHTML's `any... arguments`), else Vector (a union of GC pointers
    can't be rooted; LibWeb takes those as a Vector)."""
    element = cpp_type_for_idl_type(parameter.type, context, extended_attributes=parameter.extended_attributes)
    rootable = element == "JS::Value" or element.startswith(("GC::Ref<", "GC::Ptr<"))
    return f"{'GC::RootVector' if rootable else 'Vector'}<{element}>"


def parameter_types(parameters, promise: bool = False) -> List[str]:
    types = ["GC::Ref<Web::WebIDL::Promise> promise"] if promise else []
    for number, parameter in enumerate(parameters):
        if parameter.variadic:
            types.append(f"{variadic_type(parameter)}& a{number}")
            break
        if parameter.optional and parameter.default_value is None:
            cpp_type = cpp_type_for_idl_type(parameter.type, context, TypeOptionality.OptionalArgument,
                                             parameter.extended_attributes)
        else:
            cpp_type = cpp_type_for_idl_type(parameter.type, context, extended_attributes=parameter.extended_attributes)
        types.append(f"{cpp_type}& a{number}")
    return types


def write_attribute(chunk: Chunk, interface: Interface, attribute, getset: List[str], bound: List[str]) -> None:
    out = chunk.out
    where = f"{interface.name}.{attribute.name}"
    reason = why_unsupported(attribute.type)
    if reason:
        report.append(f"{where}: type {reason}")
        return
    if "ReturnsJSValue" in attribute.extended_attributes:
        # The JS glue returns a JS::Value (a WindowProxy for a Window); the implementation's own
        # getter, by the attribute's name, returns the object.
        extended = {k: v for k, v in attribute.extended_attributes.items() if k not in ("ReturnsJSValue", "ImplementedAs", "ImplementedInBindings")}
        attribute = replace(attribute, extended_attributes=extended)
    collect_enums(attribute.type, chunk.enums)
    referenced_interfaces(attribute.type, chunk)
    if "ImplementedInBindings" in attribute.extended_attributes:
        header = glue_header(interface)
        if header is None:
            report.append(f"{where}: [ImplementedInBindings] with no bindings glue header")
            return
        chunk.includes.add(header)
        chunk.includes.update(KEPT_GLUE_HEADERS)
    klass = cpp_class(interface)
    function = ident(interface.name, "get", attribute.name)
    reflect = "Reflect" in attribute.extended_attributes
    concept = f"bound_{function}"
    setter = "nullptr"
    if reflect:
        content = attribute.extended_attributes.get("Reflect") or attribute.name.lower()
        content = content.strip('"')
        kind = attribute.type.name
        simple = not attribute.type.nullable and "Enumerated" not in attribute.extended_attributes
        if not simple or kind not in ("boolean", "DOMString", "USVString", "long", "unsigned long"):
            report.append(f"{where}: [Reflect] of {attribute.type}{' [Enumerated]' if 'Enumerated' in attribute.extended_attributes else ''}")
            return
        if "URL" in attribute.extended_attributes:
            report.append(f"{where}: [Reflect, URL]")
            return
        out.write(f"static PyObject* {function}(PyObject* self, void*)\n{{\n    BEGIN({klass});\n")
        out.write(f'    static auto const& name = *new Utf16FlyString("{content}"_utf16_fly_string);\n')
        out.write(f"    return reflected_get_{kind.replace(' ', '_')}(*context, *impl, name);\n}}\n\n")
        setter_function = ident(interface.name, "set", attribute.name)
        if not attribute.readonly:
            out.write(f"static int {setter_function}(PyObject* self, PyObject* value, void*)\n{{\n    BEGIN_SETTER({klass});\n")
            out.write(f'    static auto const& name = *new Utf16FlyString("{content}"_utf16_fly_string);\n')
            out.write(f'    return reflected_set_{kind.replace(" ", "_")}(*context, *impl, name, value, "{where}");\n}}\n\n')
            setter = setter_function
        getset.append(f'{{ "{attribute.name}", {function}, {setter}, nullptr, nullptr }},')
        bound.append(f'{{ "{where}", true, false }},')
        return

    method = idl_implementation_cpp_name(attribute)
    calls = call_alternatives(interface, attribute, method, [])
    write_bound_concept(out, concept, calls, [])
    out.write(f"template<typename I = {klass}>\nstatic PyObject* {function}(PyObject* self, void*)\n{{\n    BEGIN(I);\n")
    out.write("    [[maybe_unused]] auto& realm = realm_of(*context);\n")
    write_dispatch(out, calls, f'unbound("{where}")')
    out.write("}\n\n")
    getter_name = f"{function}<>"

    if not attribute.readonly:
        setter_function = ident(interface.name, "set", attribute.name)
        cpp_type = cpp_type_for_idl_type(attribute.type, context, extended_attributes=attribute.extended_attributes)
        setter_calls = call_alternatives(interface, attribute, f"set_{method}", ["value_storage.value()"])
        if cpp_type.startswith("GC::Ptr<"):
            # Event handler setters take a raw pointer (set_onclick(WebIDL::CallbackType*)).
            setter_calls += call_alternatives(interface, attribute, f"set_{method}", ["value_storage.value().ptr()"])
        out.write(f"template<typename I = {klass}>\nstatic int {setter_function}(PyObject* self, PyObject* value, void*)\n{{\n")
        out.write("    BEGIN_SETTER(I);\n    [[maybe_unused]] auto& realm = realm_of(*context);\n")
        out.write(f"    Optional<{cpp_type}> value_storage;\n")
        out.write(f'    if (!convert(*context, value, value_storage, "{where}"))\n        return -1;\n')
        out.write(f'    if constexpr (!std::is_base_of_v<GC::Cell, I>)\n        return unbound_setter("{where}");\n')
        for number, call in enumerate(setter_calls):
            keyword = "else if"
            out.write(f"    {keyword} constexpr (requires {{ {call}; }})\n")
            out.write(f"        return setter_result(*context, [&]() -> decltype(auto) {{ return {call}; }});\n")
        out.write(f'    else\n        return unbound_setter("{where}");\n}}\n\n')
        setter = f"{setter_function}<>"
    forwards = attribute.extended_attributes.get("PutForwards")
    if attribute.readonly and forwards:
        # [PutForwards=href] location = "...": sets location.href, through its own proxy.
        setter_function = ident(interface.name, "set", attribute.name)
        out.write(f"static int {setter_function}(PyObject* self, PyObject* value, void*)\n{{\n")
        out.write(f'    if (!value) {{\n        PyErr_SetString(PyExc_TypeError, "the attribute can\'t be deleted");\n        return -1;\n    }}\n')
        out.write(f"    PyObject* target = {getter_name}(self, nullptr);\n    if (!target)\n        return -1;\n")
        out.write(f'    int result = PyObject_SetAttrString(target, "{forwards}", value);\n    Py_DECREF(target);\n    return result;\n}}\n\n')
        setter = setter_function
    getset.append(f'{{ "{attribute.name}", {getter_name}, {setter}, nullptr, nullptr }},')
    bound.append(f'{{ "{where}", {concept}<{klass}>, {caller_realm(attribute)} }},')


def constructor_calls(interface: Interface, constructor, call_arguments: List[str]) -> List[str]:
    """The static factories the JS glue tries for a constructor, in its order (constructors.py), with
    the document's realm for the new object's; an Event's also take its time stamp."""
    args = ", ".join(call_arguments)
    with_args = (lambda first: f"{first}, {args}" if args else first)
    is_event = any(parent.name == "Event" for parent in context.inheritance_stack(interface))
    time_stamp = "Web::HighResolutionTime::current_high_resolution_time(Web::HTML::relevant_global_object(realm.global_object()))"
    calls = []
    implemented_as = constructor.extended_attributes.get("ImplementedAs")
    if implemented_as:
        calls += [f"I::{implemented_as}({with_args('realm')})", f"I::{implemented_as}({with_args('realm.vm()')})",
                  f"I::{implemented_as}({args})"]
    calls += [f"I::construct_impl({with_args('realm')})", f"I::construct_impl({with_args('realm.vm()')})",
              f"I::construct_impl({args})",
              f"I::create_for_constructor({with_args('realm.global_object()')})",
              f"I::create_for_constructor({with_args('realm')})", f"I::create_for_constructor({with_args('realm.vm()')})",
              f"I::create_for_constructor({args})"]
    if is_event:
        calls.append(f"I::create_for_constructor({args + ', ' if args else ''}{time_stamp})")
    calls += [f"I::create_from_numberish({args})", f"I::create({with_args('realm')})", f"I::create({with_args('realm.vm()')})"]
    if is_event:
        calls.append(f"I::create({args + ', ' if args else ''}{time_stamp})")
    calls.append(f"I::create({args})")
    return calls


def write_operation_group(chunk: Chunk, interface: Interface, name: str, overloads, methods: List[str], bound: List[str],
                          mode: str = "method") -> Optional[str]:
    """An operation's overloads, by argument count. `mode` is "method", "static" (a class method:
    `URL.canParse(...)`) or "constructor" (the type's tp_new calls it, through dom.cpp); static and
    constructor glue run in the context dom.cpp's static_context gives the type. Returns the
    dispatcher's name."""
    out = chunk.out
    where = f"{interface.name}.{name}"
    klass = cpp_class(interface)
    usable = []
    for overload in overloads:
        if mode == "constructor":
            if "HTMLConstructor" in overload.extended_attributes:
                report.append(f"{where}: [HTMLConstructor] (custom elements defined in Python)")
                continue
        else:
            reason = why_unsupported(overload.return_type)
            if reason:
                report.append(f"{where}: returns {reason}")
                continue
        if mode != "method" and "ImplementedInBindings" in overload.extended_attributes:
            report.append(f"{where}: [ImplementedInBindings] {mode}")
            continue
        parameters = list(overload.parameters)
        defaulted: List[str] = []
        while parameters and parameters[-1].optional and why_unsupported(parameters[-1].type, returning=False):
            omitted = omitted_argument(parameters[-1])
            if omitted is None:
                break
            dropped = parameters.pop()
            defaulted.insert(0, omitted)
            report.append(f"{where}: optional argument {dropped.name} ({why_unsupported(dropped.type, returning=False)}) is always its default")
        overload = replace(overload, parameters=parameters, extended_attributes={**overload.extended_attributes, "karouka-defaulted": "\x1f".join(defaulted)})
        bad = next((why_unsupported(p.type, returning=False) for p in overload.parameters if why_unsupported(p.type, returning=False)), None)
        if bad:
            report.append(f"{where}({', '.join(p.name for p in overload.parameters)}): argument type {bad}")
            continue
        usable.append(overload)
    if not usable:
        return None

    def arity(operation):
        required = sum(1 for p in operation.parameters if not p.optional and not p.variadic)
        variadic = any(p.variadic for p in operation.parameters)
        return required, (10_000 if variadic else len(operation.parameters))

    # Overloads whose argument counts overlap are told apart at run time by their arguments'
    # Python kinds (bindings.h's plausible); the first plausible one in IDL order is called.
    chosen = [(arity(op), op) for op in usable]

    function = ident(interface.name, {"method": name, "static": f"static_{name}", "constructor": "construct"}[mode])
    variants = []
    for number, ((low, high), operation) in enumerate(chosen):
        variant = f"{function}_{number}"
        body = StringIO()
        names = write_arguments(body, chunk, where, operation.parameters)
        if names is None:
            continue
        if mode != "constructor":
            collect_enums(operation.return_type, chunk.enums)
            referenced_interfaces(operation.return_type, chunk)
        for parameter in operation.parameters:
            referenced_interfaces(parameter.type, chunk)
        if "ImplementedInBindings" in operation.extended_attributes:
            header = glue_header(interface)
            if header is None:
                report.append(f"{where}: [ImplementedInBindings] with no bindings glue header")
                continue
            chunk.includes.add(header)
            chunk.includes.update(KEPT_GLUE_HEADERS)
        defaulted = [a for a in operation.extended_attributes.get("karouka-defaulted", "").split("\x1f") if a]
        if mode == "constructor":
            calls = constructor_calls(interface, operation, names + defaulted)
            chunk.includes.update(["LibWeb/HTML/Scripting/Environments.h", "LibWeb/HighResolutionTime/TimeOrigin.h"])
        else:
            calls = call_alternatives(interface, operation, idl_implementation_cpp_name(operation), names + defaulted,
                                      receiver="I::" if mode == "static" else "impl->")
        promise = mode != "constructor" and "CreatesPromise" in operation.extended_attributes
        concept = f"bound_{variant}"
        write_bound_concept(out, concept, calls, parameter_types(operation.parameters, promise))
        out.write(f"template<typename I = {klass}>\nstatic PyObject* {variant}(PyObject* self, [[maybe_unused]] PyObject* const* args, [[maybe_unused]] Py_ssize_t nargs)\n{{\n")
        out.write(f"    {'BEGIN(I)' if mode == 'method' else 'BEGIN_STATIC()'};\n    [[maybe_unused]] auto& realm = realm_of(*context);\n")
        out.write(body.getvalue())
        if promise:
            out.write("    auto promise = Web::WebIDL::create_promise(realm);\n")
            write_dispatch(out, calls, f'unbound("{where}")', result="promise_result(*context, realm, promise, ")
        else:
            write_dispatch(out, calls, f'unbound("{where}")')
        out.write("}\n\n")
        check = " && ".join(f"(nargs <= {i} || plausible<{t.rsplit('&', 1)[0]}>(*context, args[{i}]))"
                            for i, t in enumerate(parameter_types(operation.parameters))
                            if not operation.parameters[i].variadic)
        variants.append((low, high, variant, check or "true"))
        count = len(operation.parameters)
        label = f"{where}/{count}"
        if sum(1 for _, other in chosen if len(other.parameters) == count) > 1:
            # Same-count overloads: postMessage/2(message, transfer) and postMessage/2(message, options).
            label += f"({', '.join(p.name for p in operation.parameters)})"
        bound.append(f'{{ "{label}", {concept}<{klass}>, {caller_realm(operation)} }},')
    if not variants:
        return None
    out.write(f"static PyObject* {function}(PyObject* self, [[maybe_unused]] PyObject* const* args, [[maybe_unused]] Py_ssize_t nargs)\n{{\n")
    def overlaps(a, b):
        return a is not b and a[0] <= b[1] and b[0] <= a[1]
    if any(overlaps(a, b) for a in variants for b in variants):
        out.write(f"    Context* context = {'live_context' if mode == 'method' else 'static_context'}(self);\n    if (!context)\n        return nullptr;\n")
        for low, high, variant, check in variants:
            out.write(f"    if (nargs >= {low} && nargs <= {high} && {check})\n        return {variant}<>(self, args, nargs);\n")
    for low, high, variant, check in variants:
        out.write(f"    if (nargs >= {low} && nargs <= {high})\n        return {variant}<>(self, args, nargs);\n")
    low_all = min(v[0] for v in variants)
    high_all = max(v[1] for v in variants)
    if len(chosen) > 1 and any(overlaps(a, b) for a in variants for b in variants):
        report.append(f"{where}: overloads of overlapping argument counts, chosen by argument kind")
    out.write(f'    return wrong_argument_count("{where}", nargs, {low_all}, {high_all});\n}}\n\n')
    if mode != "constructor":
        flags = "METH_FASTCALL | METH_CLASS" if mode == "static" else "METH_FASTCALL"
        methods.append(f'{{ "{name}", reinterpret_cast<PyCFunction>(reinterpret_cast<void(*)()>({function})), {flags}, nullptr }},')
    return function


# The C++ the JS glue's named_item_value calls (named_and_indexed_properties.py), where it isn't the
# getter's own name.
NAMED_GETTER_CALLS = {
    "DOMStringMap": "impl->determine_value_of_named_property(name)",
    "Storage": "impl->get_item(name)",
    "HTMLAllCollection": "impl->named_item(name)",
    "HTMLFormControlsCollection": "impl->named_item_or_radio_node_list(name)",
    "HTMLFormElement": "impl->named_item_or_radio_node_list(name)",
    "Document": "Web::Bindings::document_named_item_value(Web::Bindings::host_defined_wrapper_world(realm), realm, *impl, name)",
    "Window": "Web::Bindings::window_named_item_value(Web::Bindings::host_defined_wrapper_world(realm), realm, *impl, name)",
}


def write_named_getter(chunk: Chunk, interface: Interface, bound: List[str]) -> Optional[str]:
    """`obj[name]` for a str: the named getter (KeyError for no such property, as JS's undefined);
    any other key goes to the sequence's item (the indexed getter)."""
    getter = interface.named_property_getter
    where = f"{interface.name}[name]"
    if why_unsupported(getter.return_type) and interface.name not in NAMED_GETTER_CALLS:
        report.append(f"{where}: returns {why_unsupported(getter.return_type)}")
        return None
    out = chunk.out
    klass = cpp_class(interface)
    function = ident(interface.name, "subscript")
    method = idl_implementation_cpp_name(getter) if getter.name else None
    call = NAMED_GETTER_CALLS.get(interface.name) or (f"impl->{method}(name)" if method else None)
    if call is None:
        report.append(f"{where}: named getter with no C++ method")
        return None
    if interface.name in ("Document", "Window"):
        chunk.includes.update(KEPT_GLUE_HEADERS)
        chunk.includes.add("LibWeb/Bindings/WrapperWorld.h")
    collect_enums(getter.return_type, chunk.enums)
    referenced_interfaces(getter.return_type, chunk)
    names = {"Utf16FlyString": "name16", "FlyString": "name8", "String": "name8s"}
    calls = [call.replace("(name)", f"({variable})").replace(", name)", f", {variable})") for variable in names.values()]
    write_bound_concept(out, f"bound_{function}", calls, ["Utf16FlyString const& name16", "FlyString const& name8", "String const& name8s"])
    out.write(f"template<typename I = {klass}>\nstatic PyObject* {function}(PyObject* self, PyObject* key)\n{{\n")
    out.write("    if (!PyUnicode_Check(key))\n        return sequence_subscript(self, key);\n")
    out.write("    auto utf8 = utf8_of(key, \"a property name\");\n    if (!utf8.has_value())\n        return nullptr;\n")
    out.write("    BEGIN(I);\n    [[maybe_unused]] auto& realm = realm_of(*context);\n")
    out.write("    [[maybe_unused]] auto name16 = Utf16FlyString::from_utf8(*utf8);\n")
    out.write("    [[maybe_unused]] auto name8 = FlyString::from_utf8_without_validation(utf8->bytes());\n")
    out.write("    [[maybe_unused]] auto name8s = String::from_utf8_without_validation(utf8->bytes());\n")
    # As WebIDL's legacy platform object: only a supported property name has a value (the C++
    # getters VERIFY it, DOMStringMap's among them).
    out.write("    if constexpr (requires { impl->is_supported_property_name(name16); }) {\n")
    out.write("        if (!impl->is_supported_property_name(name16)) {\n            PyErr_SetObject(PyExc_KeyError, key);\n            return nullptr;\n        }\n    }\n")
    write_dispatch(out, calls, f'unbound("{where}")', result="named_result(*context, key, ")
    out.write("}\n\n")
    bound.append(f'{{ "{where}", bound_{function}<{klass}>, false }},')
    return f"{function}<>"


def write_interface(chunk: Chunk, interface: Interface) -> str:
    CURRENT_CHUNK[0] = chunk
    out = chunk.out
    if not interface.is_callback_interface:
        chunk.includes.add(implementation_header_for_interface(interface))
    klass = cpp_class(interface)
    tag = ident(interface.name)
    out.write(f"// ---- {interface.name} ({klass}) ----\n\n")
    getset: List[str] = []
    methods: List[str] = []
    bound: List[str] = []
    constants: List[str] = []
    construct = "nullptr"
    mapping = "nullptr"

    if not interface.is_callback_interface:
        attribute_names: set[str] = set()
        for attribute in interface.regular_attributes:
            if attribute.name in attribute_names:
                continue
            attribute_names.add(attribute.name)
            write_attribute(chunk, interface, attribute, getset, bound)
        groups: dict[str, list] = {}
        for operation in interface.regular_operations:
            if operation.name:
                groups.setdefault(operation.name, []).append(operation)
        for name, overloads in groups.items():
            write_operation_group(chunk, interface, name, overloads, methods, bound)
        for attribute in interface.static_attributes:
            report.append(f"{interface.name}.{attribute.name}: static attribute")
        static_groups: dict[str, list] = {}
        for operation in interface.static_operations:
            static_groups.setdefault(operation.name, []).append(operation)
        for name, overloads in static_groups.items():
            if name in groups:
                # Python has one attribute per name: Response.json(data) vs response.json().
                report.append(f"{interface.name}.{name}: static operation, shadowed by the regular operation of its name")
                continue
            write_operation_group(chunk, interface, name, overloads, methods, bound, mode="static")
        if interface.constructors:
            constructors = [Operation(name="constructor", return_type=IDLType(interface.name), parameters=c.parameters,
                                      extended_attributes=c.extended_attributes) for c in interface.constructors]
            construct = write_operation_group(chunk, interface, "constructor", constructors, methods, bound, mode="constructor") or "nullptr"
        if interface.named_property_getter is not None:
            mapping = write_named_getter(chunk, interface, bound) or "nullptr"
        for kind in ("named_property_setter", "named_property_deleter",
                     "indexed_property_setter", "maplike", "setlike", "async_iterable"):
            if getattr(interface, kind) is not None:
                report.append(f"{interface.name}: {kind.replace('_', ' ')}")
        if interface.iterable is not None and interface.iterable.key_type is not None:
            report.append(f"{interface.name}: pair iterable")

    for constant in interface.constants:
        value = constant.value
        if constant.type.name in ("boolean",):
            constants.append(f'{{ "{constant.name}", {1 if value == "true" else 0}, true }},')
        else:
            constants.append(f'{{ "{constant.name}", static_cast<long long>({value}), false }},')

    # Indexed getter: the sequence protocol (len, [i], iteration, in) over length and the getter.
    sequence = "nullptr, nullptr"
    getter = interface.indexed_property_getter
    if getter is not None and getter.name and not why_unsupported(getter.return_type) and not interface.is_callback_interface:
        method = idl_implementation_cpp_name(getter)
        collect_enums(getter.return_type, chunk.enums)
        referenced_interfaces(getter.return_type, chunk)
        out.write(f"template<typename I = {klass}>\nstatic Py_ssize_t {tag}_sq_length(PyObject* self)\n{{\n")
        out.write("    Context* context = live_context(self);\n    if (!context)\n        return -1;\n")
        out.write("    if constexpr (std::is_base_of_v<GC::Cell, I> && requires(I* impl) { impl->length(); })\n")
        out.write("        return static_cast<Py_ssize_t>(cell_of<I>(self)->length());\n")
        out.write(f'    else {{\n        unbound("{interface.name}.length");\n        return -1;\n    }}\n}}\n\n')
        out.write(f"template<typename I = {klass}>\nstatic PyObject* {tag}_sq_item(PyObject* self, Py_ssize_t index)\n{{\n")
        out.write(f"    Py_ssize_t length = {tag}_sq_length<I>(self);\n    if (length < 0)\n        return nullptr;\n")
        out.write('    if (index < 0 || index >= length) {\n        PyErr_SetString(PyExc_IndexError, "index out of range");\n        return nullptr;\n    }\n')
        out.write("    BEGIN(I);\n")
        out.write(f"    if constexpr (std::is_base_of_v<GC::Cell, I> && requires {{ impl->{method}(static_cast<u32>(index)); }})\n")
        out.write(f"        return result_to_py(*context, [&]() -> decltype(auto) {{ return impl->{method}(static_cast<u32>(index)); }});\n")
        out.write(f'    else\n        return unbound("{interface.name}[index]");\n}}\n\n')
        sequence = f"{tag}_sq_length<>, {tag}_sq_item<>"

    stringifier = "nullptr"
    if interface.stringifier is not None and interface.stringifier.attribute is not None:
        attribute_function = ident(interface.name, "get", interface.stringifier.attribute.name)
        if any(f'"{interface.stringifier.attribute.name}", {attribute_function}' in entry for entry in getset):
            out.write(f"static PyObject* {tag}_str(PyObject* self)\n{{\n    return {attribute_function}")
            out.write(f"{'<>' if 'Reflect' not in interface.stringifier.attribute.extended_attributes else ''}(self, nullptr);\n}}\n\n")
            stringifier = f"{tag}_str"

    out.write(f"extern ConstantDefinition const g_{tag}_constants[];\nextern BoundMember const g_{tag}_bound[];\n")
    out.write(f"extern SequenceFunctions const g_{tag}_sequence;\nextern reprfunc const g_{tag}_str;\n")
    out.write(f"extern PyCFunctionFast const g_{tag}_construct;\nextern binaryfunc const g_{tag}_subscript;\n")
    out.write(f"PyGetSetDef g_{tag}_getset[] = {{\n")
    for entry in getset:
        out.write(f"    {entry}\n")
    out.write("    { nullptr, nullptr, nullptr, nullptr, nullptr },\n};\n")
    out.write(f"PyMethodDef g_{tag}_methods[] = {{\n")
    for entry in methods:
        out.write(f"    {entry}\n")
    out.write("    { nullptr, nullptr, 0, nullptr },\n};\n")
    out.write(f"ConstantDefinition const g_{tag}_constants[] = {{\n")
    for entry in constants:
        out.write(f"    {entry}\n")
    out.write("    { nullptr, 0, false },\n};\n")
    out.write(f"BoundMember const g_{tag}_bound[] = {{\n")
    for entry in bound:
        out.write(f"    {entry}\n")
    out.write("    { nullptr, false, false },\n};\n")
    out.write(f"SequenceFunctions const g_{tag}_sequence = {{ {sequence} }};\n")
    out.write(f"reprfunc const g_{tag}_str = {stringifier};\n")
    out.write(f"PyCFunctionFast const g_{tag}_construct = {construct};\n")
    out.write(f"binaryfunc const g_{tag}_subscript = {mapping};\n\n")
    return tag


# ---- Output ----

def write_if_changed(path: Path, contents: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == contents:
        return
    path.write_text(contents, encoding="utf-8")


output = arguments.output_path
output.mkdir(parents=True, exist_ok=True)

header = StringIO()
header.write("// Generated by src/python/gen/generate_python_bindings.py. Do not edit.\n#pragma once\n\n")
header.write("#include <AK/Types.h>\n#include <LibWeb/Bindings/Forward.h>\n\nnamespace Karouka::Python {\n\n")
header.write("enum class DomType : u16 {\n")
for interface in ordered:
    header.write(f"    {interface.name},\n")
header.write("    Count,\n};\n\n")
header.write("template<typename T>\nstruct DomTypeOf;\n")
for interface in ordered:
    if interface.is_callback_interface:
        continue
    header.write(f"template<>\nstruct DomTypeOf<{cpp_class(interface)}> {{ static constexpr DomType value = DomType::{interface.name}; }};\n")
header.write("\n}\n")

chunk_count = max(1, arguments.chunks)
chunks = [Chunk(number) for number in range(chunk_count)]
tags: List[str] = []
for number, interface in enumerate(ordered):
    chunk = chunks[number * chunk_count // len(ordered)]
    tags.append(write_interface(chunk, interface))

enum_definitions = {enumeration.name: enumeration for enumeration in context.enumerations.values()}
enum_modules = {}
for module in modules:
    for enumeration in module.enumerations:
        enum_modules[enumeration.name] = module.path.stem

for chunk in chunks:
    text = StringIO()
    text.write("// Generated by src/python/gen/generate_python_bindings.py. Do not edit.\n\n")
    text.write('#include "bindings.h"\n\n')
    for include in sorted(chunk.includes):
        text.write(f"#include <{include}>\n")
    for stem in sorted({enum_modules[name] for name in chunk.enums if name in enum_modules} | chunk.binding_stems):
        text.write(f"#include <LibWeb/Bindings/{stem}.h>\n")
    text.write("\nusing namespace Web;\n\nnamespace Web::Bindings {\n\n")
    for name in sorted(chunk.enums):
        enumeration = enum_definitions[name]
        text.write(f"[[maybe_unused]] inline bool karouka_enum_from_string(Utf16View const& value, {name}& out)\n{{\n")
        for value in enumeration.values:
            text.write(f'    if (value == u"{value}"sv) {{\n        out = {name}::{string_to_cpp_enum_name(value)};\n        return true;\n    }}\n')
        text.write("    return false;\n}\n\n")
    for name in sorted(chunk.dictionaries):
        # LibWeb's own dictionary converter, of the JS value a Python dict (or None) becomes
        # (bindings.h's convert): found by ADL, exactly by type (no derived dictionary matches).
        text.write(f"[[maybe_unused]] inline JS::ThrowCompletionOr<{name}> karouka_dictionary_from_js(JS::VM& vm, JS::Value value, std::type_identity<{name}>)\n{{\n")
        text.write(f"    return {converter_function_name(context.dictionaries[name])}(vm, value);\n}}\n\n")
    text.write("}\n\nnamespace Karouka::Python {\n\n")
    text.write(chunk.out.getvalue())
    text.write("}\n")
    write_if_changed(output / f"PythonBindings{chunk.number}.cpp", text.getvalue())

table = StringIO()
table.write("// Generated by src/python/gen/generate_python_bindings.py. Do not edit.\n\n")
table.write('#include "bindings.h"\n\nnamespace Karouka::Python {\n\n')
for tag in tags:
    table.write(f"extern PyGetSetDef g_{tag}_getset[];\nextern PyMethodDef g_{tag}_methods[];\n")
    table.write(f"extern ConstantDefinition const g_{tag}_constants[];\nextern BoundMember const g_{tag}_bound[];\n")
    table.write(f"extern SequenceFunctions const g_{tag}_sequence;\nextern reprfunc const g_{tag}_str;\n")
    table.write(f"extern PyCFunctionFast const g_{tag}_construct;\nextern binaryfunc const g_{tag}_subscript;\n")
table.write("\nGeneratedType const g_generated_types[] = {\n")
for interface, tag in zip(ordered, tags):
    parent = f"DomType::{interface.parent_name}" if interface.parent_name in index_of else "DomType::Count"
    exposed = "true" if window_exposed(interface) and "LegacyNoInterfaceObject" not in interface.extended_attributes else "false"
    table.write(f'    {{ "_karouka.{interface.name}", "{interface.name}", "{interface.implemented_name}", {parent}, {exposed}, '
                f"g_{tag}_getset, g_{tag}_methods, g_{tag}_constants, g_{tag}_bound, &g_{tag}_sequence, &g_{tag}_str, "
                f"&g_{tag}_construct, &g_{tag}_subscript }},\n")
table.write("};\n\n")
table.write("// Other names an interface has on window: [LegacyWindowAlias] and [LegacyFactoryFunction].\n")
table.write("WindowAlias const g_window_aliases[] = {\n")
for interface in ordered:
    if not window_exposed(interface):
        continue
    names = []
    alias = interface.extended_attributes.get("LegacyWindowAlias")
    if alias:
        names += [name.strip() for name in alias.strip("()").split(",")]
    factory = interface.extended_attributes.get("LegacyFactoryFunction")
    if factory:
        names.append(factory.split("(")[0].strip())
    for name in names:
        table.write(f'    {{ "{name}", DomType::{interface.name} }},\n')
table.write("    { nullptr, DomType::Count },\n};\n\n}\n")
write_if_changed(output / "PythonBindingsTable.cpp", table.getvalue())
write_if_changed(output / "PythonBindings.h", header.getvalue())
write_if_changed(output / "PythonBindings.report", "\n".join(sorted(set(report))) + "\n")

if arguments.depfile is not None:
    outputs = [str(output / "PythonBindings.h"), str(output / "PythonBindingsTable.cpp")]
    outputs += [str(output / f"PythonBindings{chunk.number}.cpp") for chunk in chunks]
    dependencies = [str(path) for path in input_paths + arguments.pruned_idl_file]
    arguments.depfile.write_text(" ".join(outputs) + ": " + " \\\n ".join(dependencies) + "\n")
