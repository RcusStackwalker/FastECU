#pragma once

#include <array>
#include <cstdint>

enum mcu_type
{
    M32R_128KB,
    M32R_256KB,
    M32R_384KB,
    M32R_512KB,
    M32R_512KB_1block,
    M32R_512KB_4blocks,
    M32R_384KB_1block,
    MC68HC16Y5,
    MC68HC16Y5_TPU,
    SH7051,
    SH7055,
    SH7058,
    SH7058_1block,
    SH7058d,
    SH7059d,
    SH72543d,
    SH72531,
    N83M_4MB,
    N83M_1_5MB,
    SH72543R,
    MH8104,
    MH5006,
    MH8111,
    M3779x,
    M3775x,
    SH_INVALID
};

struct flashblock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct ramblock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct kernelblock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct eepromblock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct flashdev_t
{
    const char *name; // like "7058", for UI convenience only
    enum mcu_type mcutype;

    const std::uint32_t romsize; // in bytes
    const unsigned numblocks;
    const struct flashblock *fblocks;
    const struct ramblock *rblocks;
    const struct kernelblock *kblocks;
    const struct eepromblock *eblocks;
};

/* list of all defined flash devices */
// extern const struct flashdev_t flashdevices[];

inline constexpr auto fblocks_SH72543d = std::to_array<flashblock>({
    //    {0x00000000,    0x00008000},
    {0x00008000, 0x001F7F00},
    /*
        {0x00000000,    0x00002000},
        {0x00002000,    0x00002000},
        {0x00004000,    0x00002000},
        {0x00006000,    0x00002000},
        {0x00008000,    0x00002000},
        {0x0000A000,    0x00002000},
        {0x0000C000,    0x00002000},
        {0x0000E000,    0x00002000},
        {0x00010000,    0x00010000},
        {0x00020000,    0x00010000},
        {0x00030000,    0x00010000},
        {0x00040000,    0x00010000},
        {0x00050000,    0x00010000},
        {0x00060000,    0x00010000},
        {0x00070000,    0x00010000},
        {0x00080000,    0x00010000},
        {0x00090000,    0x00010000},
        {0x000A0000,    0x00020000},
        {0x000C0000,    0x00020000},
        {0x000E0000,    0x00020000},
        {0x00100000,    0x00020000},
        {0x00120000,    0x00020000},
        {0x00140000,    0x00020000},
        {0x00160000,    0x00020000},
        {0x00180000,    0x00020000},
        {0x001A0000,    0x00020000},
        {0x001C0000,    0x00020000},
        {0x001E0000,    0x00020000},
    */
});

inline constexpr auto rblocks_SH72543d = std::to_array<ramblock>({
    {0xFFF80000, 0x00004000},
    {0xFFF84000, 0x00004000},
    {0xFFF88000, 0x00004000},
    {0xFFF8C000, 0x00004000},
    {0xFFF90000, 0x00004000},
    {0xFFF94000, 0x00004000},
    {0xFFF98000, 0x00004000},
    {0xFFF9C000, 0x00004000},
});

inline constexpr auto kblocks_SH72543d = std::to_array<kernelblock>({
    {0xFFF80000, 0xFFF9FFFF},
});

inline constexpr auto eblocks_SH72543d = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

/* flash block definitions */
inline constexpr auto fblocks_SH7059d = std::to_array<flashblock>({
    {0x00000000, 0x00001000},
    {0x00001000, 0x00001000},
    {0x00002000, 0x00001000},
    {0x00003000, 0x00001000},
    {0x00004000, 0x00001000},
    {0x00005000, 0x00001000},
    {0x00006000, 0x00001000},
    {0x00007000, 0x00001000},
    {0x00008000, 0x00018000},
    {0x00020000, 0x00020000},
    {0x00040000, 0x00020000},
    {0x00060000, 0x00020000},
    {0x00080000, 0x00040000},
    {0x000C0000, 0x00040000},
    {0x00100000, 0x00040000},
    {0x00140000, 0x00040000},
});

inline constexpr auto rblocks_SH7059d = std::to_array<ramblock>({
    {0xFFFE8000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kblocks_SH7059d = std::to_array<kernelblock>({
    {0xFFFE8000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto eblocks_SH7059d = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

/* flash block definitions */
inline constexpr auto fblocks_SH7058d = std::to_array<flashblock>({
    {0x00000000, 0x00001000},
    {0x00001000, 0x00001000},
    {0x00002000, 0x00001000},
    {0x00003000, 0x00001000},
    {0x00004000, 0x00001000},
    {0x00005000, 0x00001000},
    {0x00006000, 0x00001000},
    {0x00007000, 0x00001000},
    {0x00008000, 0x00018000},
    {0x00020000, 0x00020000},
    {0x00040000, 0x00020000},
    {0x00060000, 0x00020000},
    {0x00080000, 0x00020000},
    {0x000A0000, 0x00020000},
    {0x000C0000, 0x00020000},
    {0x000E0000, 0x00020000},
});

inline constexpr auto rblocks_SH7058d = std::to_array<ramblock>({
    {0xFFFF3000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kblocks_SH7058d = std::to_array<kernelblock>({
    {0xFFFF4000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto eblocks_SH7058d = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_SH7058 = std::to_array<flashblock>({
    {0x00000000, 0x00001000},
    {0x00001000, 0x00001000},
    {0x00002000, 0x00001000},
    {0x00003000, 0x00001000},
    {0x00004000, 0x00001000},
    {0x00005000, 0x00001000},
    {0x00006000, 0x00001000},
    {0x00007000, 0x00001000},
    {0x00008000, 0x00018000},
    {0x00020000, 0x00020000},
    {0x00040000, 0x00020000},
    {0x00060000, 0x00020000},
    {0x00080000, 0x00020000},
    {0x000A0000, 0x00020000},
    {0x000C0000, 0x00020000},
    {0x000E0000, 0x00020000},
});

inline constexpr auto rblocks_SH7058 = std::to_array<ramblock>({
    {0xFFFF3000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kblocks_SH7058 = std::to_array<kernelblock>({
    {0xFFFF3000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto eblocks_SH7058 = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_SH7058_1block = std::to_array<flashblock>({
    {0x00000000, 0x00100000},
});

inline constexpr auto fblocks_SH72531 = std::to_array<flashblock>({
    {0x00000000, 0x00008000},
    {0x00008000, 0x00137F00},
    {0x00137F00, 0x00000100},
});

inline constexpr auto fblocks_N83M_4MB = std::to_array<flashblock>({
    {0x08F9C000, 0x00010000},
    {0x08FAC000, 0x003D3F00},
    {0x0937FF00, 0x00000100},
});

inline constexpr auto fblocks_N83M_1_5MB = std::to_array<flashblock>({
    {0x08F9C000, 0x00010000},
    {0x08FAC000, 0x00173F00},
    {0x0911FF00, 0x00000100},
});

inline constexpr auto fblocks_SH72543R = std::to_array<flashblock>({
    {0x00000000, 0x00006000},
    {0x00006000, 0x001FA000},
});

inline constexpr auto fblocks_SH7055 = std::to_array<flashblock>({
    {0x00000000, 0x00001000},
    {0x00001000, 0x00001000},
    {0x00002000, 0x00001000},
    {0x00003000, 0x00001000},
    {0x00004000, 0x00001000},
    {0x00005000, 0x00001000},
    {0x00006000, 0x00001000},
    {0x00007000, 0x00001000},
    {0x00008000, 0x00008000},
    {0x00010000, 0x00010000},
    {0x00020000, 0x00010000},
    {0x00030000, 0x00010000},
    {0x00040000, 0x00010000},
    {0x00050000, 0x00010000},
    {0x00060000, 0x00010000},
    {0x00070000, 0x00010000},
});

inline constexpr auto rblocks_SH7055 = std::to_array<ramblock>({
    {0xFFFF6000, 0x00006000}, // 0xFFFFBFFF // 24k
});

inline constexpr auto kblocks_SH7055 = std::to_array<kernelblock>({
    {0xFFFF6004, 0x00006000}, // 0xFFFFBFFF // 24k
});

inline constexpr auto eblocks_SH7055 = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_SH7051 = std::to_array<flashblock>({
    {0x00000000, 0x00040000},
});

inline constexpr auto rblocks_SH7051 = std::to_array<ramblock>({
    {0xFFFFD800, 0x00002800}, // 0xFFFFFFFF  // 10k
});

inline constexpr auto kblocks_SH7051 = std::to_array<kernelblock>({
    {0xFFFFD800, 0x00002800}, // 0xFFFFFFFF  // 10k
});

inline constexpr auto eblocks_SH7051 = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_M3779x = std::to_array<flashblock>({
    {0x00008000, 0x0000FFFF},
});

inline constexpr auto rblocks_M3779x = std::to_array<ramblock>({
    {0x00001000, 0x000014FF},
});

inline constexpr auto kblocks_M3779x = std::to_array<kernelblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto eblocks_M3779x = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_M3775x = std::to_array<flashblock>({
    {0x00001000, 0x0000FFFF},
});

inline constexpr auto rblocks_M3775x = std::to_array<ramblock>({
    {0x00001000, 0x000014FF},
});

inline constexpr auto kblocks_M3775x = std::to_array<kernelblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto eblocks_M3775x = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_MC68HC16Y5_TPU = std::to_array<flashblock>({
    {0x00060000, 0x00001000},
    {0x00061000, 0x00001000},
    {0x00062000, 0x00001000},
    {0x00063000, 0x00001000},
});

inline constexpr auto fblocks_MC68HC16Y5 = std::to_array<flashblock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00004000},
    {0x00008000, 0x00004000},
    {0x0000C000, 0x00004000},
    {0x00010000, 0x00004000},
    {0x00014000, 0x00004000},
    {0x00018000, 0x00004000},
    {0x0001C000, 0x00004000},
    {0x00028000, 0x00004000},
    {0x0002C000, 0x00004000},
});

inline constexpr auto rblocks_MC68HC16Y5 = std::to_array<ramblock>({
    {0x00020000, 0x00008000},
});

inline constexpr auto kblocks_MC68HC16Y5 = std::to_array<kernelblock>({
    {0x00020000, 0x00008000},
});

inline constexpr auto eblocks_MC68HC16Y5 = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_M32R_128KB = std::to_array<flashblock>({
    {0x00000000, 0x00010000},
    {0x00010000, 0x00010000},
});

inline constexpr auto rblocks_M32R_128KB = std::to_array<ramblock>({
    {0x00801000, 0x00001800},
});

inline constexpr auto kblocks_M32R_128KB = std::to_array<kernelblock>({
    {0x00801000, 0x00001800},
});

inline constexpr auto eblocks_M32R_128KB = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_M32R_256KB = std::to_array<flashblock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00008000},
    {0x00010000, 0x00010000},
    {0x00020000, 0x00010000},
    {0x00030000, 0x00010000},
});

inline constexpr auto rblocks_M32R_256KB = std::to_array<ramblock>({
    {0x00804000, 0x00004000},
});

inline constexpr auto kblocks_M32R_256KB = std::to_array<kernelblock>({
    {0x00804000, 0x00004000},
});

inline constexpr auto eblocks_M32R_256KB = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_M32R_384KB = std::to_array<flashblock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00008000},
    {0x00010000, 0x00010000},
    {0x00020000, 0x00010000},
    {0x00030000, 0x00010000},
    {0x00040000, 0x00010000},
    {0x00050000, 0x00010000},
});

inline constexpr auto rblocks_M32R_384KB = std::to_array<ramblock>({
    {0x00804000, 0x00008000},
});

inline constexpr auto kblocks_M32R_384KB = std::to_array<kernelblock>({
    {0x00804000, 0x00008000},
});

inline constexpr auto eblocks_M32R_384KB = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_M32R_512KB = std::to_array<flashblock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00008000},
    {0x00010000, 0x00010000},
    {0x00020000, 0x00010000},
    {0x00030000, 0x00010000},
    {0x00040000, 0x00010000},
    {0x00050000, 0x00010000},
    {0x00060000, 0x00010000},
    {0x00070000, 0x00010000},
});

inline constexpr auto fblocks_M32R_512KB_1block = std::to_array<flashblock>({
    {0x00000000, 0x00080000},
});

inline constexpr auto fblocks_M32R_512KB_4blocks = std::to_array<flashblock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00078000},
});

// Mitsubishi Colt CZT (Z37A, ROM 47110032): readable/writeable userspace
// range. Reads use bootload session 0x85; the 0x0-0x8000 boot region is not
// touched by this protocol.
inline constexpr auto fblocks_M32R_384KB_1block = std::to_array<flashblock>({
    {0x00008000, 0x00058000},
});

inline constexpr auto rblocks_M32R_512KB = std::to_array<ramblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kblocks_M32R_512KB = std::to_array<kernelblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto eblocks_M32R_512KB = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_MH8104 = std::to_array<flashblock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00078000},
});

inline constexpr auto rblocks_MH8104 = std::to_array<ramblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kblocks_MH8104 = std::to_array<kernelblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto eblocks_MH8104 = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_MH5006 = std::to_array<flashblock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x000F8000},
});

inline constexpr auto rblocks_MH5006 = std::to_array<ramblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kblocks_MH5006 = std::to_array<kernelblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto eblocks_MH5006 = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto fblocks_MH8111 = std::to_array<flashblock>({
    {0x00000000, 0x00040000},
    {0x00040000, 0x00020000},
    {0x00060000, 0x00020000},
    {0x00080000, 0x00100000},
});

inline constexpr auto rblocks_MH8111 = std::to_array<ramblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kblocks_MH8111 = std::to_array<kernelblock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto eblocks_MH8111 = std::to_array<eepromblock>({
    {0x00000000, 0x00000100},
});

// name mcutype romsize numblocks fblocks rblocks kblocks eblocks;
inline constexpr auto flashdevices = std::to_array<flashdev_t>({
    {"M32R_128KB", M32R_128KB, 128 * 1024, 2, fblocks_M32R_128KB.data(), rblocks_M32R_128KB.data(),
     kblocks_M32R_128KB.data(), eblocks_M32R_128KB.data()},
    {"M32R_256KB", M32R_256KB, 256 * 1024, 7, fblocks_M32R_256KB.data(), rblocks_M32R_256KB.data(),
     kblocks_M32R_256KB.data(), eblocks_M32R_256KB.data()},
    {"M32R_384KB", M32R_384KB, 384 * 1024, 9, fblocks_M32R_384KB.data(), rblocks_M32R_384KB.data(),
     kblocks_M32R_384KB.data(), eblocks_M32R_384KB.data()},
    {"M32R_512KB", M32R_512KB, 512 * 1024, 11, fblocks_M32R_512KB.data(), rblocks_M32R_512KB.data(),
     kblocks_M32R_512KB.data(), eblocks_M32R_512KB.data()},
    {"M32R_512KB_1block", M32R_512KB_1block, 512 * 1024, 1, fblocks_M32R_512KB_1block.data(), rblocks_M32R_512KB.data(),
     kblocks_M32R_512KB.data(), eblocks_M32R_512KB.data()},
    {"M32R_512KB_4blocks", M32R_512KB_4blocks, 512 * 1024, 4, fblocks_M32R_512KB_4blocks.data(),
     rblocks_M32R_512KB.data(), kblocks_M32R_512KB.data(), eblocks_M32R_512KB.data()},
    {"M32R_384KB_1block", M32R_384KB_1block, 384 * 1024, 1, fblocks_M32R_384KB_1block.data(), rblocks_M32R_512KB.data(),
     kblocks_M32R_512KB.data(), eblocks_M32R_512KB.data()},
    {"MC68HC16Y5", MC68HC16Y5, 160 * 1024, 10, fblocks_MC68HC16Y5.data(), rblocks_MC68HC16Y5.data(),
     kblocks_MC68HC16Y5.data(), eblocks_MC68HC16Y5.data()},
    {"MC68HC16Y5_TPU", MC68HC16Y5_TPU, 4 * 1024, 4, fblocks_MC68HC16Y5_TPU.data(), rblocks_MC68HC16Y5.data(),
     kblocks_MC68HC16Y5.data(), eblocks_MC68HC16Y5.data()},
    {"SH7051", SH7051, 256 * 1024, 1, fblocks_SH7051.data(), rblocks_SH7051.data(), kblocks_SH7051.data(),
     eblocks_SH7051.data()},
    {"SH7055", SH7055, 512 * 1024, 16, fblocks_SH7055.data(), rblocks_SH7055.data(), kblocks_SH7055.data(),
     eblocks_SH7055.data()},
    {"SH7058", SH7058, 1024 * 1024, 16, fblocks_SH7058.data(), rblocks_SH7058.data(), kblocks_SH7058.data(),
     eblocks_SH7058.data()},
    {"SH7058_1block", SH7058, 1024 * 1024, 1, fblocks_SH7058_1block.data(), rblocks_SH7058.data(),
     kblocks_SH7058.data(), eblocks_SH7058.data()},
    {"SH7058d", SH7058d, 1024 * 1024, 16, fblocks_SH7058d.data(), rblocks_SH7058d.data(), kblocks_SH7058d.data(),
     eblocks_SH7058d.data()},
    {"SH7059d", SH7059d, 1536 * 1024, 16, fblocks_SH7059d.data(), rblocks_SH7059d.data(), kblocks_SH7059d.data(),
     eblocks_SH7059d.data()},
    {"SH72543d", SH72543d, 2 * 1024 * 1024, 1, fblocks_SH72543d.data(), rblocks_SH72543d.data(),
     kblocks_SH72543d.data(), eblocks_SH72543d.data()},
    {"SH72531", SH72531, 1280 * 1024, 3, fblocks_SH72531.data(), rblocks_SH7058.data(), kblocks_SH7058.data(),
     eblocks_SH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"N83M_4MB", N83M_4MB, 3984 * 1024, 3, fblocks_N83M_4MB.data(), rblocks_SH7058.data(), kblocks_SH7058.data(),
     eblocks_SH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"N83M_1_5MB", N83M_1_5MB, 1488 * 1024, 3, fblocks_N83M_1_5MB.data(), rblocks_SH7058.data(), kblocks_SH7058.data(),
     eblocks_SH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"SH72543R", SH72543R, 2 * 1024 * 1024, 2, fblocks_SH72543R.data(), rblocks_SH7058.data(), kblocks_SH7058.data(),
     eblocks_SH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"MH8104", MH8104, 512 * 1024, 4, fblocks_MH8104.data(), rblocks_MH8104.data(), kblocks_MH8104.data(),
     eblocks_MH8104.data()},
    {"MH5006", MH5006, 1024 * 1024, 4, fblocks_MH5006.data(), rblocks_MH5006.data(), kblocks_MH5006.data(),
     eblocks_MH5006.data()},
    {"MH8111", MH8111, 3 * 512 * 1024, 4, fblocks_MH8111.data(), rblocks_MH8111.data(), kblocks_MH8111.data(),
     eblocks_MH8111.data()},
    {"M3779x", M3779x, 64 * 1024, 1, fblocks_M3779x.data(), rblocks_M3779x.data(), kblocks_M3779x.data(),
     eblocks_M3779x.data()},
    {"M3775x", M3775x, 64 * 1024, 1, fblocks_M3775x.data(), rblocks_M3775x.data(), kblocks_M3775x.data(),
     eblocks_M3775x.data()},
    {0, SH_INVALID, 0, 0, 0, 0, 0, 0},
});
