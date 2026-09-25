#pragma once
// Shared by the EchoXR launcher (xr/src/launcher.cpp) and the installer (installer/setup.cpp):
// file helpers, the echovr_openxr.exe patch, and the dbgcore.dll plugin-loader rules.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace echoxr {

inline bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
inline bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::string ReadAll(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// Writes to <path>.new, then swaps it in, so a file in use fails cleanly instead of
// being left half-written. Returns 0 or the Windows error.
inline DWORD WriteAll(const std::wstring& path, const void* data, size_t size) {
    std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &wrote, nullptr) && wrote == size;
    DWORD e = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return e ? e : ERROR_WRITE_FAULT; }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        e = GetLastError();
        DeleteFileW(tmp.c_str());
        return e;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// echovr_openxr.exe: a copy of echovr.exe whose LibOVR runtime signature check always
// passes, so Echo accepts the unsigned EchoXR runtime. echovr.exe itself is untouched.
// The check's prologue is verified first; any other game build is refused.
// ---------------------------------------------------------------------------
static const wchar_t* kModdedExe = L"echovr_openxr.exe";
static const DWORD kSigCheckRva = 0x1365bd0;
static const BYTE kSigCheckPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x56 };
static const BYTE kSigCheckPatch[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };   // mov eax,1 ; ret

inline size_t RvaToOffset(const std::string& pe, DWORD rva) {
    if (pe.size() < 0x40 || pe[0] != 'M' || pe[1] != 'Z') return 0;
    DWORD nt = *(const DWORD*)&pe[0x3C];
    if ((size_t)nt + sizeof(IMAGE_NT_HEADERS64) > pe.size() || memcmp(&pe[nt], "PE\0\0", 4)) return 0;
    const IMAGE_NT_HEADERS64* h = (const IMAGE_NT_HEADERS64*)&pe[nt];
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(h);
    for (WORD i = 0; i < h->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((const char*)(sec + 1) > pe.data() + pe.size()) return 0;
        DWORD size = sec->Misc.VirtualSize > sec->SizeOfRawData ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + size) {
            size_t off = rva - sec->VirtualAddress + sec->PointerToRawData;
            return off < pe.size() ? off : 0;
        }
    }
    return 0;
}

// dir = bin\win10 with a trailing-slash-free path. On failure, err says why and
// *winErr holds the Windows error (0 when the reason is the game build).
inline bool MakeOpenXRExe(const std::wstring& dir, std::wstring& err, size_t* offOut = nullptr, DWORD* winErr = nullptr) {
    if (winErr) *winErr = 0;
    std::string data = ReadAll(dir + L"\\echovr.exe");
    if (data.empty()) { err = L"couldn't read echovr.exe"; return false; }
    size_t off = RvaToOffset(data, kSigCheckRva);
    if (!off || off + sizeof(kSigCheckPrologue) > data.size()) { err = L"echovr.exe isn't the expected build"; return false; }
    if (memcmp(&data[off], kSigCheckPrologue, sizeof(kSigCheckPrologue))) {
        err = memcmp(&data[off], kSigCheckPatch, sizeof(kSigCheckPatch)) ? L"echovr.exe isn't the expected build (check bytes differ)"
                                                                         : L"echovr.exe itself is already patched -- restore the original first";
        return false;
    }
    memcpy(&data[off], kSigCheckPatch, sizeof(kSigCheckPatch));
    DWORD e = WriteAll(dir + L"\\" + kModdedExe, data.data(), data.size());
    if (e) {
        if (winErr) *winErr = e;
        err = (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED) ? L"can't write echovr_openxr.exe (in use, or the folder needs administrator)"
                                                                         : L"can't write echovr_openxr.exe (error " + std::to_wstring(e) + L")";
        return false;
    }
    if (offOut) *offOut = off;
    return true;
}

// ---------------------------------------------------------------------------
// Plugin loader (dbgcore.dll) rules:
//   none                             -> install the loader
//   the old 45 KB one (by hash)      -> install the loader; the old one moves to
//                                       plugins\dbgcore_legacy.dll and loads as a plugin
//   anything else + plugins\ exists  -> a working loader: kept, plugin goes in plugins\
//   anything else, no plugins\       -> not a plugin loader: replaced only when asked,
//                                       old one saved as dbgcore.dll.bak
// ---------------------------------------------------------------------------
static const size_t   kLegacySize = 45568;
static const uint64_t kLegacyHash = 0xbba858556700da1aull;   // FNV-1a 64
static const wchar_t* kLegacyRel  = L"plugins\\dbgcore_legacy.dll";

inline uint64_t Fnv1a(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

enum LoaderState { L_MISSING, L_OURS, L_LEGACY, L_OTHER_LOADER, L_FOREIGN };

// ours = the loader this build ships, to recognise it byte for byte.
inline LoaderState ClassifyLoader(const std::wstring& dir, const void* ours, size_t oursSize) {
    std::string have = ReadAll(dir + L"\\dbgcore.dll");
    if (have.empty()) return L_MISSING;
    if (ours && have.size() == oursSize && !memcmp(have.data(), ours, oursSize)) return L_OURS;
    if (have.size() == kLegacySize && Fnv1a(have) == kLegacyHash) return L_LEGACY;
    if (IsDir(dir + L"\\plugins")) return L_OTHER_LOADER;   // a loader is already set up here
    // a plugin loader names the folder it scans
    return have.find("plugins") != std::string::npos ? L_OTHER_LOADER : L_FOREIGN;
}

// Whether the loader gets installed without asking.
inline bool LoaderWanted(LoaderState st) { return st == L_MISSING || st == L_LEGACY; }

}  // namespace echoxr
