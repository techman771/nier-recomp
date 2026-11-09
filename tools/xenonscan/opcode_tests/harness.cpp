// Runs recompiler-emitted C++ for the opcodes added for NieR against expected values derived
// from the PowerPC architecture. Register numbers come from the instruction instances themselves.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "ppc_config.h"
#include "ppc_context.h"
#include "emitted.inc"

static int gFailures = 0, gChecks = 0;

#define CHECK(name, cond) do { gChecks++; if (!(cond)) { gFailures++; printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); } } while (0)

static PPCRegister* R[32];
static PPCRegister* F[32];
static PPCVRegister* V[128];
static PPCCRRegister* CR[8];
static uint8_t* mem;
static PPCContext* C;

#define MAP(arr, field, n) arr[n] = &C->field##n
static void Setup(PPCContext& c)
{
    C = &c;
#define R4(a, b, c_, d) MAP(R, r, a); MAP(R, r, b); MAP(R, r, c_); MAP(R, r, d)
#define F4(a, b, c_, d) MAP(F, f, a); MAP(F, f, b); MAP(F, f, c_); MAP(F, f, d)
#define V4(a, b, c_, d) MAP(V, v, a); MAP(V, v, b); MAP(V, v, c_); MAP(V, v, d)
    R4(0,1,2,3); R4(4,5,6,7); R4(8,9,10,11); R4(12,13,14,15); R4(16,17,18,19); R4(20,21,22,23); R4(24,25,26,27); R4(28,29,30,31);
    F4(0,1,2,3); F4(4,5,6,7); F4(8,9,10,11); F4(12,13,14,15); F4(16,17,18,19); F4(20,21,22,23); F4(24,25,26,27); F4(28,29,30,31);
    for (int base = 0; base < 128; base += 4)
    {
        switch (base / 4)
        {
#define VC(i, a, b, c_, d) case i: V4(a, b, c_, d); break;
        VC(0,0,1,2,3) VC(1,4,5,6,7) VC(2,8,9,10,11) VC(3,12,13,14,15) VC(4,16,17,18,19) VC(5,20,21,22,23) VC(6,24,25,26,27) VC(7,28,29,30,31)
        VC(8,32,33,34,35) VC(9,36,37,38,39) VC(10,40,41,42,43) VC(11,44,45,46,47) VC(12,48,49,50,51) VC(13,52,53,54,55) VC(14,56,57,58,59) VC(15,60,61,62,63)
        VC(16,64,65,66,67) VC(17,68,69,70,71) VC(18,72,73,74,75) VC(19,76,77,78,79) VC(20,80,81,82,83) VC(21,84,85,86,87) VC(22,88,89,90,91) VC(23,92,93,94,95)
        VC(24,96,97,98,99) VC(25,100,101,102,103) VC(26,104,105,106,107) VC(27,108,109,110,111) VC(28,112,113,114,115) VC(29,116,117,118,119) VC(30,120,121,122,123) VC(31,124,125,126,127)
        }
    }
    CR[0] = &c.cr0; CR[1] = &c.cr1; CR[2] = &c.cr2; CR[3] = &c.cr3; CR[4] = &c.cr4; CR[5] = &c.cr5; CR[6] = &c.cr6; CR[7] = &c.cr7;
}

static void Put32(uint32_t a, uint32_t v) { mem[a] = v >> 24; mem[a + 1] = v >> 16; mem[a + 2] = v >> 8; mem[a + 3] = v; }
static void Put16(uint32_t a, uint16_t v) { mem[a] = v >> 8; mem[a + 1] = uint8_t(v); }
static void Put64(uint32_t a, uint64_t v) { Put32(a, uint32_t(v >> 32)); Put32(a + 4, uint32_t(v)); }
static uint32_t Get32(uint32_t a) { return (uint32_t(mem[a]) << 24) | (mem[a + 1] << 16) | (mem[a + 2] << 8) | mem[a + 3]; }
static uint16_t Get16(uint32_t a) { return uint16_t((mem[a] << 8) | mem[a + 1]); }
static uint64_t Get64(uint32_t a) { return (uint64_t(Get32(a)) << 32) | Get32(a + 4); }

static bool CrBit(int bi)
{
    const auto& c = *CR[bi / 4];
    return (bi % 4) == 0 ? c.lt : (bi % 4) == 1 ? c.gt : (bi % 4) == 2 ? c.eq : c.so;
}
static void SetCrBit(int bi, bool value)
{
    auto& c = *CR[bi / 4];
    ((bi % 4) == 0 ? c.lt : (bi % 4) == 1 ? c.gt : (bi % 4) == 2 ? c.eq : c.so) = value;
}

using Emit = void (*)(PPCContext&, uint8_t*, bool&);

static void TestCtrBranch(const char* n, Emit e, const int* ops, bool whenZero, bool whenSet)
{
    for (uint64_t ctr : { 1ull, 2ull, 5ull })
        for (bool bit : { false, true })
        {
            C->ctr.u64 = ctr;
            SetCrBit(ops[0], bit);
            bool taken = false;
            e(*C, mem, taken);
            bool expect = ((ctr - 1 == 0) == whenZero) && (bit == whenSet);
            CHECK(n, taken == expect);
            CHECK(n, C->ctr.u64 == ctr - 1);
        }
}

// Update-form addressing: returns the address the instruction should touch and primes RA/RB.
static uint32_t PrimeEa(bool indexed, const int* ops, int& ra, int& rt)
{
    rt = ops[0];
    if (indexed)
    {
        ra = ops[1];
        R[ra]->u64 = 0x1000;
        R[ops[2]]->u64 = 0x140;
        return 0x1140;
    }
    ra = ops[2];
    R[ra]->u64 = 0x1000;
    return uint32_t(0x1000 + ops[1]);
}

static void TestLoadInt(const char* n, Emit e, const int* ops, bool indexed, int size, bool sign)
{
    int ra, rt;
    uint32_t a = PrimeEa(indexed, ops, ra, rt);
    uint64_t pattern = size == 8 ? 0x8123456789ABCDEFull : size == 4 ? 0x89ABCDEFull : size == 2 ? 0x89ABull : 0x89ull;
    if (size == 8) Put64(a, pattern); else if (size == 4) Put32(a, uint32_t(pattern)); else if (size == 2) Put16(a, uint16_t(pattern)); else mem[a] = uint8_t(pattern);
    bool t = false;
    e(*C, mem, t);
    uint64_t expect = sign ? uint64_t(int64_t(int16_t(pattern))) : pattern;
    CHECK(n, R[rt]->u64 == expect);
    CHECK(n, R[ra]->u32 == a);
}

static void TestStoreInt(const char* n, Emit e, const int* ops, bool indexed, int size)
{
    int ra, rt;
    uint32_t a = PrimeEa(indexed, ops, ra, rt);
    memset(mem + a - 8, 0x55, 32);
    R[rt]->u64 = 0xFEDCBA9876543210ull;
    bool t = false;
    e(*C, mem, t);
    if (size == 8) CHECK(n, Get64(a) == 0xFEDCBA9876543210ull);
    else CHECK(n, Get16(a) == 0x3210);
    CHECK(n, mem[a + size] == 0x55);
    CHECK(n, mem[a - 1] == 0x55);
    CHECK(n, R[ra]->u32 == a);
}

static void TestFloatLoad(const char* n, Emit e, const int* ops, bool indexed, bool dbl)
{
    int ra, rt;
    uint32_t a = PrimeEa(indexed, ops, ra, rt);
    if (dbl) { double d = -1234.5625; uint64_t u; memcpy(&u, &d, 8); Put64(a, u); }
    else { float f = -1234.5625f; uint32_t u; memcpy(&u, &f, 4); Put32(a, u); }
    bool t = false;
    e(*C, mem, t);
    CHECK(n, F[rt]->f64 == -1234.5625);
    CHECK(n, R[ra]->u32 == a);
}

static void TestFloatStore(const char* n, Emit e, const int* ops, bool indexed, bool dbl)
{
    int ra, rt;
    uint32_t a = PrimeEa(indexed, ops, ra, rt);
    F[rt]->f64 = 2.5;
    bool t = false;
    e(*C, mem, t);
    if (dbl) { uint64_t u = Get64(a); double d; memcpy(&d, &u, 8); CHECK(n, d == 2.5); }
    else { uint32_t u = Get32(a); float f; memcpy(&f, &u, 4); CHECK(n, f == 2.5f); }
    CHECK(n, R[ra]->u32 == a);
}

static void TestArith(const char* n, Emit e, const int* ops, int kind, bool rc)
{
    static const uint32_t vals[] = { 0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFF0, 0x20 };
    for (uint32_t x : vals)
        for (uint32_t y : vals)
            for (int ca : { 0, 1 })
            {
                int rt = ops[0], ra = ops[1];
                R[ra]->u64 = x;
                if (kind == 0 || kind == 3) R[ops[2]]->u64 = y;
                uint32_t a = R[ra]->u32;
                uint32_t b = (kind == 0 || kind == 3) ? R[ops[2]]->u32 : 0;
                C->xer.ca = uint8_t(ca);
                bool t = false;
                e(*C, mem, t);
                uint64_t wide = 0;
                uint32_t expect = 0;
                bool checkCa = true;
                if (kind == 0) { wide = uint64_t(a) + b; expect = uint32_t(wide); }          // addc
                if (kind == 1) { wide = uint64_t(a) + 0xFFFFFFFFull + ca; expect = uint32_t(wide); } // addme
                if (kind == 2) { wide = uint64_t(uint32_t(~a)) + ca; expect = uint32_t(wide); }       // subfze
                if (kind == 3) { expect = ~(a ^ b); checkCa = false; }                       // eqv
                CHECK(n, R[rt]->u32 == expect);
                if (checkCa) CHECK(n, C->xer.ca == ((wide >> 32) & 1));
                if (rc)
                {
                    int32_t s = int32_t(expect);
                    CHECK(n, C->cr0.lt == (s < 0));
                    CHECK(n, C->cr0.gt == (s > 0));
                    CHECK(n, C->cr0.eq == (s == 0));
                }
            }
}

static void TestVcmp(const char* n, Emit e, const int* ops, bool rc)
{
    struct Case { uint32_t a[4], b[4]; uint32_t out[4]; bool all, none; };
    const Case cases[] = {
        // High-bit values: a signed compare gets every one of these wrong.
        { {0xFFFFFFFF,0x80000000,0xFFFFFFFF,0x80000001}, {0,0,0,0}, {~0u,~0u,~0u,~0u}, true, false },
        { {0,1,2,3}, {0xFFFFFFFF,0x80000000,2,4}, {0,0,0,0}, false, true },
        { {5,1,0xF0000000,3}, {4,2,1,3}, {~0u,0,~0u,0}, false, false },
    };
    for (const auto& c : cases)
    {
        int va = ops[1], vb = ops[2];
        if (va == vb) continue;
        memcpy(V[va]->u32, c.a, 16);
        memcpy(V[vb]->u32, c.b, 16);
        bool t = false;
        e(*C, mem, t);
        CHECK(n, memcmp(V[ops[0]]->u32, c.out, 16) == 0);
        if (rc)
        {
            CHECK(n, C->cr6.lt == c.all);
            CHECK(n, C->cr6.eq == c.none);
        }
    }
}

static void TestVnor(const char* n, Emit e, const int* ops)
{
    memset(V[ops[1]]->u8, 0xA5, 16);
    memset(V[ops[2]]->u8, 0x0F, 16);
    uint8_t expect = ops[1] == ops[2] ? uint8_t(~0x0F) : uint8_t(~(0xA5 | 0x0F));
    bool t = false;
    e(*C, mem, t);
    for (int i = 0; i < 16; i++) CHECK(n, V[ops[0]]->u8[i] == expect);
}

static void TestVsel(const char* n, Emit e, const int* ops)
{
    // Result bit comes from vB where the selector (vD) is 1, otherwise from vA.
    if (ops[1] == ops[2] || ops[1] == ops[0] || ops[2] == ops[0]) { CHECK(n, !"instance operands alias"); return; }
    memset(V[ops[1]]->u8, 0xCC, 16);
    memset(V[ops[2]]->u8, 0x33, 16);
    memset(V[ops[0]]->u8, 0xF0, 16);
    bool t = false;
    e(*C, mem, t);
    for (int i = 0; i < 16; i++) CHECK(n, V[ops[0]]->u8[i] == uint8_t((0x33 & 0xF0) | (0xCC & 0x0F)));
}

static void TestLvehx(const char* n, Emit e, const int* ops)
{
    uint32_t ea = 0x2004;
    if (ops[1]) R[ops[1]]->u64 = 0x2000;
    R[ops[2]]->u64 = ops[1] ? 4 : ea;
    for (int i = 0; i < 16; i++) mem[0x2000 + i] = uint8_t(0x10 + i);
    bool t = false;
    e(*C, mem, t);
    // Big-endian halfword element k lives at u16[7 - k] because vectors are stored reversed.
    CHECK(n, V[ops[0]]->u16[7 - (ea & 0xF) / 2] == Get16(ea));
}

static void TestVrfip(const char* n, Emit e, const int* ops)
{
    const float in[4] = { 1.25f, -1.75f, 3.0f, -0.25f };
    memcpy(V[ops[1]]->f32, in, 16);
    bool t = false;
    e(*C, mem, t);
    for (int i = 0; i < 4; i++) CHECK(n, V[ops[0]]->f32[i] == std::ceil(in[i]));
}

static void TestVpkswss(const char* n, Emit e, const int* ops)
{
    if (ops[1] == ops[2]) { CHECK(n, !"instance operands alias"); return; }
    const int32_t a[4] = { 1, -2, 70000, -70000 };       // big-endian words 0..3 of vA
    const int32_t b[4] = { 32767, 32768, -32768, -32769 }; // big-endian words 0..3 of vB
    for (int i = 0; i < 4; i++) { V[ops[1]]->s32[3 - i] = a[i]; V[ops[2]]->s32[3 - i] = b[i]; }
    const int16_t expect[8] = { 1, -2, 32767, -32768, 32767, 32767, -32768, -32768 };
    bool t = false;
    e(*C, mem, t);
    for (int i = 0; i < 8; i++) CHECK(n, V[ops[0]]->s16[7 - i] == expect[i]);
}

static void TestVcmpb(const char* n, Emit e, const int* ops)
{
    const float a[4] = { 1.0f, 5.0f, -3.0f, std::nanf("") };
    const float b[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
    const uint32_t expect[4] = { 0, 0x80000000u, 0x40000000u, 0xC0000000u };
    memcpy(V[ops[1]]->f32, a, 16);
    memcpy(V[ops[2]]->f32, b, 16);
    bool t = false;
    e(*C, mem, t);
    CHECK(n, memcmp(V[ops[0]]->u32, expect, 16) == 0);
}

static void TestVupkShort4(const char* n, Emit e, const int* ops)
{
    // Big-endian shorts x, y, z, w sit at s16[3], s16[2], s16[1], s16[0]. Results are the bit
    // pattern of 3.0f plus the sign-extended short, so a short's sign must borrow correctly.
    memset(V[ops[1]]->u8, 0x77, 16);
    V[ops[1]]->s16[3] = 0x1234;
    V[ops[1]]->s16[2] = -2;
    V[ops[1]]->s16[1] = 7;
    V[ops[1]]->s16[0] = -32768;
    bool t = false;
    e(*C, mem, t);
    CHECK(n, V[ops[0]]->u32[3] == 0x40401234u);
    CHECK(n, V[ops[0]]->u32[2] == 0x403FFFFEu);
    CHECK(n, V[ops[0]]->u32[1] == 0x40400007u);
    CHECK(n, V[ops[0]]->u32[0] == 0x403F8000u);
}

static void TestVpkFloat162(const char* n, Emit e, const int* ops)
{
    // x and y are big-endian elements 0 and 1, and shift 3 targets the leftmost word, u32[3].
    for (int i = 0; i < 4; i++) V[ops[1]]->f32[i] = 9.0f;
    V[ops[1]]->f32[3] = 1.0f;
    V[ops[1]]->f32[2] = -2.5f;
    memset(V[ops[0]]->u8, 0xAA, 16);
    bool t = false;
    e(*C, mem, t);
    CHECK(n, V[ops[0]]->u16[7] == 0x3C00);
    CHECK(n, V[ops[0]]->u16[6] == 0xC100);
    CHECK(n, V[ops[0]]->u32[2] == 0xAAAAAAAAu);
    CHECK(n, V[ops[0]]->u32[1] == 0xAAAAAAAAu);
    CHECK(n, V[ops[0]]->u32[0] == 0xAAAAAAAAu);
}

int main()
{
    static PPCContext ctx;
    Setup(ctx);
    mem = (uint8_t*)calloc(1, 1 << 20);

#define T(name) emit_##name, ops_##name
    TestCtrBranch("bdzf", T(bdzf), true, false);
    TestCtrBranch("bdnzt", T(bdnzt), false, true);
    TestFloatLoad("lfsu", T(lfsu), false, false);
    TestFloatLoad("lfsux", T(lfsux), true, false);
    TestFloatLoad("lfdu", T(lfdu), false, true);
    TestFloatStore("stfsu", T(stfsu), false, false);
    TestFloatStore("stfsux", T(stfsux), true, false);
    TestFloatStore("stfdu", T(stfdu), false, true);
    TestStoreInt("sthu", T(sthu), false, 2);
    TestStoreInt("sthux", T(sthux), true, 2);
    TestStoreInt("stdux", T(stdux), true, 8);
    TestLoadInt("lhzu", T(lhzu), false, 2, false);
    TestLoadInt("lhzux", T(lhzux), true, 2, false);
    TestLoadInt("lhau", T(lhau), false, 2, true);
    TestLoadInt("lwzux", T(lwzux), true, 4, false);
    TestLoadInt("lbzux", T(lbzux), true, 1, false);
    TestLoadInt("ldux", T(ldux), true, 8, false);
    TestArith("addc", T(addc), 0, false);
    TestArith("addme", T(addme), 1, false);
    TestArith("subfze", T(subfze), 2, false);
    TestArith("subfze.", T(subfze_rc), 2, true);
    TestArith("eqv", T(eqv), 3, false);
    TestVcmp("vcmpgtuw", T(vcmpgtuw), false);
    TestVcmp("vcmpgtuw.", T(vcmpgtuw_rc), true);
    TestVnor("vnor128", T(vnor128));
    TestVsel("vsel128", T(vsel128));
    TestLvehx("lvehx", T(lvehx));
    TestVrfip("vrfip128", T(vrfip128));
    TestVpkswss("vpkswss128", T(vpkswss128));
    TestVcmpb("vcmpbfp128", T(vcmpbfp128));
    TestVupkShort4("vupkd3d128", T(vupkd3d128));
    TestVpkFloat162("vpkd3d128", T(vpkd3d128));

    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
