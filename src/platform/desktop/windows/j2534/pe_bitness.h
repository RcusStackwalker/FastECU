#pragma once

// Reads a PE file's IMAGE_FILE_HEADER.Machine field to determine whether it
// is a 32-bit (x86) or 64-bit (x64/arm64) image, without LoadLibrary-ing it.
// Returns false if the file can't be opened or isn't a valid PE/COFF image --
// callers should fall through to their normal load path in that case rather
// than treating detection failure as a new error mode.
bool isDll32Bit(const char *dllPath, bool& out32Bit);
