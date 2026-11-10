// Xenos microcode to HLSL. Covers straight-line programs with bool-conditional execs; anything
// else fails with a reason rather than guessing, so the unsupported list drives what to add next.
#include <algorithm>
#include <cstdio>
#include "xenos_shader.h"

namespace
{
    const char kCommon[] = R"(
cbuffer Constants : register(b0) { float4 c[512]; };
cbuffer System : register(b1)
{
    float2 ndcScale; float2 ndcOffset;
    uint bypassViewport; uint3 systemPadding;
    uint4 vbBase[24];
    uint4 samplerPoint[4];
    float4 texScale[16];
    uint4 boolConstants[2];
};
ByteAddressBuffer vb : register(t16);
SamplerState linearSampler : register(s0);
SamplerState pointSampler : register(s1);
struct Interpolated { float4 position : SV_Position; float4 t[16] : TEXCOORD0; };
bool BoolConstant(uint index) { return (boolConstants[index >> 7][(index >> 5) & 3] >> (index & 31)) & 1; }
float4 F32(uint a, uint n)
{
    float4 v = float4(0, 0, 0, 1);
    for (uint i = 0; i < n; i++) v[i] = asfloat(vb.Load(a + i * 4));
    return v;
}
float Snorm(int v, int bits) { return max(float(v) / float((1 << (bits - 1)) - 1), -1.0); }
float4 Fetch8888(uint a, bool isSigned, bool isInt)
{
    uint w = vb.Load(a);
    float4 v;
    for (uint i = 0; i < 4; i++)
    {
        uint b = (w >> (i * 8)) & 255;
        int s = int(b << 24) >> 24;
        v[i] = isSigned ? (isInt ? float(s) : Snorm(s, 8)) : (isInt ? float(b) : float(b) / 255.0);
    }
    return v;
}
float4 Fetch16(uint a, uint n, bool isSigned, bool isInt)
{
    float4 v = float4(0, 0, 0, 1);
    for (uint i = 0; i < n; i++)
    {
        uint w = vb.Load(a + (i / 2) * 4);
        uint u = (w >> ((i & 1) ? 0 : 16)) & 65535;
        int s = int(u << 16) >> 16;
        v[i] = isSigned ? (isInt ? float(s) : Snorm(s, 16)) : (isInt ? float(u) : float(u) / 65535.0);
    }
    return v;
}
float PackedField(uint w, uint offset, uint bits, bool isSigned, bool isInt)
{
    uint u = (w >> offset) & ((1u << bits) - 1u);
    int s = int(u << (32 - bits)) >> (32 - bits);
    float scale = float((1u << (bits - (isSigned ? 1u : 0u))) - 1u);
    return isSigned ? (isInt ? float(s) : max(float(s) / scale, -1.0)) : (isInt ? float(u) : float(u) / scale);
}
float4 Fetch2101010(uint a, bool s, bool i) { uint w = vb.Load(a); return float4(PackedField(w, 0, 10, s, i), PackedField(w, 10, 10, s, i), PackedField(w, 20, 10, s, i), PackedField(w, 30, 2, s, i)); }
float4 Fetch101111(uint a, bool s, bool i) { uint w = vb.Load(a); return float4(PackedField(w, 0, 10, s, i), PackedField(w, 10, 11, s, i), PackedField(w, 21, 11, s, i), 1.0); }
float4 Fetch111110(uint a, bool s, bool i) { uint w = vb.Load(a); return float4(PackedField(w, 0, 11, s, i), PackedField(w, 11, 11, s, i), PackedField(w, 22, 10, s, i), 1.0); }
float4 Fetch16F(uint a, uint n)
{
    float4 v = float4(0, 0, 0, 1);
    for (uint i = 0; i < n; i++)
    {
        uint w = vb.Load(a + (i / 2) * 4);
        v[i] = f16tof32((w >> ((i & 1) ? 0 : 16)) & 65535);
    }
    return v;
}
// CUBE takes the direction as source 0 swizzled .zzxy and returns (T, S, 2 * major axis, face).
float4 CubeVector(float4 s)
{
    float x = s.z, y = s.w, z = s.x;
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return float4(-y, z < 0.0 ? -x : x, 2.0 * z, z < 0.0 ? 5.0 : 4.0);
    if (abs(y) >= abs(x)) return float4(y < 0.0 ? -z : z, x, 2.0 * y, y < 0.0 ? 3.0 : 2.0);
    return float4(-y, x < 0.0 ? z : -z, 2.0 * x, x < 0.0 ? 1.0 : 0.0);
}
// Cube fetches take (S, T, face) with S and T in 1..2; this undoes CUBE back into a direction.
float3 CubeDirection(float3 c)
{
    float2 st = c.xy * 2.0 - 3.0;
    uint face = min(uint(max(c.z, 0.0)), 5u);
    bool negative = (face & 1u) != 0;
    float major = negative ? -1.0 : 1.0;
    if ((face >> 1) == 0) return float3(major, -st.y, negative ? st.x : -st.x);
    if ((face >> 1) == 1) return float3(st.x, major, negative ? -st.y : st.y);
    return float3(negative ? -st.x : st.x, -st.y, major);
}
)";

    constexpr uint32_t CF_EXEC = 1, CF_EXEC_END = 2, CF_COND_EXEC = 3, CF_COND_EXEC_END = 4, CF_ALLOC = 12, CF_NOP = 0;

    struct Translator
    {
        const std::vector<uint32_t>& words;
        bool pixel;
        TranslatedShader result;
        std::string body;
        int maxRegister = 1;
        uint32_t lastStride = 0;
        bool failed = false;
        std::string scalarAfter;   // statement run after the scalar result exists (predicate updates, kills)
        bool exportsDepth = false;

        void Fail(const std::string& reason)
        {
            if (!failed) result.error = reason;
            failed = true;
        }

        static std::string Format(const char* format, int a = 0, int b = 0, int c = 0, int d = 0)
        {
            char buffer[256];
            snprintf(buffer, sizeof(buffer), format, a, b, c, d);
            return buffer;
        }

        void Touch(int reg) { maxRegister = std::max(maxRegister, reg + 1); }

        std::string Register(int reg)
        {
            Touch(reg);
            return Format("r[%d]", reg);
        }

        // ALU source swizzles store each channel as an offset from identity.
        static std::string Swizzle(uint32_t bits)
        {
            std::string out = ".";
            for (int i = 0; i < 4; i++) out += "xyzw"[(((bits >> (i * 2)) & 3) + i) & 3];
            return out;
        }

        std::string Source(int index, const uint32_t* w)
        {
            static const int kSwizzleShift[4] = { 0, 16, 8, 0 }, kRegShift[4] = { 0, 16, 8, 0 }, kNegate[4] = { 0, 26, 25, 24 }, kSelect[4] = { 0, 31, 30, 29 };
            const bool isRegister = (w[2] >> kSelect[index]) & 1;
            const int reg = (w[2] >> kRegShift[index]) & 0xFF;
            std::string out;
            if (isRegister)
            {
                out = Register(reg & 0x3F);
                if (reg & 0x80) out = "abs(" + out + ")";
            }
            else
            {
                // The slot is this operand's position among the instruction's constant operands,
                // taken from the select bits so it doesn't depend on the order sources are emitted.
                int slot = 0;
                for (int j = 1; j < index; j++) if (!((w[2] >> kSelect[j]) & 1)) slot++;
                const bool addressed = slot == 0 ? ((w[1] >> 31) & 1) : ((w[1] >> 30) & 1);
                if (addressed && ((w[1] >> 29) & 1)) out = Format("c[%d + a0]", pixel ? reg + 256 : reg);
                else out = Format("c[%d]", pixel ? reg + 256 : reg);
                if ((w[0] >> 7) & 1) out = "abs(" + out + ")";   // abs_constants
            }
            out += Swizzle((w[1] >> kSwizzleShift[index]) & 0xFF);
            return ((w[1] >> kNegate[index]) & 1) ? "(-" + out + ")" : out;
        }

        std::string Destination(int reg, bool isExport)
        {
            if (!isExport) return Register(reg);
            if (pixel)
            {
                if (reg == 61) { exportsDepth = true; return "oDepth"; }
                if (reg > 3) { Fail(Format("pixel export %d", reg)); return "oC[0]"; }
                return Format("oC[%d]", reg);
            }
            if (reg == 62) return "oPos";
            if (reg == 63) return "oPointSize";
            if (reg > 15) { Fail(Format("vertex export %d", reg)); return "oT[0]"; }
            return Format("oT[%d]", reg);
        }

        static std::string Mask(uint32_t bits)
        {
            std::string out = ".";
            for (int i = 0; i < 4; i++) if (bits & (1u << i)) out += "xyzw"[i];
            return out;
        }

        std::string VectorExpression(uint32_t op, const std::string& a, const std::string& b, const std::string& c)
        {
            switch (op)
            {
            case 0: return a + " + " + b;
            case 1: return a + " * " + b;
            case 2: return "max(" + a + ", " + b + ")";
            case 3: return "min(" + a + ", " + b + ")";
            case 4: return "float4(" + a + " == " + b + ")";
            case 5: return "float4(" + a + " > " + b + ")";
            case 6: return "float4(" + a + " >= " + b + ")";
            case 7: return "float4(" + a + " != " + b + ")";
            case 8: return "frac(" + a + ")";
            case 9: return "trunc(" + a + ")";
            case 10: return "floor(" + a + ")";
            case 11: return a + " * " + b + " + " + c;
            case 12: return "lerp(" + c + ", " + b + ", float4(" + a + " == 0.0))";
            case 13: return "lerp(" + c + ", " + b + ", float4(" + a + " >= 0.0))";
            case 14: return "lerp(" + c + ", " + b + ", float4(" + a + " > 0.0))";
            case 15: return "dot(" + a + ", " + b + ").xxxx";
            case 16: return "dot((" + a + ").xyz, (" + b + ").xyz).xxxx";
            case 17: return "(dot((" + a + ").xy, (" + b + ").xy) + (" + c + ").x).xxxx";
            case 18: return "CubeVector(" + a + ")";
            case 19: { const std::string v = "(" + a + ")"; return "max(max(" + v + ".x, " + v + ".y), max(" + v + ".z, " + v + ".w)).xxxx"; }
            default:
                Fail(Format("vector opcode %d", int(op)));
                return "0.0.xxxx";
            }
        }

        // Scalar opcodes read two channels of source 3. The constant variants pair a constant with a register.
        std::string ScalarExpression(uint32_t op, const uint32_t* w)
        {
            const std::string t = "(" + Source(3, w) + ")";
            const std::string x = t + ".x", y = t + ".y";
            switch (op)
            {
            case 0: return x + " + " + y;
            case 1: return x + " + ps";
            case 2: return x + " * " + y;
            case 3: return x + " * ps";
            case 5: return "max(" + x + ", " + y + ")";
            case 6: return "min(" + x + ", " + y + ")";
            case 7: return "float(" + x + " == 0.0)";
            case 8: return "float(" + x + " > 0.0)";
            case 9: return "float(" + x + " >= 0.0)";
            case 10: return "float(" + x + " != 0.0)";
            case 11: return "frac(" + x + ")";
            case 12: return "trunc(" + x + ")";
            case 13: return "floor(" + x + ")";
            case 14: return "pow(2.0, " + x + ")";
            case 15: case 16: return "log2(" + x + ")";
            case 17: case 18: case 19: return "(1.0 / " + x + ")";
            case 20: case 21: case 22: return "(1.0 / sqrt(" + x + "))";
            case 23: scalarAfter = "a0 = clamp(int(floor(" + x + " + 0.5)), -256, 255);"; return "max(" + x + ", " + y + ")";
            case 24: scalarAfter = "a0 = clamp(int(floor(" + x + ")), -256, 255);"; return "max(" + x + ", " + y + ")";
            case 25: return x + " - " + y;
            case 26: return x + " - ps";
            case 40: return "sqrt(" + x + ")";
            case 48: return "sin(" + x + ")";
            case 49: return "cos(" + x + ")";
            case 42: case 43: case 44: case 45: case 46: case 47:
            {
                // Constant op register (Xenia's ParseAluInstruction): a constant (source 3's register,
                // swizzle component 3) and a temporary whose index is built from the opcode's low bit,
                // the source 3 select bit and swizzle bits 2-5 (component 0). Source 3's negate and the
                // abs-constants flag apply to both; the constant's relative mode comes from const_1.
                const uint32_t swizzle = w[1] & 0xFF;
                const int constant = int(w[2] & 0xFF) + (pixel ? 256 : 0);
                const int temp = int((op & 1) | (((w[2] >> 29) & 1) << 1) | (swizzle & 0x3C));
                const bool relative = ((w[1] >> 30) & 1) && ((w[1] >> 29) & 1);
                std::string a = Format(relative ? "c[%d + a0]" : "c[%d]", constant) + "." + "xyzw"[((swizzle >> 6) + 3) & 3];
                std::string b = Register(temp) + "." + "xyzw"[swizzle & 3];
                if ((w[0] >> 7) & 1) { a = "abs(" + a + ")"; b = "abs(" + b + ")"; }
                if ((w[1] >> 24) & 1) { a = "(-" + a + ")"; b = "(-" + b + ")"; }
                const char* symbol = (op == 42 || op == 43) ? " * " : (op == 44 || op == 45) ? " + " : " - ";
                return a + symbol + b;
            }
            case 27: scalarAfter = "p0 = s_ == 0.0;"; return "(" + x + " == 0.0 ? 0.0 : 1.0)";
            case 28: scalarAfter = "p0 = s_ == 0.0;"; return "(" + x + " != 0.0 ? 0.0 : 1.0)";
            case 29: scalarAfter = "p0 = s_ == 0.0;"; return "(" + x + " > 0.0 ? 0.0 : 1.0)";
            case 30: scalarAfter = "p0 = s_ == 0.0;"; return "(" + x + " >= 0.0 ? 0.0 : 1.0)";
            case 31: scalarAfter = "p0 = s_ == 0.0;"; return "(" + x + " == 1.0 ? 0.0 : (" + x + " == 0.0 ? 1.0 : " + x + "))";
            case 32: scalarAfter = "p0 = s_ == 0.0;"; return "(" + x + " - 1.0 <= 0.0 ? 0.0 : " + x + " - 1.0)";
            case 33: scalarAfter = "p0 = false;"; return "3.402823466e+38";
            case 34: scalarAfter = "p0 = s_ == 0.0;"; return x;
            case 35: case 36: case 37: case 38: case 39:
            {
                if (!pixel) { Fail("kill in a vertex shader"); return "0.0"; }
                static const char* kTests[] = { " == 0.0", " > 0.0", " >= 0.0", " != 0.0", " == 1.0" };
                scalarAfter = "if (s_ != 0.0) discard;";
                return "float(" + x + kTests[op - 35] + ")";
            }
            case 50: return "ps";
            default:
                Fail(Format("scalar opcode %d", int(op)));
                return "0.0";
            }
        }

        void EmitAlu(const uint32_t* w)
        {
            const int vectorDest = w[0] & 0x3F, scalarDest = (w[0] >> 8) & 0x3F;
            const bool isExport = (w[0] >> 15) & 1;
            const uint32_t vectorMask = (w[0] >> 16) & 0xF, scalarMask = (w[0] >> 20) & 0xF;
            const bool vectorClamp = (w[0] >> 24) & 1, scalarClamp = (w[0] >> 25) & 1;
            const uint32_t scalarOp = (w[0] >> 26) & 0x3F, vectorOp = (w[2] >> 24) & 0x1F;
            body += OpenPredicate(w[1] >> 28, w[1] >> 27);
            if (vectorOp == 29)
            {
                body += "        float4 a_ = " + Source(1, w) + ";\n";
                body += "        float4 v_ = max(a_, " + Source(2, w) + ");\n        a0 = clamp(int(floor(a_.w + 0.5)), -256, 255);\n";
            }
            else if (vectorOp >= 20 && vectorOp <= 23)
            {
                // Predicate push: true when a.w passes the test and b.w is zero; otherwise counts up a.x.
                static const char* kTests[] = { " == 0.0", " != 0.0", " > 0.0", " >= 0.0" };
                body += "        float4 a_ = " + Source(1, w) + "; float4 b_ = " + Source(2, w) + ";\n";
                body += std::string("        bool c_ = a_.w") + kTests[vectorOp - 20] + " && b_.w == 0.0;\n";
                body += "        float4 v_ = c_ ? (float4)0 : (a_.x + 1.0).xxxx;\n        p0 = c_;\n";
            }
            else if (vectorOp >= 24 && vectorOp <= 27)
            {
                if (!pixel) Fail("kill in a vertex shader");
                static const char* kTests[] = { " == ", " > ", " >= ", " != " };
                body += "        bool4 k_ = " + Source(1, w) + kTests[vectorOp - 24] + Source(2, w) + ";\n";
                body += "        if (any(k_)) discard;\n        float4 v_ = float4(k_);\n";
            }
            else
            {
                body += "        float4 v_ = " + VectorExpression(vectorOp, Source(1, w), Source(2, w), Source(3, w)) + ";\n";
            }
            scalarAfter.clear();
            body += "        float s_ = " + ScalarExpression(scalarOp, w) + ";\n";
            if (!scalarAfter.empty()) body += "        " + scalarAfter + "\n";
            if (vectorClamp) body += "        v_ = saturate(v_);\n";
            if (scalarClamp) body += "        s_ = saturate(s_);\n";
            if (isExport)
            {
                // Exports send both results to the vector destination. Components in both masks get
                // 1.0, and with the scalar-relative bit set, components in neither mask get 0.0 (Xenia's
                // GetConstant0WriteMask / GetConstant1WriteMask).
                const std::string target = Destination(vectorDest, true);
                const uint32_t vectorOnly = vectorMask & ~scalarMask, scalarOnly = scalarMask & ~vectorMask;
                const uint32_t ones = vectorMask & scalarMask;
                const uint32_t zeros = ((w[0] >> 14) & 1) ? (0xF & ~(vectorMask | scalarMask)) : 0;
                if (vectorOnly) body += "        " + target + Mask(vectorOnly) + " = v_" + Mask(vectorOnly) + ";\n";
                if (scalarOnly) body += "        " + target + Mask(scalarOnly) + " = s_;\n";
                if (ones) body += "        " + target + Mask(ones) + " = 1.0;\n";
                if (zeros) body += "        " + target + Mask(zeros) + " = 0.0;\n";
            }
            else
            {
                if (vectorMask) body += "        " + Register(vectorDest) + Mask(vectorMask) + " = v_" + Mask(vectorMask) + ";\n";
                if (scalarMask) body += "        " + Register(scalarDest) + Mask(scalarMask) + " = s_;\n";
            }
            body += "        pv = v_; ps = s_;\n    }\n";
        }

        // Predicated instructions run only when p0 matches their condition bit.
        static std::string OpenPredicate(uint32_t predicated, uint32_t condition)
        {
            if (!(predicated & 1)) return "    {\n";
            return std::string("    if (p0 == ") + ((condition & 1) ? "true" : "false") + ") {\n";
        }

        void WriteFetched(int reg, uint32_t destSwizzle)
        {
            const std::string destination = Register(reg);
            for (int i = 0; i < 4; i++)
            {
                const uint32_t select = (destSwizzle >> (i * 3)) & 7;
                if (select == 7) continue;
                std::string value;
                if (select < 4) value = std::string("f_.") + "xyzw"[select];
                else if (select == 4) value = "0.0";
                else if (select == 5) value = "1.0";
                else continue;
                body += "        " + destination + "." + "xyzw"[i] + " = " + value + ";\n";
            }
        }

        void EmitVertexFetch(const uint32_t* w)
        {
            const int source = (w[0] >> 5) & 0x3F, dest = (w[0] >> 12) & 0x3F;
            const uint32_t slot = ((w[0] >> 20) & 0x1F) * 3 + ((w[0] >> 25) & 3);
            const uint32_t sourceChannel = (w[0] >> 30) & 3;
            const uint32_t destSwizzle = w[1] & 0xFFF;
            const bool isSigned = (w[1] >> 12) & 1, isInteger = (w[1] >> 13) & 1;
            const uint32_t format = (w[1] >> 16) & 0x3F;
            uint32_t stride = w[2] & 0xFF;
            const uint32_t offset = (w[2] >> 8) & 0x7FFFFF;
            const bool mini = (w[1] >> 30) & 1;
            if (mini || stride == 0) stride = lastStride; else lastStride = stride;
            result.vertexSlots.insert(slot);

            char address[160];
            snprintf(address, sizeof(address), "vbBase[%u][%u] + uint(%s.%c) * %u + %u", slot / 4, slot % 4, Register(source).c_str(), "xyzw"[sourceChannel], stride * 4, offset * 4);
            std::string fetch;
            const std::string flags = std::string(isSigned ? "true" : "false") + ", " + (isInteger ? "true" : "false");
            switch (format)
            {
            case 6: fetch = "Fetch8888(a_, " + flags + ")"; break;
            case 7: fetch = "Fetch2101010(a_, " + flags + ")"; break;
            case 16: fetch = "Fetch101111(a_, " + flags + ")"; break;
            case 17: fetch = "Fetch111110(a_, " + flags + ")"; break;
            case 25: fetch = "Fetch16(a_, 2, " + flags + ")"; break;
            case 26: fetch = "Fetch16(a_, 4, " + flags + ")"; break;
            case 31: fetch = "Fetch16F(a_, 2)"; break;
            case 32: fetch = "Fetch16F(a_, 4)"; break;
            case 36: fetch = "F32(a_, 1)"; break;
            case 37: fetch = "F32(a_, 2)"; break;
            case 57: fetch = "F32(a_, 3)"; break;
            case 38: fetch = "F32(a_, 4)"; break;
            default:
                Fail(Format("vertex format %d", int(format)));
                fetch = "float4(0, 0, 0, 1)";
                break;
            }
            body += OpenPredicate(w[1] >> 31, w[2] >> 31);
            body += std::string("        uint a_ = ") + address + ";\n        float4 f_ = " + fetch + ";\n";
            WriteFetched(dest, destSwizzle);
            body += "    }\n";
        }

        void EmitTextureFetch(const uint32_t* w)
        {
            const uint32_t opcode = w[0] & 0x1F;
            if (opcode == 24) return;   // set texture LOD: only base levels are bound so far
            if (opcode != 1) { Fail(Format("texture opcode %d", int(opcode))); return; }
            if (!pixel) { Fail("texture fetch in a vertex shader"); return; }
            const int source = (w[0] >> 5) & 0x3F, dest = (w[0] >> 12) & 0x3F;
            const uint32_t slot = (w[0] >> 20) & 0x1F;
            if (slot >= 16) { Fail("texture slot above 15"); return; }
            const uint32_t dimension = (w[2] >> 14) & 3;   // 1D, 2D, 3D or stacked, cube
            if (dimension == 2) { Fail("3D texture fetch"); return; }
            const bool cube = dimension == 3;
            if (result.textureSlots.count(slot) && result.cubeSlots.count(slot) != (cube ? 1u : 0u)) { Fail("texture slot used as both 2D and cube"); return; }
            result.textureSlots.insert(slot);
            if (cube) result.cubeSlots.insert(slot);
            // Fetch source swizzles are absolute channel numbers, unlike ALU sources.
            std::string coordinates = Register(source) + ".";
            for (int i = 0; i < (cube ? 3 : 2); i++) coordinates += "xyzw"[(w[0] >> (26 + i * 2)) & 3];
            char sample[320];
            if (cube)
                snprintf(sample, sizeof(sample), "(samplerPoint[%u][%u] ? t%u.Sample(pointSampler, CubeDirection(%s)) : t%u.Sample(linearSampler, CubeDirection(%s)))",
                    slot / 4, slot % 4, slot, coordinates.c_str(), slot, coordinates.c_str());
            else
                snprintf(sample, sizeof(sample), "(samplerPoint[%u][%u] ? t%u.Sample(pointSampler, %s * texScale[%u].xy) : t%u.Sample(linearSampler, %s * texScale[%u].xy))",
                    slot / 4, slot % 4, slot, coordinates.c_str(), slot, slot, coordinates.c_str(), slot);
            body += OpenPredicate(w[1] >> 31, w[2] >> 31);
            body += std::string("        float4 f_ = ") + sample + ";\n";
            WriteFetched(dest, w[1] & 0xFFF);
            body += "    }\n";
        }

        void EmitExec(uint32_t address, uint32_t count, uint32_t sequence)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                const size_t first = size_t(address + i) * 3;
                if (first + 3 > words.size()) { Fail("instruction past the end of the shader"); return; }
                const uint32_t* w = words.data() + first;
                if ((sequence >> (i * 2)) & 1)
                {
                    if ((w[0] & 0x1F) == 0) EmitVertexFetch(w);
                    else EmitTextureFetch(w);
                }
                else EmitAlu(w);
            }
        }

        bool ProcessControlFlow(uint32_t d0, uint32_t d1)
        {
            const uint32_t opcode = d1 >> 12;
            switch (opcode)
            {
            case CF_NOP: case CF_ALLOC:
                return false;
            case CF_EXEC: case CF_EXEC_END:
                EmitExec(d0 & 0xFFF, (d0 >> 12) & 7, (d0 >> 16) & 0xFFF);
                return opcode == CF_EXEC_END;
            case 5: case 6:
            {
                body += std::string("    if (p0 == ") + (((d1 >> 10) & 1) ? "true" : "false") + ") {\n";
                EmitExec(d0 & 0xFFF, (d0 >> 12) & 7, (d0 >> 16) & 0xFFF);
                body += "    }\n";
                return opcode == 6;
            }
            case CF_COND_EXEC: case CF_COND_EXEC_END: case 13: case 14:
            {
                const uint32_t boolIndex = (d1 >> 2) & 0xFF;
                const bool condition = (d1 >> 10) & 1;
                body += "    if (BoolConstant(" + std::to_string(boolIndex) + ") == " + (condition ? "true" : "false") + ") {\n";
                EmitExec(d0 & 0xFFF, (d0 >> 12) & 7, (d0 >> 16) & 0xFFF);
                body += "    }\n";
                return opcode == CF_COND_EXEC_END || opcode == 14;
            }
            default:
                Fail(Format("control flow opcode %d", int(opcode)));
                return true;
            }
        }

        void Run()
        {
            bool finished = false;
            size_t limit = words.size() / 3;
            for (size_t index = 0; index < limit && !finished && !failed; index++)
            {
                const uint32_t* w = words.data() + index * 3;
                const uint32_t pair[2][2] = { { w[0], w[1] & 0xFFFF }, { (w[1] >> 16) | ((w[2] & 0xFFFF) << 16), w[2] >> 16 } };
                for (const auto& cf : pair)
                {
                    const uint32_t opcode = cf[1] >> 12;
                    if ((opcode >= CF_EXEC && opcode <= 6) || opcode == 13 || opcode == 14) limit = std::min<size_t>(limit, cf[0] & 0xFFF);
                    if (ProcessControlFlow(cf[0], cf[1])) { finished = true; break; }
                }
            }

            std::string hlsl = kCommon;
            for (uint32_t slot : result.textureSlots)
                hlsl += std::string(result.cubeSlots.count(slot) ? "TextureCube" : "Texture2D") + Format("<float4> t%d : register(t%d);\n", int(slot), int(slot));
            if (pixel && exportsDepth) hlsl += "struct PixelOutput { float4 color : SV_Target0; float depth : SV_Depth; };\n";
            hlsl += pixel ? std::string(exportsDepth ? "PixelOutput" : "float4") + " main(Interpolated input)" + (exportsDepth ? "" : " : SV_Target0") +
                                "\n{\n    float4 oC[4] = { (float4)0, (float4)0, (float4)0, (float4)0 }; float4 oDepth = (float4)0;\n"
                          : "Interpolated main(uint vertexId : SV_VertexID)\n{\n    float4 oPos = float4(0, 0, 0, 1); float4 oPointSize = (float4)0;\n    float4 oT[16];\n    for (int i_ = 0; i_ < 16; i_++) oT[i_] = (float4)0;\n";
            hlsl += Format("    float4 r[%d];\n    for (int i_ = 0; i_ < %d; i_++) r[i_] = (float4)0;\n    float4 pv = (float4)0; float ps = 0.0; bool p0 = false; int a0 = 0;\n", maxRegister, maxRegister);
            if (pixel) hlsl += Format("    for (int i_ = 0; i_ < %d; i_++) r[i_] = input.t[i_];\n", std::min(maxRegister, 16));
            else hlsl += "    r[0].x = float(vertexId);\n";
            hlsl += body;
            if (pixel)
            {
                hlsl += exportsDepth ? "    PixelOutput output_;\n    output_.color = oC[0];\n    output_.depth = oDepth.x;\n    return output_;\n}\n"
                                     : "    return oC[0];\n}\n";
            }
            else
            {
                hlsl += R"(    Interpolated output;
    if (bypassViewport)
    {
        output.position = float4(oPos.xy * ndcScale + ndcOffset, oPos.z, 1.0);
    }
    else
    {
        output.position = oPos;
    }
    for (int i_ = 0; i_ < 16; i_++) output.t[i_] = oT[i_];
    return output;
}
)";
            }
            result.hlsl = hlsl;
            result.ok = !failed;
        }
    };
}

TranslatedShader TranslateShader(const std::vector<uint32_t>& words, bool pixel, uint64_t hash)
{
    Translator translator{ words, pixel };
    translator.result.hash = hash;
    translator.result.pixel = pixel;
    translator.Run();
    return translator.result;
}
