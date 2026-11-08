/*
 * Unicode character name lookup for "\N{...}" escapes in string literals.
 *
 * Derived from CPython Modules/unicodedata.c (PSF-2.0): the name -> code
 * point half of the unicodedata name database (DAWG lookup, aliases, named
 * sequences, algorithmic HANGUL SYLLABLE / CJK UNIFIED IDEOGRAPH-XXXX and
 * the other derived names), exactly as reached from
 * _PyUnicode_DecodeUnicodeEscapeInternal2 through
 * _PyUnicode_Name_CAPI.getcode(name, namelen, &code, with_named_seq = 0).
 * The data in unicodename_db.h is a verbatim copy of CPython 3.15.2's
 * generated Modules/unicodename_db.h.
 */

#include <stdbool.h>

/* Unit-local CPython compatibility names (undefined at the end of this file). */
#define Py_UCS1 uint8_t
#define Py_UCS2 uint16_t
#define Py_UCS4 uint32_t
#define Py_ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))
#define Py_SAFE_DOWNCAST(VALUE, WIDE, NARROW) ((NARROW)(VALUE))
#define Py_TOUPPER(c) qp_un_toupper((unsigned char)(c))
#define PyOS_strnicmp qp_un_strnicmp

/* Py_TOUPPER: ASCII-only upper-casing of a byte. */
static inline unsigned char qp_un_toupper(unsigned char c)
{
    return (c >= 'a' && c <= 'z') ? (unsigned char)(c - ('a' - 'A')) : c;
}

/* Py_TOLOWER: ASCII-only lower-casing of a byte. */
static inline unsigned char qp_un_tolower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* PyOS_mystrnicmp (Python/pystrcmp.c): ASCII case-insensitive comparison of
   at most `size` bytes, stopping at a NUL byte in either string. */
static int qp_un_strnicmp(const char *s1, const char *s2, Py_ssize_t size)
{
    const unsigned char *p1, *p2;
    if (size == 0)
        return 0;
    p1 = (const unsigned char *)s1;
    p2 = (const unsigned char *)s2;
    for (; (--size > 0) && *p1 && *p2 && (qp_un_tolower(*p1) == qp_un_tolower(*p2));
         p1++, p2++) {
        ;
    }
    return qp_un_tolower(*p1) - qp_un_tolower(*p2);
}

/* The database also carries tables only needed for the code point -> name
   direction (and named sequence expansion), which "\N{...}" never uses. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "unicodename_db.h"
#pragma GCC diagnostic pop

// For Hangul decomposition
#define SBase   0xAC00
#define LBase   0x1100
#define VBase   0x1161
#define TBase   0x11A7
#define LCount  19
#define VCount  21
#define TCount  28
#define NCount  (VCount*TCount)
#define SCount  (LCount*NCount)


static const char * const hangul_syllables[][3] = {
    { "G",  "A",   ""   },
    { "GG", "AE",  "G"  },
    { "N",  "YA",  "GG" },
    { "D",  "YAE", "GS" },
    { "DD", "EO",  "N", },
    { "R",  "E",   "NJ" },
    { "M",  "YEO", "NH" },
    { "B",  "YE",  "D"  },
    { "BB", "O",   "L"  },
    { "S",  "WA",  "LG" },
    { "SS", "WAE", "LM" },
    { "",   "OE",  "LB" },
    { "J",  "YO",  "LS" },
    { "JJ", "U",   "LT" },
    { "C",  "WEO", "LP" },
    { "K",  "WE",  "LH" },
    { "T",  "WI",  "M"  },
    { "P",  "YU",  "B"  },
    { "H",  "EU",  "BS" },
    { 0,    "YI",  "S"  },
    { 0,    "I",   "SS" },
    { 0,    0,     "NG" },
    { 0,    0,     "J"  },
    { 0,    0,     "C"  },
    { 0,    0,     "K"  },
    { 0,    0,     "T"  },
    { 0,    0,     "P"  },
    { 0,    0,     "H"  }
};

static int
find_prefix_id(Py_UCS4 code)
{
    for (int i = 0; i < (int)Py_ARRAY_LENGTH(derived_name_ranges); i++) {
        if (code < derived_name_ranges[i].first) {
            return -1;
        }
        if (code <= derived_name_ranges[i].last) {
            return derived_name_ranges[i].prefixid;
        }
    }
    return -1;
}

/* macros used to determine if the given code point is in the PUA range that
 * we are using to store aliases and named sequences */
#define IS_ALIAS(cp) ((cp >= aliases_start) && (cp < aliases_end))
#define IS_NAMED_SEQ(cp) ((cp >= named_sequences_start) && \
                          (cp < named_sequences_end))

// DAWG decoding functions

static unsigned int
_dawg_decode_varint_unsigned(unsigned int index, unsigned int* result)
{
    unsigned int res = 0;
    unsigned int shift = 0;
    for (;;) {
        unsigned char byte = packed_name_dawg[index];
        res |= (byte & 0x7f) << shift;
        index++;
        shift += 7;
        if (!(byte & 0x80)) {
            *result = res;
            return index;
        }
    }
}

static int
_dawg_match_edge(const char* name, unsigned int namelen, unsigned int size,
                 unsigned int label_offset, unsigned int namepos)
{
    // This returns 1 if the edge matched, 0 if it didn't (but further edges
    // could match) and -1 if the name cannot match at all.
    if (size > 1 && namepos + size > namelen) {
        return 0;
    }
    for (unsigned int i = 0; i < size; i++) {
        if (packed_name_dawg[label_offset + i] != Py_TOUPPER(name[namepos + i])) {
            if (i > 0) {
                return -1; // cannot match at all
            }
            return 0;
        }
    }
    return 1;
}

// reading DAWG node information:
// a node is encoded by a varint. The lowest bit of that int is set if the node
// is a final, accepting state. The higher bits of that int represent the
// number of names that are encoded by the sub-DAWG started by this node. It's
// used to compute the position of a name.
//
// the starting node of the DAWG is at position 0.
//
// the varint representing a node is followed by the node's edges, the encoding
// is described below


static unsigned int
_dawg_decode_node(unsigned int node_offset, bool* final)
{
    unsigned int num;
    node_offset = _dawg_decode_varint_unsigned(node_offset, &num);
    *final = num & 1;
    return node_offset;
}

static bool
_dawg_node_is_final(unsigned int node_offset)
{
    unsigned int num;
    _dawg_decode_varint_unsigned(node_offset, &num);
    return num & 1;
}

static unsigned int
_dawg_node_descendant_count(unsigned int node_offset)
{
    unsigned int num;
    _dawg_decode_varint_unsigned(node_offset, &num);
    return num >> 1;
}


// reading DAWG edge information:
// a DAWG edge is comprised of the following information:
// (1) the size of the label of the string attached to the edge
// (2) the characters of that edge
// (3) the target node
// (4) whether the edge is the last edge in the list of edges following a node
//
// this information is encoded in a compact form as follows:
//
// +---------+-----------------+--------------+--------------------
// |  varint | size (if != 1)  | label chars  | ... next edge ...
// +---------+-----------------+--------------+--------------------
//
// - first comes a varint
//     - the lowest bit of that varint is whether the edge is final (4)
//     - the second lowest bit of that varint is true if the size of
//       the length of the label is 1 (1)
//     - the rest of the varint is an offset that can be used to compute
//       the offset of the target node of that edge (3)
//  - if the size is not 1, the first varint is followed by a
//    character encoding the number of characters of the label (1)
//    (unicode character names aren't larger than 256 bytes, therefore each
//    edge label can be at most 256 chars, but is usually smaller)
//  - the next size bytes are the characters of the label (2)
//
// the offset of the target node is computed as follows: the number in the
// upper bits of the varint needs to be added to the offset of the target node
// of the previous edge. For the first edge, where there is no previous target
// node, the offset of the first edge is used.
// The intuition here is that edges going out from a node often lead to nodes
// that are close by, leading to small offsets from the current node and thus
// fewer bytes.
//
// There is a special case: if a final node has no outgoing edges, it has to be
// followed by a 0 byte to indicate that there are no edges (because the end of
// the edge list is normally indicated in a bit in the edge encoding). This is
// indicated by _dawg_decode_edge returning -1


static int
_dawg_decode_edge(bool is_first_edge, unsigned int prev_target_node_offset,
                  unsigned int edge_offset, unsigned int* size,
                  unsigned int* label_offset, unsigned int* target_node_offset)
{
    unsigned int num;
    edge_offset = _dawg_decode_varint_unsigned(edge_offset, &num);
    if (num == 0 && is_first_edge) {
        return -1; // trying to decode past a final node without outgoing edges
    }
    bool last_edge = num & 1;
    num >>= 1;
    bool len_is_one = num & 1;
    num >>= 1;
    *target_node_offset = prev_target_node_offset + num;
    if (len_is_one) {
        *size = 1;
    } else {
        *size = packed_name_dawg[edge_offset++];
    }
    *label_offset = edge_offset;
    return last_edge;
}

static int
_lookup_dawg_packed(const char* name, unsigned int namelen)
{
    unsigned int stringpos = 0;
    unsigned int node_offset = 0;
    unsigned int result = 0; // this is the number of final nodes that we skipped to match name
    while (stringpos < namelen) {
        bool final;
        unsigned int edge_offset = _dawg_decode_node(node_offset, &final);
        unsigned int prev_target_node_offset = edge_offset;
        bool is_first_edge = true;
        for (;;) {
            unsigned int size;
            unsigned int label_offset, target_node_offset;
            int last_edge = _dawg_decode_edge(
                    is_first_edge, prev_target_node_offset, edge_offset,
                    &size, &label_offset, &target_node_offset);
            if (last_edge == -1) {
                return -1;
            }
            is_first_edge = false;
            prev_target_node_offset = target_node_offset;
            int matched = _dawg_match_edge(name, namelen, size, label_offset, stringpos);
            if (matched == -1) {
                return -1;
            }
            if (matched) {
                if (final)
                    result += 1;
                stringpos += size;
                node_offset = target_node_offset;
                break;
            }
            if (last_edge) {
                return -1;
            }
            result += _dawg_node_descendant_count(target_node_offset);
            edge_offset = label_offset + size;
        }
    }
    if (_dawg_node_is_final(node_offset)) {
        return result;
    }
    return -1;
}

static void
find_syllable(const char *str, int *len, int *pos, int count, int column)
{
    int i, len1;
    *len = -1;
    for (i = 0; i < count; i++) {
        const char *s = hangul_syllables[i][column];
        len1 = Py_SAFE_DOWNCAST(strlen(s), size_t, int);
        if (len1 <= *len)
            continue;
        if (PyOS_strnicmp(str, s, len1) == 0) {
            *len = len1;
            *pos = i;
        }
    }
    if (*len == -1) {
        *len = 0;
    }
}

static int
_check_alias_and_seq(Py_UCS4* code, int with_named_seq)
{
    /* check if named sequences are allowed */
    if (!with_named_seq && IS_NAMED_SEQ(*code))
        return 0;
    /* if the code point is in the PUA range that we use for aliases,
     * convert it to obtain the right code point */
    if (IS_ALIAS(*code))
        *code = name_aliases[*code-aliases_start];
    return 1;
}

static Py_UCS4
parse_hex_code(const char *name, int namelen)
{
    if (namelen < 4 || namelen > 6) {
        return (Py_UCS4)-1;
    }
    if (*name == '0') {
        return (Py_UCS4)-1;
    }
    int v = 0;
    while (namelen--) {
        v *= 16;
        Py_UCS1 c = Py_TOUPPER(*name);
        if (c >= '0' && c <= '9') {
            v += c - '0';
        }
        else if (c >= 'A' && c <= 'F') {
            v += c - 'A' + 10;
        }
        else {
            return (Py_UCS4)-1;
        }
        name++;
    }
    if (v > 0x10ffff) {
        return (Py_UCS4)-1;
    }
    return v;
}

static int
_getcode(const char* name, int namelen, Py_UCS4* code)
{
    /* Return the code point associated with the given name.
     * Named aliases are not resolved, they are returned as a code point in the
     * PUA */

    int i = 0;
    size_t prefixlen;
    for (; i < (int)Py_ARRAY_LENGTH(derived_name_prefixes); i++) {
        const char *prefix = derived_name_prefixes[i];
        prefixlen = strlen(derived_name_prefixes[i]);
        if (PyOS_strnicmp(name, prefix, prefixlen) == 0) {
            break;
        }
    }

    if (i == 0) {
        /* Hangul syllables. */
        assert(PyOS_strnicmp(name, "HANGUL SYLLABLE ", 16) == 0);
        int len, L = -1, V = -1, T = -1;
        const char *pos = name + 16;
        find_syllable(pos, &len, &L, LCount, 0);
        pos += len;
        find_syllable(pos, &len, &V, VCount, 1);
        pos += len;
        find_syllable(pos, &len, &T, TCount, 2);
        pos += len;
        if (L != -1 && V != -1 && T != -1 && pos-name == namelen) {
            *code = SBase + (L*VCount+V)*TCount + T;
            return 1;
        }
        /* Otherwise, it's an illegal syllable name. */
        return 0;
    }

    if (i < (int)Py_ARRAY_LENGTH(derived_name_prefixes)) {
        Py_UCS4 v = parse_hex_code(name + prefixlen, namelen - (int)prefixlen);
        if (find_prefix_id(v) != i) {
            return 0;
        }
        *code = v;
        return 1;
    }

    assert(namelen >= 0);
    int position = _lookup_dawg_packed(name, Py_SAFE_DOWNCAST(namelen, int, unsigned int));
    if (position < 0) {
        return 0;
    }
    *code = dawg_pos_to_codepoint[position];
    return 1;
}


static int
capi_getcode(const char* name, int namelen, Py_UCS4* code,
             int with_named_seq)
{
    if (!_getcode(name, namelen, code)) {
        return 0;
    }
    return _check_alias_and_seq(code, with_named_seq);
}

/* Look up the code point of the Unicode character name `name` (len bytes,
   case-insensitive) the way "\N{name}" does: standard names, name aliases
   and algorithmic names are accepted, named sequences are not.  As in
   CPython, the bytes following the name must be readable up to a NUL (the
   name always sits inside a NUL-terminated literal buffer).  Returns 1 and
   stores the code point in *cp, or 0 if the name is unknown. */
static int qp_unicode_lookup_name(const char *name, size_t len, uint32_t *cp)
{
    Py_UCS4 code = 0xffffffff;
    if (len > INT_MAX)
        return 0;
    if (!capi_getcode(name, (int)len, &code, 0))
        return 0;
    *cp = code;
    return 1;
}


static int
_inverse_dawg_lookup(char* buffer, unsigned int buflen, unsigned int pos)
{
    unsigned int node_offset = 0;
    unsigned int bufpos = 0;
    for (;;) {
        bool final;
        unsigned int edge_offset = _dawg_decode_node(node_offset, &final);

        if (final) {
            if (pos == 0) {
                if (bufpos + 1 == buflen) {
                    return 0;
                }
                buffer[bufpos] = '\0';
                return 1;
            }
            pos--;
        }
        unsigned int prev_target_node_offset = edge_offset;
        bool is_first_edge = true;
        for (;;) {
            unsigned int size;
            unsigned int label_offset, target_node_offset;
            int last_edge = _dawg_decode_edge(
                    is_first_edge, prev_target_node_offset, edge_offset,
                    &size, &label_offset, &target_node_offset);
            if (last_edge == -1) {
                return 0;
            }
            is_first_edge = false;
            prev_target_node_offset = target_node_offset;

            unsigned int descendant_count = _dawg_node_descendant_count(target_node_offset);
            if (pos < descendant_count) {
                if (bufpos + size >= buflen) {
                    return 0; // buffer overflow
                }
                for (unsigned int i = 0; i < size; i++) {
                    buffer[bufpos++] = packed_name_dawg[label_offset++];
                }
                node_offset = target_node_offset;
                break;
            } else if (!last_edge) {
                pos -= descendant_count;
                edge_offset = label_offset + size;
            } else {
                return 0;
            }
        }
    }
}

/* Name of code point `code` (unicodedata.name() without default): writes a
   NUL-terminated ASCII name into buffer (buflen >= NAME_MAXLEN is always
   enough) and returns 1, or returns 0 if the code point has no name. */
static int qp_unicode_get_name(uint32_t code, char *buffer, int buflen)
{
    int offset;

    if (code >= 0x110000)
        return 0;
    if (IS_ALIAS(code) || IS_NAMED_SEQ(code))
        return 0;

    int prefixid = find_prefix_id(code);
    if (prefixid == 0) {
        /* Hangul syllable. */
        int SIndex = code - SBase;
        int L = SIndex / NCount;
        int V = (SIndex % NCount) / TCount;
        int T = SIndex % TCount;

        if (buflen < 27)
            return 0;
        strcpy(buffer, "HANGUL SYLLABLE ");
        buffer += 16;
        strcpy(buffer, hangul_syllables[L][0]);
        buffer += strlen(hangul_syllables[L][0]);
        strcpy(buffer, hangul_syllables[V][1]);
        buffer += strlen(hangul_syllables[V][1]);
        strcpy(buffer, hangul_syllables[T][2]);
        buffer += strlen(hangul_syllables[T][2]);
        *buffer = '\0';
        return 1;
    }

    if (prefixid > 0) {
        const char *prefix = derived_name_prefixes[prefixid];
        if (snprintf(buffer, buflen, "%s%04X", prefix, (unsigned)code) >= buflen) {
            return 0;
        }
        return 1;
    }

    offset = dawg_codepoint_to_pos_index1[(code>>DAWG_CODEPOINT_TO_POS_SHIFT)];
    offset = dawg_codepoint_to_pos_index2[(offset<<DAWG_CODEPOINT_TO_POS_SHIFT) +
                               (code&((1<<DAWG_CODEPOINT_TO_POS_SHIFT)-1))];
    if (offset == DAWG_CODEPOINT_TO_POS_NOTFOUND)
        return 0;

    return _inverse_dawg_lookup(buffer, (unsigned int)buflen, offset);
}

#undef SBase
#undef LBase
#undef VBase
#undef TBase
#undef LCount
#undef VCount
#undef TCount
#undef NCount
#undef SCount
#undef IS_ALIAS
#undef IS_NAMED_SEQ
#undef NAME_MAXLEN
#undef DAWG_CODEPOINT_TO_POS_SHIFT
#undef DAWG_CODEPOINT_TO_POS_NOTFOUND
#undef Py_UCS1
#undef Py_UCS2
#undef Py_UCS4
#undef Py_ARRAY_LENGTH
#undef Py_SAFE_DOWNCAST
#undef Py_TOUPPER
#undef PyOS_strnicmp
