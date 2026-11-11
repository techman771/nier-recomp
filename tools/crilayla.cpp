// Decompresses every CRILAYLA block in a CRI CPK archive into one output file (blocks one after another,
// each its 0x100-byte header followed by its data), so the decompressed contents can be searched.
// Algorithm as in CriPakTools' DecompressCRILAYLA: the compressed data is read backwards bit by bit
// and the output written from the end; the block's first 0x100 bytes are stored uncompressed after it.
// Usage: crilayla <archive.cpk> <output.bin>
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    struct BitReader
    {
        const uint8_t* data;
        int64_t offset;   // next byte, read backwards
        uint32_t pool = 0, left = 0;
        uint32_t Get(uint32_t count)
        {
            uint32_t out = 0, produced = 0;
            while (produced < count)
            {
                if (left == 0)
                {
                    if (offset < 0) return out;
                    pool = data[offset--];
                    left = 8;
                }
                const uint32_t take = std::min(left, count - produced);
                out = (out << take) | ((pool >> (left - take)) & ((1u << take) - 1));
                left -= take;
                produced += take;
            }
            return out;
        }
    };

    bool Decompress(const uint8_t* block, size_t available, std::vector<uint8_t>& out)
    {
        if (available < 16) return false;
        uint32_t size, headerOffset;
        memcpy(&size, block + 8, 4);
        memcpy(&headerOffset, block + 12, 4);
        if (size == 0 || size > (256u << 20) || uint64_t(headerOffset) + 0x10 + 0x100 > available) return false;
        out.assign(size_t(size) + 0x100, 0);
        memcpy(out.data(), block + headerOffset + 0x10, 0x100);
        BitReader bits{ block, int64_t(headerOffset) + 0x10 - 1 };
        const int64_t outputEnd = 0x100 + int64_t(size) - 1;
        static const uint32_t kLengths[4] = { 2, 3, 5, 8 };
        int64_t written = 0;
        while (written < int64_t(size))
        {
            if (bits.Get(1))
            {
                int64_t from = outputEnd - written + bits.Get(13) + 3;
                int64_t length = 3;
                int level = 0;
                for (; level < 4; level++)
                {
                    const uint32_t value = bits.Get(kLengths[level]);
                    length += value;
                    if (value != (1u << kLengths[level]) - 1) break;
                }
                if (level == 4)
                {
                    uint32_t value;
                    do
                    {
                        value = bits.Get(8);
                        length += value;
                    } while (value == 255);
                }
                for (int64_t i = 0; i < length && written < int64_t(size); i++)
                {
                    if (from < 0 || from > outputEnd) return false;
                    out[size_t(outputEnd - written)] = out[size_t(from--)];
                    written++;
                }
            }
            else
            {
                out[size_t(outputEnd - written)] = uint8_t(bits.Get(8));
                written++;
            }
        }
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: crilayla <archive.cpk> <output.bin>\n"); return 1; }
    FILE* in = fopen(argv[1], "rb");
    if (!in) return 1;
    _fseeki64(in, 0, SEEK_END);
    const int64_t size = _ftelli64(in);
    _fseeki64(in, 0, SEEK_SET);
    std::vector<uint8_t> file(static_cast<size_t>(size));
    fread(file.data(), 1, file.size(), in);
    fclose(in);
    FILE* out = fopen(argv[2], "wb");
    if (!out) return 1;
    std::vector<uint8_t> buffer;
    size_t blocks = 0, failed = 0;
    uint64_t total = 0;
    for (size_t at = 0; at + 16 <= file.size(); at++)
    {
        if (file[at] != 'C' || memcmp(&file[at], "CRILAYLA", 8) != 0) continue;
        if (Decompress(&file[at], file.size() - at, buffer))
        {
            fwrite(buffer.data(), 1, buffer.size(), out);
            total += buffer.size();
            blocks++;
        }
        else failed++;
    }
    fclose(out);
    printf("%zu blocks decompressed (%.0f MB), %zu failed\n", blocks, total / 1048576.0, failed);
    return 0;
}
