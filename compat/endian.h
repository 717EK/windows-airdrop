/* Windows stand-in for glibc <endian.h>. x86/x64/ARM64 Windows is little-endian. */
#ifndef COMPAT_ENDIAN_H
#define COMPAT_ENDIAN_H

#include <stdint.h>

#define htole16(x) ((uint16_t) (x))
#define le16toh(x) ((uint16_t) (x))
#define htole32(x) ((uint32_t) (x))
#define le32toh(x) ((uint32_t) (x))
#define htole64(x) ((uint64_t) (x))
#define le64toh(x) ((uint64_t) (x))

#define htobe16(x) __builtin_bswap16((uint16_t) (x))
#define be16toh(x) __builtin_bswap16((uint16_t) (x))
#define htobe32(x) __builtin_bswap32((uint32_t) (x))
#define be32toh(x) __builtin_bswap32((uint32_t) (x))
#define htobe64(x) __builtin_bswap64((uint64_t) (x))
#define be64toh(x) __builtin_bswap64((uint64_t) (x))

#endif
