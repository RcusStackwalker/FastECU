#pragma once

#include <array>
#include <cstdint>

enum McuType
{
    kM32R128Kb,
    kM32R256Kb,
    kM32R384Kb,
    kM32R512Kb,
    kM32R512Kb1block,
    kM32R512Kb4blocks,
    kM32R384Kb1block,
    kMC68HC16Y5,
    kMC68HC16Y5Tpu,
    kSH7051,
    kSH7055,
    kSH7058,
    kSH70581block,
    kSH7058d,
    kSH7059d,
    kSH72543d,
    kSH72531,
    kN83M4Mb,
    kN83M15Mb,
    kSH72543R,
    kMH8104,
    kMH5006,
    kMH8111,
    kM3779x,
    kM3775x,
    kShInvalid
};

struct FlashBlock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct RamBlock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct KernelBlock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct EepromBlock
{
    std::uint32_t start;
    std::uint32_t len;
};

struct FlashDevice
{
    const char *name; // like "7058", for UI convenience only
    enum McuType mcutype;

    const std::uint32_t romsize; // in bytes
    const unsigned numblocks;
    const struct FlashBlock *fblocks;
    const struct RamBlock *rblocks;
    const struct KernelBlock *kblocks;
    const struct EepromBlock *eblocks;
};

/* list of all defined flash devices */
// extern const struct FlashDevice kFlashDevices[];

inline constexpr auto kFlashBlocksSH72543d = std::to_array<FlashBlock>({
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

inline constexpr auto kRamBlocksSH72543d = std::to_array<RamBlock>({
    {0xFFF80000, 0x00004000},
    {0xFFF84000, 0x00004000},
    {0xFFF88000, 0x00004000},
    {0xFFF8C000, 0x00004000},
    {0xFFF90000, 0x00004000},
    {0xFFF94000, 0x00004000},
    {0xFFF98000, 0x00004000},
    {0xFFF9C000, 0x00004000},
});

inline constexpr auto kKernelBlocksSH72543d = std::to_array<KernelBlock>({
    {0xFFF80000, 0xFFF9FFFF},
});

inline constexpr auto kEepromBlocksSH72543d = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

/* flash block definitions */
inline constexpr auto kFlashBlocksSH7059d = std::to_array<FlashBlock>({
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

inline constexpr auto kRamBlocksSH7059d = std::to_array<RamBlock>({
    {0xFFFE8000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kKernelBlocksSH7059d = std::to_array<KernelBlock>({
    {0xFFFE8000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kEepromBlocksSH7059d = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

/* flash block definitions */
inline constexpr auto kFlashBlocksSH7058d = std::to_array<FlashBlock>({
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

inline constexpr auto kRamBlocksSH7058d = std::to_array<RamBlock>({
    {0xFFFF3000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kKernelBlocksSH7058d = std::to_array<KernelBlock>({
    {0xFFFF4000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kEepromBlocksSH7058d = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksSH7058 = std::to_array<FlashBlock>({
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

inline constexpr auto kRamBlocksSH7058 = std::to_array<RamBlock>({
    {0xFFFF3000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kKernelBlocksSH7058 = std::to_array<KernelBlock>({
    {0xFFFF3000, 0x00009000}, // 0xFFFFBFFF // 36k
});

inline constexpr auto kEepromBlocksSH7058 = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksSH7058_1block = std::to_array<FlashBlock>({
    {0x00000000, 0x00100000},
});

inline constexpr auto kFlashBlocksSH72531 = std::to_array<FlashBlock>({
    {0x00000000, 0x00008000},
    {0x00008000, 0x00137F00},
    {0x00137F00, 0x00000100},
});

inline constexpr auto kFlashBlocksN83M_4MB = std::to_array<FlashBlock>({
    {0x08F9C000, 0x00010000},
    {0x08FAC000, 0x003D3F00},
    {0x0937FF00, 0x00000100},
});

inline constexpr auto kFlashBlocksN83M_1_5MB = std::to_array<FlashBlock>({
    {0x08F9C000, 0x00010000},
    {0x08FAC000, 0x00173F00},
    {0x0911FF00, 0x00000100},
});

inline constexpr auto kFlashBlocksSH72543R = std::to_array<FlashBlock>({
    {0x00000000, 0x00006000},
    {0x00006000, 0x001FA000},
});

inline constexpr auto kFlashBlocksSH7055 = std::to_array<FlashBlock>({
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

inline constexpr auto kRamBlocksSH7055 = std::to_array<RamBlock>({
    {0xFFFF6000, 0x00006000}, // 0xFFFFBFFF // 24k
});

inline constexpr auto kKernelBlocksSH7055 = std::to_array<KernelBlock>({
    {0xFFFF6004, 0x00006000}, // 0xFFFFBFFF // 24k
});

inline constexpr auto kEepromBlocksSH7055 = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksSH7051 = std::to_array<FlashBlock>({
    {0x00000000, 0x00040000},
});

inline constexpr auto kRamBlocksSH7051 = std::to_array<RamBlock>({
    {0xFFFFD800, 0x00002800}, // 0xFFFFFFFF  // 10k
});

inline constexpr auto kKernelBlocksSH7051 = std::to_array<KernelBlock>({
    {0xFFFFD800, 0x00002800}, // 0xFFFFFFFF  // 10k
});

inline constexpr auto kEepromBlocksSH7051 = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksM3779x = std::to_array<FlashBlock>({
    {0x00008000, 0x0000FFFF},
});

inline constexpr auto kRamBlocksM3779x = std::to_array<RamBlock>({
    {0x00001000, 0x000014FF},
});

inline constexpr auto kKernelBlocksM3779x = std::to_array<KernelBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kEepromBlocksM3779x = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksM3775x = std::to_array<FlashBlock>({
    {0x00001000, 0x0000FFFF},
});

inline constexpr auto kRamBlocksM3775x = std::to_array<RamBlock>({
    {0x00001000, 0x000014FF},
});

inline constexpr auto kKernelBlocksM3775x = std::to_array<KernelBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kEepromBlocksM3775x = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksMC68HC16Y5_TPU = std::to_array<FlashBlock>({
    {0x00060000, 0x00001000},
    {0x00061000, 0x00001000},
    {0x00062000, 0x00001000},
    {0x00063000, 0x00001000},
});

inline constexpr auto kFlashBlocksMC68HC16Y5 = std::to_array<FlashBlock>({
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

inline constexpr auto kRamBlocksMC68HC16Y5 = std::to_array<RamBlock>({
    {0x00020000, 0x00008000},
});

inline constexpr auto kKernelBlocksMC68HC16Y5 = std::to_array<KernelBlock>({
    {0x00020000, 0x00008000},
});

inline constexpr auto kEepromBlocksMC68HC16Y5 = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksM32R_128KB = std::to_array<FlashBlock>({
    {0x00000000, 0x00010000},
    {0x00010000, 0x00010000},
});

inline constexpr auto kRamBlocksM32R_128KB = std::to_array<RamBlock>({
    {0x00801000, 0x00001800},
});

inline constexpr auto kKernelBlocksM32R_128KB = std::to_array<KernelBlock>({
    {0x00801000, 0x00001800},
});

inline constexpr auto kEepromBlocksM32R_128KB = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksM32R_256KB = std::to_array<FlashBlock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00008000},
    {0x00010000, 0x00010000},
    {0x00020000, 0x00010000},
    {0x00030000, 0x00010000},
});

inline constexpr auto kRamBlocksM32R_256KB = std::to_array<RamBlock>({
    {0x00804000, 0x00004000},
});

inline constexpr auto kKernelBlocksM32R_256KB = std::to_array<KernelBlock>({
    {0x00804000, 0x00004000},
});

inline constexpr auto kEepromBlocksM32R_256KB = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksM32R_384KB = std::to_array<FlashBlock>({
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

inline constexpr auto kRamBlocksM32R_384KB = std::to_array<RamBlock>({
    {0x00804000, 0x00008000},
});

inline constexpr auto kKernelBlocksM32R_384KB = std::to_array<KernelBlock>({
    {0x00804000, 0x00008000},
});

inline constexpr auto kEepromBlocksM32R_384KB = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksM32R_512KB = std::to_array<FlashBlock>({
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

inline constexpr auto kFlashBlocksM32R_512KB_1block = std::to_array<FlashBlock>({
    {0x00000000, 0x00080000},
});

inline constexpr auto kFlashBlocksM32R_512KB_4blocks = std::to_array<FlashBlock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00078000},
});

// Mitsubishi Colt CZT (Z37A, ROM 47110032): readable/writeable userspace
// range. Reads use bootload session 0x85; the 0x0-0x8000 boot region is not
// touched by this protocol.
inline constexpr auto kFlashBlocksM32R_384KB_1block = std::to_array<FlashBlock>({
    {0x00008000, 0x00058000},
});

inline constexpr auto kRamBlocksM32R_512KB = std::to_array<RamBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kKernelBlocksM32R_512KB = std::to_array<KernelBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kEepromBlocksM32R_512KB = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksMH8104 = std::to_array<FlashBlock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x00078000},
});

inline constexpr auto kRamBlocksMH8104 = std::to_array<RamBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kKernelBlocksMH8104 = std::to_array<KernelBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kEepromBlocksMH8104 = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksMH5006 = std::to_array<FlashBlock>({
    {0x00000000, 0x00004000},
    {0x00004000, 0x00002000},
    {0x00006000, 0x00002000},
    {0x00008000, 0x000F8000},
});

inline constexpr auto kRamBlocksMH5006 = std::to_array<RamBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kKernelBlocksMH5006 = std::to_array<KernelBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kEepromBlocksMH5006 = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

inline constexpr auto kFlashBlocksMH8111 = std::to_array<FlashBlock>({
    {0x00000000, 0x00040000},
    {0x00040000, 0x00020000},
    {0x00060000, 0x00020000},
    {0x00080000, 0x00100000},
});

inline constexpr auto kRamBlocksMH8111 = std::to_array<RamBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kKernelBlocksMH8111 = std::to_array<KernelBlock>({
    {0x00804000, 0x0000A000},
});

inline constexpr auto kEepromBlocksMH8111 = std::to_array<EepromBlock>({
    {0x00000000, 0x00000100},
});

// name mcutype romsize numblocks fblocks rblocks kblocks eblocks;
inline constexpr auto kFlashDevices = std::to_array<FlashDevice>({
    {"M32R_128KB", kM32R128Kb, 128 * 1024, 2, kFlashBlocksM32R_128KB.data(), kRamBlocksM32R_128KB.data(),
     kKernelBlocksM32R_128KB.data(), kEepromBlocksM32R_128KB.data()},
    {"M32R_256KB", kM32R256Kb, 256 * 1024, 7, kFlashBlocksM32R_256KB.data(), kRamBlocksM32R_256KB.data(),
     kKernelBlocksM32R_256KB.data(), kEepromBlocksM32R_256KB.data()},
    {"M32R_384KB", kM32R384Kb, 384 * 1024, 9, kFlashBlocksM32R_384KB.data(), kRamBlocksM32R_384KB.data(),
     kKernelBlocksM32R_384KB.data(), kEepromBlocksM32R_384KB.data()},
    {"M32R_512KB", kM32R512Kb, 512 * 1024, 11, kFlashBlocksM32R_512KB.data(), kRamBlocksM32R_512KB.data(),
     kKernelBlocksM32R_512KB.data(), kEepromBlocksM32R_512KB.data()},
    {"M32R_512KB_1block", kM32R512Kb1block, 512 * 1024, 1, kFlashBlocksM32R_512KB_1block.data(),
     kRamBlocksM32R_512KB.data(), kKernelBlocksM32R_512KB.data(), kEepromBlocksM32R_512KB.data()},
    {"M32R_512KB_4blocks", kM32R512Kb4blocks, 512 * 1024, 4, kFlashBlocksM32R_512KB_4blocks.data(),
     kRamBlocksM32R_512KB.data(), kKernelBlocksM32R_512KB.data(), kEepromBlocksM32R_512KB.data()},
    {"M32R_384KB_1block", kM32R384Kb1block, 384 * 1024, 1, kFlashBlocksM32R_384KB_1block.data(),
     kRamBlocksM32R_512KB.data(), kKernelBlocksM32R_512KB.data(), kEepromBlocksM32R_512KB.data()},
    {"MC68HC16Y5", kMC68HC16Y5, 160 * 1024, 10, kFlashBlocksMC68HC16Y5.data(), kRamBlocksMC68HC16Y5.data(),
     kKernelBlocksMC68HC16Y5.data(), kEepromBlocksMC68HC16Y5.data()},
    {"MC68HC16Y5_TPU", kMC68HC16Y5Tpu, 4 * 1024, 4, kFlashBlocksMC68HC16Y5_TPU.data(), kRamBlocksMC68HC16Y5.data(),
     kKernelBlocksMC68HC16Y5.data(), kEepromBlocksMC68HC16Y5.data()},
    {"SH7051", kSH7051, 256 * 1024, 1, kFlashBlocksSH7051.data(), kRamBlocksSH7051.data(), kKernelBlocksSH7051.data(),
     kEepromBlocksSH7051.data()},
    {"SH7055", kSH7055, 512 * 1024, 16, kFlashBlocksSH7055.data(), kRamBlocksSH7055.data(), kKernelBlocksSH7055.data(),
     kEepromBlocksSH7055.data()},
    {"SH7058", kSH7058, 1024 * 1024, 16, kFlashBlocksSH7058.data(), kRamBlocksSH7058.data(), kKernelBlocksSH7058.data(),
     kEepromBlocksSH7058.data()},
    {"SH7058_1block", kSH7058, 1024 * 1024, 1, kFlashBlocksSH7058_1block.data(), kRamBlocksSH7058.data(),
     kKernelBlocksSH7058.data(), kEepromBlocksSH7058.data()},
    {"SH7058d", kSH7058d, 1024 * 1024, 16, kFlashBlocksSH7058d.data(), kRamBlocksSH7058d.data(),
     kKernelBlocksSH7058d.data(), kEepromBlocksSH7058d.data()},
    {"SH7059d", kSH7059d, 1536 * 1024, 16, kFlashBlocksSH7059d.data(), kRamBlocksSH7059d.data(),
     kKernelBlocksSH7059d.data(), kEepromBlocksSH7059d.data()},
    {"SH72543d", kSH72543d, 2 * 1024 * 1024, 1, kFlashBlocksSH72543d.data(), kRamBlocksSH72543d.data(),
     kKernelBlocksSH72543d.data(), kEepromBlocksSH72543d.data()},
    {"SH72531", kSH72531, 1280 * 1024, 3, kFlashBlocksSH72531.data(), kRamBlocksSH7058.data(),
     kKernelBlocksSH7058.data(), kEepromBlocksSH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"N83M_4MB", kN83M4Mb, 3984 * 1024, 3, kFlashBlocksN83M_4MB.data(), kRamBlocksSH7058.data(),
     kKernelBlocksSH7058.data(), kEepromBlocksSH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"N83M_1_5MB", kN83M15Mb, 1488 * 1024, 3, kFlashBlocksN83M_1_5MB.data(), kRamBlocksSH7058.data(),
     kKernelBlocksSH7058.data(), kEepromBlocksSH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"SH72543R", kSH72543R, 2 * 1024 * 1024, 2, kFlashBlocksSH72543R.data(), kRamBlocksSH7058.data(),
     kKernelBlocksSH7058.data(), kEepromBlocksSH7058.data()}, // rblocks, kblocks, eblocks not updated
    {"MH8104", kMH8104, 512 * 1024, 4, kFlashBlocksMH8104.data(), kRamBlocksMH8104.data(), kKernelBlocksMH8104.data(),
     kEepromBlocksMH8104.data()},
    {"MH5006", kMH5006, 1024 * 1024, 4, kFlashBlocksMH5006.data(), kRamBlocksMH5006.data(), kKernelBlocksMH5006.data(),
     kEepromBlocksMH5006.data()},
    {"MH8111", kMH8111, 3 * 512 * 1024, 4, kFlashBlocksMH8111.data(), kRamBlocksMH8111.data(),
     kKernelBlocksMH8111.data(), kEepromBlocksMH8111.data()},
    {"M3779x", kM3779x, 64 * 1024, 1, kFlashBlocksM3779x.data(), kRamBlocksM3779x.data(), kKernelBlocksM3779x.data(),
     kEepromBlocksM3779x.data()},
    {"M3775x", kM3775x, 64 * 1024, 1, kFlashBlocksM3775x.data(), kRamBlocksM3775x.data(), kKernelBlocksM3775x.data(),
     kEepromBlocksM3775x.data()},
    {0, kShInvalid, 0, 0, 0, 0, 0, 0},
});
