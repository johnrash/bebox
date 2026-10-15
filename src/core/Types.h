/*
 * BeBox emulator - basic types and byte order helpers.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_TYPES_H
#define BEBOX_TYPES_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bebox {

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
	"the emulator assumes a little endian host");

inline u16 BSwap16(u16 v) { return __builtin_bswap16(v); }
inline u32 BSwap32(u32 v) { return __builtin_bswap32(v); }
inline u64 BSwap64(u64 v) { return __builtin_bswap64(v); }

// Guest memory is stored in guest (big endian) byte order.
inline u16 LoadBE16(const u8* p) { u16 v; memcpy(&v, p, 2); return BSwap16(v); }
inline u32 LoadBE32(const u8* p) { u32 v; memcpy(&v, p, 4); return BSwap32(v); }
inline u64 LoadBE64(const u8* p) { u64 v; memcpy(&v, p, 8); return BSwap64(v); }
inline void StoreBE16(u8* p, u16 v) { v = BSwap16(v); memcpy(p, &v, 2); }
inline void StoreBE32(u8* p, u32 v) { v = BSwap32(v); memcpy(p, &v, 4); }
inline void StoreBE64(u8* p, u64 v) { v = BSwap64(v); memcpy(p, &v, 8); }

inline u16 LoadLE16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
inline u32 LoadLE32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
inline void StoreLE16(u8* p, u16 v) { memcpy(p, &v, 2); }
inline void StoreLE32(u8* p, u32 v) { memcpy(p, &v, 4); }

// Reverse the low 'size' bytes of v (size 1, 2 or 4).
inline u32 SwapSized(u32 v, int size)
{
	if (size == 2)
		return BSwap16((u16)v);
	if (size == 4)
		return BSwap32(v);
	return v & 0xff;
}

// Index of a bus master for accesses: CPUs are 0 and 1.
enum { kInitiatorCpu0 = 0, kInitiatorCpu1 = 1, kInitiatorDma = 8 };

}	// namespace bebox

#endif	// BEBOX_TYPES_H
