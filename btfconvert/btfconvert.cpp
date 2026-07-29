// btfconvert.exe - the two-panel .btf <-> .txt converter as a single native
// executable, so it can be handed to someone who has no Python.
//
// This is a port of tools/assets/btf.py, and a second implementation of a
// format is exactly the thing this project has been bitten by before, so the
// port is held to the same oracle the Python one is:
//
//     btfconvert.exe --selftest <media_soviet>
//
// reads every shipped language file, rebuilds it, and compares bytes; then
// dumps it to text, parses that back, rebuilds again, and compares bytes once
// more. On top of that, `--unpack` exists so the text this writes can be
// diffed against the text btf.py writes for the same input. The two agree byte
// for byte on all twenty-one files - that check, not "it compiles", is what
// makes the port trustworthy.
//
// The format, from C3D_LANGUAGE::Initialize (C3DDLL64.dll rva 0x96A50), all
// big-endian including the payload:
//
//     u32 count
//     u32 file size in bytes
//     u32 payload length in UTF-16 code units
//     count * { u32 id, u32 offset, u16 length }      <- 10 bytes on disk
//     payload: UTF-16BE, every string NUL-terminated, length excludes the NUL
//
// Offsets and lengths are code units, not bytes. Strings sit back to back in
// entry order with no padding and no de-duplication, which is why a rebuild
// reproduces the original exactly. See docs/02-findings.md.
//
// Working in wchar_t throughout is what makes the C++ side simpler than the
// Python one rather than harder: a Windows wchar_t *is* a UTF-16 code unit, so
// the payload is a byte swap and nothing else, and the length field is
// std::wstring::size().

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>       // CommandLineToArgvW; WIN32_LEAN_AND_MEAN drops it
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// Themed controls without a resource script - the same dependency an
// .rc-embedded manifest would declare. tesmiolauncher.cpp does this too.
#pragma comment(linker, "\"/MANIFESTDEPENDENCY:type='win32' "                  \
                        "name='Microsoft.Windows.Common-Controls' "            \
                        "version='6.0.0.0' processorArchitecture='*' "         \
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")

#define APP_TITLE L"btf converter"

struct Entry
{
    unsigned     id;
    std::wstring text;
};

typedef std::vector<unsigned char> Bytes;

// ---------------------------------------------------------------- small helpers

static std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return std::wstring(buf);
}

static const wchar_t* BaseName(const wchar_t* path)
{
    const wchar_t* p = path;
    for (const wchar_t* s = path; *s; s++)
        if (*s == L'\\' || *s == L'/') p = s + 1;
    return p;
}

// The extension swapped, so "a\b.btf" -> "a\b.txt". A dot in a directory name
// must not count, hence the scan back to the last separator first.
static std::wstring SwapExt(const std::wstring& path, const wchar_t* ext)
{
    size_t cut = path.size();
    for (size_t i = path.size(); i > 0; i--)
    {
        wchar_t c = path[i - 1];
        if (c == L'\\' || c == L'/') break;
        if (c == L'.') { cut = i - 1; break; }
    }
    return path.substr(0, cut) + ext;
}

static bool Exists(const wchar_t* path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool ReadWholeFile(const wchar_t* path, Bytes& out, std::wstring& err)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        err = Fmt(L"cannot open %s (error %lu)", BaseName(path), GetLastError());
        return false;
    }

    LARGE_INTEGER size = {0};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 64 * 1024 * 1024)
    {
        CloseHandle(h);
        err = Fmt(L"%s is not a size this reads (%lld bytes)",
                  BaseName(path), size.QuadPart);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() ||
              (ReadFile(h, &out[0], (DWORD)out.size(), &got, NULL) &&
               got == out.size());
    CloseHandle(h);
    if (!ok) err = Fmt(L"short read on %s", BaseName(path));
    return ok;
}

static bool WriteWholeFile(const wchar_t* path, const void* data, size_t n,
                           std::wstring& err)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        err = Fmt(L"cannot write %s (error %lu)", BaseName(path), GetLastError());
        return false;
    }
    DWORD put = 0;
    bool ok = (n == 0) ||
              (WriteFile(h, data, (DWORD)n, &put, NULL) && put == n);
    CloseHandle(h);
    if (!ok) err = Fmt(L"short write on %s", BaseName(path));
    return ok;
}

// ---------------------------------------------------------------- the container

static unsigned Be32(const unsigned char* p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static unsigned Be16(const unsigned char* p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static void PutBe32(Bytes& out, unsigned v)
{
    out.push_back((unsigned char)(v >> 24));
    out.push_back((unsigned char)(v >> 16));
    out.push_back((unsigned char)(v >> 8));
    out.push_back((unsigned char)v);
}

static void PutBe16(Bytes& out, unsigned v)
{
    out.push_back((unsigned char)(v >> 8));
    out.push_back((unsigned char)v);
}

static const size_t BTF_HDR = 12;
static const size_t BTF_REC = 10;

static bool ParseBtf(const Bytes& raw, const wchar_t* name,
                     std::vector<Entry>& out, std::wstring& err,
                     std::wstring* warn)
{
    out.clear();
    if (raw.size() < BTF_HDR)
    {
        err = Fmt(L"%s: %Iu bytes, too short for a 12-byte header",
                  name, raw.size());
        return false;
    }

    unsigned count = Be32(&raw[0]);
    unsigned size  = Be32(&raw[4]);
    unsigned units = Be32(&raw[8]);

    // Both products are checked in 64 bits before either is used as a length,
    // so a corrupt count cannot wrap into a small number and pass.
    unsigned long long base = (unsigned long long)BTF_HDR +
                              (unsigned long long)count * BTF_REC;
    unsigned long long need = base + (unsigned long long)units * 2;
    if (need > raw.size())
    {
        err = Fmt(L"%s: header claims %u entries and %u payload units, which "
                  L"needs %llu bytes but the file is %Iu",
                  name, count, units, need, raw.size());
        return false;
    }
    if (size != raw.size() && warn)
        *warn += Fmt(L"warning: %s: header says %u bytes, file is %Iu\n",
                     name, size, raw.size());

    const unsigned char* payload = &raw[(size_t)base];
    out.reserve(count);
    for (unsigned i = 0; i < count; i++)
    {
        const unsigned char* rec = &raw[BTF_HDR + (size_t)i * BTF_REC];
        Entry e;
        e.id           = Be32(rec);
        unsigned off   = Be32(rec + 4);
        unsigned len   = Be16(rec + 8);

        if ((unsigned long long)off + len > units)
        {
            err = Fmt(L"%s: entry %u (id %u) runs to unit %llu, past the "
                      L"%u-unit payload", name, i,
                      (unsigned long long)off + len, units);
            return false;
        }

        // UTF-16BE on disk, UTF-16LE in a wchar_t: one swap per unit, and no
        // transcoding anywhere in this program.
        e.text.resize(len);
        for (unsigned k = 0; k < len; k++)
            e.text[k] = (wchar_t)Be16(payload + ((size_t)off + k) * 2);

        out.push_back(e);
    }
    return true;
}

static bool BuildBtf(const std::vector<Entry>& in, const wchar_t* name,
                     Bytes& out, std::wstring& err, std::wstring* warn)
{
    Bytes recs, payload;
    recs.reserve(in.size() * BTF_REC);

    unsigned long long unit = 0;
    for (size_t i = 0; i < in.size(); i++)
    {
        const Entry& e = in[i];
        if (e.text.size() > 0xFFFF)
        {
            err = Fmt(L"%s: id %u is %Iu code units, over the u16 length "
                      L"field's 65535", name, e.id, e.text.size());
            return false;
        }
        if (warn)
        {
            if (e.text.empty())
                *warn += Fmt(L"warning: %s: id %u is empty; GetString treats an "
                             L"empty string as missing and falls back to "
                             L"sovietEnglish.btf\n", name, e.id);
            for (size_t j = 0; j < i; j++)
                if (in[j].id == e.id)
                {
                    *warn += Fmt(L"warning: %s: id %u appears twice (entries %Iu "
                                 L"and %Iu); the game's lookup is a linear scan, "
                                 L"so the second is unreachable\n",
                                 name, e.id, j, i);
                    break;
                }
        }

        PutBe32(recs, e.id);
        PutBe32(recs, (unsigned)unit);
        PutBe16(recs, (unsigned)e.text.size());

        for (size_t k = 0; k < e.text.size(); k++)
            PutBe16(payload, (unsigned)(unsigned short)e.text[k]);
        PutBe16(payload, 0);                        // the terminator
        unit += e.text.size() + 1;
    }

    unsigned long long total = BTF_HDR + recs.size() + payload.size();
    if (total > 0xFFFFFFFFull || unit > 0xFFFFFFFFull)
    {
        err = Fmt(L"%s: the rebuilt file would be %llu bytes, past the u32 "
                  L"header fields", name, total);
        return false;
    }

    out.clear();
    out.reserve((size_t)total);
    PutBe32(out, (unsigned)in.size());
    PutBe32(out, (unsigned)total);
    PutBe32(out, (unsigned)(payload.size() / 2));
    out.insert(out.end(), recs.begin(), recs.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

// ---------------------------------------------------------------- escaping
//
// Identical rules to btf.py, and the reason each one exists is there. The short
// version: 3226 shipped entries begin or end with a space and editors eat those,
// and the French file alone carries 1270 no-break spaces that survive a round
// trip through an editor badly.

static bool Opaque(unsigned o)
{
    if (o < 0x20 || o == 0x7F || o == 0x85) return true;    // control, and NEL
    if (o == 0xA0 || o == 0xAD || o == 0xFEFF) return true; // nbsp, shy, bom
    if (o >= 0x2000 && o <= 0x200F) return true;            // exotic spaces
    if (o >= 0x2028 && o <= 0x202F) return true;            // bidi marks
    return false;
}

static void Escape(const std::wstring& s, std::wstring& out)
{
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned c = (unsigned short)s[i];

        if (c == L'\\') { out += L"\\\\"; continue; }
        if (c == L'\n') { out += L"\\n";  continue; }
        if (c == L'\r') { out += L"\\r";  continue; }
        if (c == L'\t') { out += L"\\t";  continue; }

        // A valid surrogate pair is one character and goes out as itself; a
        // lone half cannot survive the UTF-8 conversion and is escaped. This is
        // what keeps the output identical to Python's, whose decoder had already
        // joined the pair before the escaper saw it.
        bool high = (c >= 0xD800 && c <= 0xDBFF);
        bool low  = (c >= 0xDC00 && c <= 0xDFFF);
        if (high && i + 1 < s.size())
        {
            unsigned n = (unsigned short)s[i + 1];
            if (n >= 0xDC00 && n <= 0xDFFF)
            {
                out += s[i];
                out += s[i + 1];
                i++;
                continue;
            }
        }

        bool edgeSpace = (c == L' ' && (i == 0 || i + 1 == s.size()));
        if (high || low || Opaque(c) || edgeSpace)
        {
            wchar_t buf[8];
            swprintf_s(buf, _countof(buf), L"\\u%04X", c);
            out += buf;
            continue;
        }
        out += s[i];
    }
}

static int HexVal(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static bool Unescape(const std::wstring& s, const std::wstring& where,
                     std::wstring& out, std::wstring& err)
{
    out.clear();
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] != L'\\') { out += s[i]; continue; }

        if (i + 1 >= s.size())
        {
            err = where + L"a backslash ends the line; write \\\\ for a "
                          L"literal one";
            return false;
        }
        wchar_t k = s[i + 1];
        if (k == L'\\') { out += L'\\'; i++; continue; }
        if (k == L'n')  { out += L'\n'; i++; continue; }
        if (k == L'r')  { out += L'\r'; i++; continue; }
        if (k == L't')  { out += L'\t'; i++; continue; }
        if (k == L'u' || k == L'U')
        {
            int v = 0;
            if (i + 5 >= s.size()) v = -1;
            for (int d = 0; v >= 0 && d < 4; d++)
            {
                int h = HexVal(s[i + 2 + d]);
                v = (h < 0) ? -1 : (v * 16 + h);
            }
            if (v < 0)
            {
                err = where + L"\\u must be followed by exactly four hex digits";
                return false;
            }
            out += (wchar_t)v;
            i += 5;
            continue;
        }
        err = where + Fmt(L"unknown escape \\%c; the escapes are \\\\ \\n \\r "
                          L"\\t \\uXXXX", k);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- the text form

static bool Utf8Encode(const std::wstring& in, Bytes& out, std::wstring& err)
{
    if (in.empty()) { out.clear(); return true; }
    int n = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), (int)in.size(),
                               NULL, 0, NULL, NULL);
    if (n <= 0) { err = L"UTF-8 conversion failed"; return false; }
    out.resize((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, in.c_str(), (int)in.size(),
                        (char*)&out[0], n, NULL, NULL);
    return true;
}

static bool Utf8Decode(const Bytes& in, size_t skip, const wchar_t* name,
                       std::wstring& out, std::wstring& err)
{
    size_t n = in.size() - skip;
    if (n == 0) { out.clear(); return true; }
    // MB_ERR_INVALID_CHARS so a file saved as ANSI is reported rather than
    // silently turned into mojibake.
    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                (const char*)&in[skip], (int)n, NULL, 0);
    if (w <= 0)
    {
        err = Fmt(L"%s: not valid UTF-8. Save the file as UTF-8.", name);
        return false;
    }
    out.resize((size_t)w);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        (const char*)&in[skip], (int)n, &out[0], w);
    return true;
}

// The header block is a comment and is discarded on the way back in. It is
// worded without naming the tool on purpose: btf.py writes the same bytes, so
// the two outputs can be diffed against each other. Change one and change both.
static void DumpText(const std::vector<Entry>& in, const wchar_t* source,
                     std::wstring& out)
{
    out  = L"# soviet republic language file, unpacked for editing\r\n";
    out += Fmt(L"# source: %s\r\n", source);
    out += Fmt(L"# entries: %Iu, in file order\r\n", in.size());
    out += L"#\r\n";
    out += L"# <id> = <text>. Escapes: \\\\ \\n \\r \\t \\uXXXX .\r\n";
    out += L"# Surrounding whitespace is not significant - a space that "
           L"belongs to the\r\n";
    out += L"# string is written \\u0020. Blank and #-comment lines are "
           L"ignored on pack.\r\n";
    out += L"\r\n";

    for (size_t i = 0; i < in.size(); i++)
    {
        out += Fmt(L"%u = ", in[i].id);
        Escape(in[i].text, out);
        out += L"\r\n";
    }
}

// Spaces and tabs only, deliberately: bare Python str.strip() also eats U+00A0
// and U+0085, which in this format are data. btf.py was changed to match, so
// both implementations trim the same set.
static void Trim(std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t')) b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) e--;
    s = s.substr(b, e - b);
}

static bool LoadText(const Bytes& raw, const wchar_t* name,
                     std::vector<Entry>& out, std::wstring& err)
{
    out.clear();
    size_t skip = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB &&
                   raw[2] == 0xBF) ? 3 : 0;
    std::wstring body;
    if (!Utf8Decode(raw, skip, name, body, err)) return false;

    size_t pos = 0;
    unsigned lineno = 0;
    while (pos <= body.size())
    {
        // \r\n, \r or \n, and nothing else - a U+0085 or U+2028 inside a string
        // is not a line break here even though some readers treat it as one.
        size_t nl = body.size();
        size_t adv = 1;
        for (size_t i = pos; i < body.size(); i++)
        {
            if (body[i] == L'\r')
            {
                nl = i;
                adv = (i + 1 < body.size() && body[i + 1] == L'\n') ? 2 : 1;
                break;
            }
            if (body[i] == L'\n') { nl = i; adv = 1; break; }
        }
        std::wstring line = body.substr(pos, nl - pos);
        bool last = (nl == body.size());
        pos = nl + adv;
        lineno++;

        std::wstring stripped = line;
        Trim(stripped);
        if (!stripped.empty() && stripped[0] != L'#')
        {
            size_t eq = stripped.find(L'=');
            if (eq == std::wstring::npos)
            {
                std::wstring shown = line.substr(0, 60);
                err = Fmt(L"%s:%u: no '=' on the line: %s",
                          name, lineno, shown.c_str());
                return false;
            }

            std::wstring key = stripped.substr(0, eq);
            std::wstring val = stripped.substr(eq + 1);
            Trim(key);
            Trim(val);

            bool hex = (key.size() > 2 && key[0] == L'0' &&
                        (key[1] == L'x' || key[1] == L'X'));
            const wchar_t* digits = key.c_str() + (hex ? 2 : 0);
            if (!*digits)
            {
                err = Fmt(L"%s:%u: '%s' is not an id", name, lineno, key.c_str());
                return false;
            }
            unsigned long long id = 0;
            for (const wchar_t* d = digits; *d; d++)
            {
                int v = hex ? HexVal(*d) : ((*d >= L'0' && *d <= L'9') ? *d - L'0' : -1);
                if (v < 0)
                {
                    err = Fmt(L"%s:%u: '%s' is not an id", name, lineno,
                              key.c_str());
                    return false;
                }
                id = id * (hex ? 16 : 10) + v;
                if (id > 0xFFFFFFFFull)
                {
                    err = Fmt(L"%s:%u: id '%s' does not fit in a u32",
                              name, lineno, key.c_str());
                    return false;
                }
            }

            Entry e;
            e.id = (unsigned)id;
            if (!Unescape(val, Fmt(L"%s:%u: ", name, lineno), e.text, err))
                return false;
            out.push_back(e);
        }

        if (last) break;
    }
    return true;
}

// ---------------------------------------------------------------- conversions

// Reads a .btf and writes the text form. Returns a one-line summary.
static bool Unpack(const wchar_t* src, const wchar_t* dst,
                   std::wstring& note, std::wstring& err, std::wstring& warn)
{
    Bytes raw;
    if (!ReadWholeFile(src, raw, err)) return false;

    std::vector<Entry> entries;
    if (!ParseBtf(raw, BaseName(src), entries, err, &warn)) return false;

    std::wstring text;
    DumpText(entries, BaseName(src), text);

    Bytes utf8;
    if (!Utf8Encode(text, utf8, err)) return false;
    if (!WriteWholeFile(dst, utf8.empty() ? (const void*)L"" : (const void*)&utf8[0],
                        utf8.size(), err))
        return false;

    note = Fmt(L"%Iu entries", entries.size());
    return true;
}

// Reads the text form and writes a .btf, then reads that back before saying so.
static bool Pack(const wchar_t* src, const wchar_t* dst,
                 std::wstring& note, std::wstring& err, std::wstring& warn)
{
    Bytes raw;
    if (!ReadWholeFile(src, raw, err)) return false;

    std::vector<Entry> entries;
    if (!LoadText(raw, BaseName(src), entries, err)) return false;

    Bytes blob;
    if (!BuildBtf(entries, BaseName(dst), blob, err, &warn)) return false;
    if (!WriteWholeFile(dst, blob.empty() ? (const void*)L"" : (const void*)&blob[0],
                        blob.size(), err))
        return false;

    // BuildBtf cannot really fail on entries LoadText accepted - but the whole
    // point of the round trip is that nobody trusts that claim, and re-parsing
    // a megabyte costs milliseconds.
    std::vector<Entry> back;
    if (!ParseBtf(blob, BaseName(dst), back, err, NULL)) return false;
    if (back.size() != entries.size())
    {
        err = Fmt(L"%s was written but reads back with %Iu of %Iu entries; do "
                  L"not ship it", BaseName(dst), back.size(), entries.size());
        return false;
    }
    for (size_t i = 0; i < back.size(); i++)
        if (back[i].id != entries[i].id || back[i].text != entries[i].text)
        {
            err = Fmt(L"%s was written but entry %Iu (id %u) does not read back "
                      L"the same; do not ship it",
                      BaseName(dst), i, entries[i].id);
            return false;
        }

    note = Fmt(L"%Iu entries, %Iu bytes, reads back clean",
               entries.size(), blob.size());
    return true;
}

// ---------------------------------------------------------------- console mode

static bool   g_console;
static HANDLE g_conOut;         // set only when stdout really is a console

// Three cases, and the first version handled only the third, which is why
// `--selftest` printed nothing when its output was redirected to a file:
//
//   - stdout is already a redirected file or pipe. The CRT set it up from the
//     inherited handle whatever the subsystem is; reopening CONOUT$ over it
//     would send the output to a console the caller is not reading.
//   - stdout is an inherited console handle, which is what running this from
//     cmd.exe gives even though the exe is /SUBSYSTEM:WINDOWS.
//   - there are no standard handles at all. Then, and only then, borrow the
//     parent's console.
static void OpenConsole()
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;

    if (out && out != INVALID_HANDLE_VALUE)
    {
        if (GetConsoleMode(out, &mode)) g_conOut = out;
        g_console = true;
        return;
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
    if (con != INVALID_HANDLE_VALUE) g_conOut = con;
    g_console = true;
}

static void Say(const std::wstring& s)
{
    if (!g_console) return;
    std::wstring line = s + L"\r\n";

    // WriteConsoleW on a console, because that draws any character correctly
    // whatever the codepage is; UTF-8 bytes down a pipe, because that is what
    // the thing on the other end can read.
    if (g_conOut)
    {
        DWORD wrote = 0;
        WriteConsoleW(g_conOut, line.c_str(), (DWORD)line.size(), &wrote, NULL);
        return;
    }
    Bytes utf8;
    std::wstring err;
    if (Utf8Encode(line, utf8, err) && !utf8.empty())
    {
        fwrite(&utf8[0], 1, utf8.size(), stdout);
        fflush(stdout);
    }
}

// A warning block arrives as one string of \n-separated lines.
static void SayBlock(const std::wstring& text)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(pos, nl - pos);
        while (!line.empty() && line[line.size() - 1] == L'\r')
            line.erase(line.size() - 1);
        if (!line.empty()) Say(line);
        pos = nl + 1;
    }
}

static int SelfTest(const wchar_t* dir)
{
    std::wstring pattern = std::wstring(dir) + L"\\*.btf";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        Say(Fmt(L"no .btf files under %s", dir));
        return 1;
    }

    int total = 0, bad = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring path = std::wstring(dir) + L"\\" + fd.cFileName;
        total++;

        Bytes raw;
        std::wstring err, warn;
        std::vector<Entry> entries;
        if (!ReadWholeFile(path.c_str(), raw, err) ||
            !ParseBtf(raw, fd.cFileName, entries, err, &warn))
        {
            Say(Fmt(L"FAIL %-30s %s", fd.cFileName, err.c_str()));
            bad++;
            continue;
        }

        Bytes rebuilt;
        bool binOk = BuildBtf(entries, fd.cFileName, rebuilt, err, NULL) &&
                     rebuilt == raw;

        std::wstring text;
        DumpText(entries, fd.cFileName, text);
        Bytes utf8;
        std::vector<Entry> back;
        bool textOk = false, viaOk = false;
        if (Utf8Encode(text, utf8, err) &&
            LoadText(utf8, fd.cFileName, back, err))
        {
            textOk = (back.size() == entries.size());
            for (size_t i = 0; textOk && i < back.size(); i++)
                textOk = (back[i].id == entries[i].id &&
                          back[i].text == entries[i].text);
            Bytes again;
            viaOk = BuildBtf(back, fd.cFileName, again, err, NULL) &&
                    again == raw;
        }

        bool ok = binOk && textOk && viaOk;
        if (!ok) bad++;
        Say(Fmt(L"%-4s %-30s %6Iu entries   rebuild=%s  text=%s  text->btf=%s",
                ok ? L"ok" : L"FAIL", fd.cFileName, entries.size(),
                binOk ? L"ok" : L"NO", textOk ? L"ok" : L"NO",
                viaOk ? L"ok" : L"NO"));
    }
    while (FindNextFileW(h, &fd));
    FindClose(h);

    Say(Fmt(L"%d of %d files round-trip byte for byte", total - bad, total));
    return bad ? 1 : 0;
}

// ---------------------------------------------------------------- the window
//
// Plain Win32, controls created by hand, metrics written at 96 dpi and scaled
// through S() - the same shape as tesmiolauncher.cpp, for the same reason: one
// .cpp per binary means no resource script in the build.

#define IDC_SRC_A   100
#define IDC_BROWSE_A 101
#define IDC_GO_A    102
#define IDC_OUT_A   103
#define IDC_SRC_B   200
#define IDC_BROWSE_B 201
#define IDC_GO_B    202
#define IDC_OUT_B   203
#define IDC_LOG     300

static HFONT g_font;
static HFONT g_mono;
static int   g_dpi = 96;
static HWND  g_log;

struct Side
{
    HWND         edit;
    HWND         out;
    HWND         go;
    const wchar_t* filter;
    const wchar_t* title;
    const wchar_t* dstExt;
    bool         (*run)(const wchar_t*, const wchar_t*, std::wstring&,
                        std::wstring&, std::wstring&);
};

static Side g_left, g_right;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Child(HWND parent, const wchar_t* cls, const wchar_t* text,
                  DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, NULL, NULL);
    if (c) SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    return c;
}

static void Log(const std::wstring& line)
{
    if (!g_log) return;
    std::wstring s = line + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

// Each line of a multi-line warning block goes in on its own, indented, so the
// log reads the same as the Python window's.
static void LogBlock(const std::wstring& text)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(pos, nl - pos);
        while (!line.empty() && (line[line.size() - 1] == L'\r')) line.erase(line.size() - 1);
        if (!line.empty()) Log(L"  " + line);
        pos = nl + 1;
    }
}

static std::wstring EditText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s;
    s.resize((size_t)n + 1);
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

static void RefreshSide(Side& side)
{
    std::wstring src = EditText(side.edit);
    Trim(src);
    if (src.empty())
    {
        SetWindowTextW(side.out, L"");
        EnableWindow(side.go, FALSE);
    }
    else
    {
        std::wstring dst = SwapExt(src, side.dstExt);
        SetWindowTextW(side.out, (L"-> " + std::wstring(BaseName(dst.c_str()))).c_str());
        EnableWindow(side.go, TRUE);
    }
}

// Where the file dialog opens: beside whatever is typed, else the game's
// media_soviet if this exe still sits somewhere under the install, else the
// exe's own folder - because it is meant to be copied out and handed around.
static std::wstring StartDir(Side& side)
{
    std::wstring typed = EditText(side.edit);
    Trim(typed);
    if (!typed.empty())
    {
        std::wstring dir = typed;
        while (!dir.empty() && dir[dir.size() - 1] != L'\\' && dir[dir.size() - 1] != L'/')
            dir.erase(dir.size() - 1);
        if (!dir.empty()) return dir;
    }

    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring dir = self;
    while (!dir.empty() && dir[dir.size() - 1] != L'\\') dir.erase(dir.size() - 1);
    std::wstring walk = dir;
    for (int up = 0; up < 5 && walk.size() > 3; up++)
    {
        if (Exists((walk + L"media_soviet").c_str())) return walk + L"media_soviet";
        walk.erase(walk.size() - 1);                        // drop the separator
        while (!walk.empty() && walk[walk.size() - 1] != L'\\') walk.erase(walk.size() - 1);
    }
    return dir;
}

static void Browse(HWND owner, Side& side)
{
    wchar_t file[MAX_PATH] = {0};
    std::wstring typed = EditText(side.edit);
    Trim(typed);
    if (typed.size() < MAX_PATH) wcscpy_s(file, MAX_PATH, typed.c_str());

    std::wstring start = StartDir(side);

    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner    = owner;
    ofn.lpstrFilter  = side.filter;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrTitle   = side.title;
    ofn.lpstrInitialDir = start.empty() ? NULL : start.c_str();
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) SetWindowTextW(side.edit, file);
}

static void Convert(HWND owner, Side& side)
{
    std::wstring src = EditText(side.edit);
    Trim(src);
    if (src.empty() || !Exists(src.c_str()))
    {
        MessageBoxW(owner, Fmt(L"%s is not a file.", src.c_str()).c_str(),
                    APP_TITLE, MB_ICONWARNING | MB_OK);
        return;
    }

    std::wstring dst = SwapExt(src, side.dstExt);
    if (_wcsicmp(dst.c_str(), src.c_str()) == 0)
    {
        MessageBoxW(owner, L"Input and output are the same file.",
                    APP_TITLE, MB_ICONWARNING | MB_OK);
        return;
    }
    if (Exists(dst.c_str()))
    {
        std::wstring ask = Fmt(L"%s already exists.\n\nOverwrite it?",
                               BaseName(dst.c_str()));
        if (MessageBoxW(owner, ask.c_str(), L"Overwrite?",
                        MB_ICONQUESTION | MB_YESNO) != IDYES)
        {
            Log(Fmt(L"cancelled, %s kept", BaseName(dst.c_str())));
            return;
        }
    }

    EnableWindow(side.go, FALSE);
    std::wstring note, err, warn;
    bool ok = side.run(src.c_str(), dst.c_str(), note, err, warn);
    EnableWindow(side.go, TRUE);

    if (!ok)
    {
        Log(L"FAILED  " + err);
        MessageBoxW(owner, err.c_str(), APP_TITLE, MB_ICONERROR | MB_OK);
        return;
    }
    LogBlock(warn);
    Log(Fmt(L"ok      %s -> %s : %s", BaseName(src.c_str()),
            BaseName(dst.c_str()), note.c_str()));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CTLCOLORSTATIC:
        // Statics and group boxes sit on the dialog face, not on a white field.
        // The log is an EDIT and answers WM_CTLCOLOREDIT instead, so it keeps
        // its own background.
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_COMMAND:
    {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        if (code == EN_CHANGE && id == IDC_SRC_A) { RefreshSide(g_left);  return 0; }
        if (code == EN_CHANGE && id == IDC_SRC_B) { RefreshSide(g_right); return 0; }

        if (id == IDC_BROWSE_A) { Browse(hwnd, g_left);  return 0; }
        if (id == IDC_BROWSE_B) { Browse(hwnd, g_right); return 0; }
        if (id == IDC_GO_A)     { Convert(hwnd, g_left);  return 0; }
        if (id == IDC_GO_B)     { Convert(hwnd, g_right); return 0; }
        if (id == IDCANCEL)     { DestroyWindow(hwnd); return 0; }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void GoDpiAware()
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    if (SetCtxFn ctx = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
        if (ctx((HANDLE)-4))              // PER_MONITOR_AWARE_V2
            return;
    SetProcessDPIAware();
}

static void DetectDpi()
{
    HDC dc = GetDC(NULL);
    if (!dc) return;
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (dpi >= 96) g_dpi = dpi;
}

static int ShowUi()
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
        ncm.lfMessageFont.lfHeight = -MulDiv(9, g_dpi, 72);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    {
        LOGFONTW lf = {0};
        lf.lfHeight = -MulDiv(9, g_dpi, 72);
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcscpy_s(lf.lfFaceName, L"Consolas");
        g_mono = CreateFontIndirectW(&lf);
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"btfconvert";
    if (!RegisterClassExW(&wc)) return 1;

    // 96-dpi layout. Two halves of equal width, each a group box holding a path
    // row, the derived output name and the button; the log spans both below.
    const int pad   = 12;
    const int half  = 300;
    const int W     = pad + half + pad + half + pad;
    const int rowH  = 22;
    const int btnW  = 84;
    const int btnH  = 28;
    const int grpH  = 22 + rowH + 6 + 16 + 8 + btnH + 10;
    const int logH  = 170;

    int y = pad;
    const int grpY = y;            y += grpH + pad;
    const int logY = y;            y += logH + pad;

    // The same style the window is actually created with, or the client area
    // comes out short by the difference.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    RECT rc = { 0, 0, S(W), S(y) };
    AdjustWindowRect(&rc, style, FALSE);
    int winW = rc.right - rc.left, winH = rc.bottom - rc.top;
    int cx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int cy = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName,
                                APP_TITLE L" - Workers & Resources language files",
                                style, cx, cy, winW, winH,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;

    struct Spec
    {
        int x;
        const wchar_t* group;
        const wchar_t* button;
        int idSrc, idBrowse, idGo, idOut;
        Side* side;
    };
    Spec specs[2] =
    {
        { pad,                 L"Language file  ->  text", L"Convert in txt",
          IDC_SRC_A, IDC_BROWSE_A, IDC_GO_A, IDC_OUT_A, &g_left  },
        { pad + half + pad,    L"Text  ->  language file", L"Convert in btf",
          IDC_SRC_B, IDC_BROWSE_B, IDC_GO_B, IDC_OUT_B, &g_right },
    };

    for (int i = 0; i < 2; i++)
    {
        Spec& s = specs[i];
        Child(hwnd, L"BUTTON", s.group, BS_GROUPBOX, s.x, grpY, half, grpH, 0, NULL);

        int ix = s.x + 10, iw = half - 20;
        int ry = grpY + 22;
        s.side->edit = Child(hwnd, L"EDIT", L"",
                             WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                             ix, ry, iw - btnW - 6, rowH, s.idSrc, NULL);
        Child(hwnd, L"BUTTON", L"Browse...", WS_TABSTOP,
              ix + iw - btnW, ry, btnW, rowH, s.idBrowse, NULL);
        s.side->out = Child(hwnd, L"STATIC", L"", SS_PATHELLIPSIS,
                            ix, ry + rowH + 6, iw, 16, s.idOut, NULL);
        s.side->go  = Child(hwnd, L"BUTTON", s.button, WS_TABSTOP,
                            ix, ry + rowH + 6 + 16 + 8, iw, btnH, s.idGo, NULL);
        EnableWindow(s.side->go, FALSE);
    }

    Child(hwnd, L"BUTTON", L"Log", BS_GROUPBOX, pad, logY, W - 2 * pad, logH, 0, NULL);
    g_log = Child(hwnd, L"EDIT", L"",
                  WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                  ES_AUTOVSCROLL,
                  pad + 8, logY + 18, W - 2 * pad - 16, logH - 26, IDC_LOG, g_mono);

    Log(L"btfconvert - .btf <-> .txt, no Python needed");
    Log(L"language files are media_soviet\\soviet<Language>.btf");
    Log(L"a rebuilt file ships as tesmioloader\\vfs\\media_soviet\\<name>.btf");
    Log(L"- no game file is touched");

    ShowWindow(hwnd, SW_SHOW);
    SetFocus(g_left.edit);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }

    return 0;
}

// ---------------------------------------------------------------- entry

static int Usage()
{
    Say(L"btfconvert - Workers & Resources language files");
    Say(L"");
    Say(L"  btfconvert                              open the window");
    Say(L"  btfconvert --unpack <in.btf> [out.txt]");
    Say(L"  btfconvert --pack   <in.txt> [out.btf]");
    Say(L"  btfconvert --selftest <dir>             round-trip every .btf there");
    Say(L"");
    Say(L"Without an output path the input's extension is swapped. An existing");
    Say(L"file is overwritten without asking on the command line and never in");
    Say(L"the window.");
    return 0;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    GoDpiAware();
    DetectDpi();

    g_left.filter  = L"language file (*.btf)\0*.btf\0all files\0*.*\0";
    g_left.title   = L"Choose a .btf file";
    g_left.dstExt  = L".txt";
    g_left.run     = Unpack;
    g_right.filter = L"text file (*.txt)\0*.txt\0all files\0*.*\0";
    g_right.title  = L"Choose a .txt file";
    g_right.dstExt = L".btf";
    g_right.run    = Pack;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc <= 1) return ShowUi();

    OpenConsole();
    std::wstring cmd = argv[1];

    if (cmd == L"--help" || cmd == L"-h" || cmd == L"/?") return Usage();

    if (cmd == L"--selftest")
        return SelfTest(argc > 2 ? argv[2] : L".");

    bool unpack = (cmd == L"--unpack");
    if (unpack || cmd == L"--pack")
    {
        if (argc < 3) return Usage();
        std::wstring src = argv[2];
        std::wstring dst = (argc > 3) ? argv[3]
                                      : SwapExt(src, unpack ? L".txt" : L".btf");
        std::wstring note, err, warn;
        bool ok = unpack ? Unpack(src.c_str(), dst.c_str(), note, err, warn)
                         : Pack(src.c_str(), dst.c_str(), note, err, warn);
        if (!warn.empty()) SayBlock(warn);
        if (!ok) { Say(L"error: " + err); return 1; }
        Say(Fmt(L"%s -> %s : %s", src.c_str(), dst.c_str(), note.c_str()));
        return 0;
    }

    Say(L"error: unknown option " + cmd);
    Usage();
    return 2;
}
