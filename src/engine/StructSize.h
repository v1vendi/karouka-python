/*
 * struct_size versioning of the public structs (karouka.h, "versioned by a leading struct_size"):
 * the host's struct holds the members that end within its struct_size, and karouka reads or writes
 * only those. Each struct lists its members after struct_size, in order, as a StructMembers array;
 * lists_every_member checks at compile time that the list reaches the end of the struct, so a
 * member added to it without a line here fails the build (one that fits in the struct's tail
 * padding, such as a bool after a bool, is not caught: list it).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Karouka {

struct StructMember {
    size_t offset;
    size_t size;
    constexpr size_t end() const { return offset + size; }
};

#define KR_STRUCT_MEMBER(type, member) (::Karouka::StructMember { offsetof(type, member), sizeof(type::member) })

template<typename T, size_t N>
constexpr bool lists_every_member(StructMember const (&members)[N])
{
    if (members[0].offset < sizeof(uint32_t))
        return false;
    for (size_t i = 1; i < N; ++i) {
        if (members[i].offset < members[i - 1].end())
            return false;
    }
    size_t end = members[N - 1].end();
    return (end + alignof(T) - 1) / alignof(T) * alignof(T) == sizeof(T);
}

// Where the members that end within struct_size end: whole members only, since a struct_size
// between members (no sizeof is one) must take no bytes of the next. 0 when not even the first fits.
template<size_t N>
constexpr size_t whole_members_end(uint32_t struct_size, StructMember const (&members)[N])
{
    size_t end = 0;
    for (auto const& member : members) {
        if (member.end() <= struct_size)
            end = member.end();
    }
    return end;
}

// A descriptor the host filled: *out gets the members within in.struct_size, the rest zero (their
// defaults), and struct_size sizeof(T). false, *out untouched, when struct_size can't hold the first
// member. `in` may be a host's smaller struct: nothing past its struct_size is read.
template<typename T, size_t N>
bool read_sized(T const& in, StructMember const (&members)[N], T& out)
{
    size_t end = whole_members_end(in.struct_size, members);
    if (end == 0)
        return false;
    out = {};
    out.struct_size = sizeof(T);
    size_t begin = members[0].offset;
    memcpy(reinterpret_cast<unsigned char*>(&out) + begin, reinterpret_cast<unsigned char const*>(&in) + begin, end - begin);
    return true;
}

// A stats struct karouka fills: the members of `in` within out.struct_size are copied to `out`, and
// nothing else of the host's struct (its struct_size and a newer header's members past ours) is
// written. false, nothing written, when struct_size can't hold the first member.
template<typename T, size_t N>
bool write_sized(T const& in, StructMember const (&members)[N], T& out)
{
    size_t end = whole_members_end(out.struct_size, members);
    if (end == 0)
        return false;
    size_t begin = members[0].offset;
    memcpy(reinterpret_cast<unsigned char*>(&out) + begin, reinterpret_cast<unsigned char const*>(&in) + begin, end - begin);
    return true;
}

}
