// LZO1X decompression, and the game's "lzo" container that uses it.
//
// Many files in the game's archives (most model data, *.MDP) are such containers:
//   +0   "lzo\0"
//   +8   slot size (0x20000): block b starts at container + slot * b (block 0: 0x20 further on)
//   +12  number of blocks
//   +16  size of everything unpacked
// and each block is: offset of its data in the unpacked whole, unpacked size, packed size (all
// big-endian 32-bit), then the packed bytes. A block whose two sizes are equal is stored as it is.
// Shaders inside them are found by tools/pipeline_list and read back by prewarm.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Returns the bytes written, or -1 when the stream is not valid or does not fit.
inline int64_t Lzo1xDecompress(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize)
{
    const uint8_t* ip = in;
    const uint8_t* const ipEnd = in + inSize;
    uint8_t* op = out;
    uint8_t* const opEnd = out + outSize;
#define LZO_NEED_IN(n) if (size_t(ipEnd - ip) < size_t(n)) return -1
#define LZO_NEED_OUT(n) if (size_t(opEnd - op) < size_t(n)) return -1
    size_t t;
    // What the next instruction byte below 16 means: after a run of literals it is a three-byte
    // match far back, after a match's own trailing literals a two-byte match close by.
    enum { kStart, kAfterLiterals, kMatch } state = kStart;
    LZO_NEED_IN(1);
    if (*ip > 17)
    {
        t = size_t(*ip++) - 17;
        LZO_NEED_IN(t + 1); LZO_NEED_OUT(t);
        memcpy(op, ip, t); op += t; ip += t;
        state = t < 4 ? kMatch : kAfterLiterals;
        t = *ip++;
    }
    while (true)
    {
        if (state == kStart)
        {
            LZO_NEED_IN(1);
            t = *ip++;
            if (t < 16)
            {
                if (t == 0)
                {
                    LZO_NEED_IN(1);
                    while (*ip == 0) { t += 255; ip++; LZO_NEED_IN(1); }
                    t += 15 + *ip++;
                }
                t += 3;
                LZO_NEED_IN(t + 1); LZO_NEED_OUT(t);
                memcpy(op, ip, t); op += t; ip += t;
                state = kAfterLiterals;
                t = *ip++;
            }
            else state = kMatch;
        }
        const uint8_t* from;
        size_t length;
        if (t >= 64)
        {
            LZO_NEED_IN(1);
            from = op - 1 - ((t >> 2) & 7) - (size_t(*ip++) << 3);
            length = (t >> 5) + 1;
        }
        else if (t >= 32)
        {
            t &= 31;
            if (t == 0)
            {
                LZO_NEED_IN(1);
                while (*ip == 0) { t += 255; ip++; LZO_NEED_IN(1); }
                t += 31 + *ip++;
            }
            LZO_NEED_IN(2);
            from = op - 1 - (ip[0] >> 2) - (size_t(ip[1]) << 6);
            ip += 2;
            length = t + 2;
        }
        else if (t >= 16)
        {
            from = op - ((t & 8) << 11);
            t &= 7;
            if (t == 0)
            {
                LZO_NEED_IN(1);
                while (*ip == 0) { t += 255; ip++; LZO_NEED_IN(1); }
                t += 7 + *ip++;
            }
            LZO_NEED_IN(2);
            from -= (ip[0] >> 2) + (size_t(ip[1]) << 6);
            ip += 2;
            if (from == op) return op - out;   // the end marker
            from -= 0x4000;
            length = t + 2;
        }
        else if (state == kAfterLiterals)
        {
            LZO_NEED_IN(1);
            from = op - 0x801 - (t >> 2) - (size_t(*ip++) << 2);
            length = 3;
        }
        else
        {
            LZO_NEED_IN(1);
            from = op - 1 - (t >> 2) - (size_t(*ip++) << 2);
            length = 2;
        }
        if (from < out) return -1;
        LZO_NEED_OUT(length);
        for (size_t i = 0; i < length; i++) *op++ = *from++;   // may overlap what it writes
        t = ip[-2] & 3;
        if (t == 0) { state = kStart; continue; }
        LZO_NEED_IN(t + 1); LZO_NEED_OUT(t);
        memcpy(op, ip, t); op += t; ip += t;
        state = kMatch;
        t = *ip++;
    }
#undef LZO_NEED_IN
#undef LZO_NEED_OUT
}

struct LzoBlock { uint64_t dataAt = 0; uint32_t offset = 0, unpacked = 0, packed = 0; };

inline uint32_t LzoBe32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
// Where block `index` of the container at `container` has its 12-byte header.
inline uint64_t LzoBlockHeaderAt(uint64_t container, uint32_t slot, uint32_t index) { return container + uint64_t(slot) * index + (index == 0 ? 0x20 : 0); }
// Unpacks one block (`packed` bytes at `data`) into `out` (block.unpacked bytes). False when it is damaged.
inline bool LzoUnpackBlock(const LzoBlock& block, const uint8_t* data, uint8_t* out)
{
    if (block.packed == block.unpacked) { memcpy(out, data, block.unpacked); return true; }
    return Lzo1xDecompress(data, block.packed, out, block.unpacked) == int64_t(block.unpacked);
}
