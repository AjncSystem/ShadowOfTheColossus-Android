// SPDX-License-Identifier: GPL-2.0-or-later
// Tiny x86-64 encoder for hand-written guest test code (M1 self-test). Only the forms the tests
// use; registers are numbered like the hardware (RAX=0 ... R15=15).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace sotc::guest::x86 {

enum Reg : uint8_t { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

class Asm {
public:
    size_t Pos() const { return b_.size(); }
    const std::vector<uint8_t>& Bytes() const { return b_; }

    void MovImm(Reg r, uint64_t v) {  // mov r64, imm64
        Rex(true, 0, 0, r);
        Byte(0xB8 + (r & 7));
        Imm(v, 8);
    }
    void Mov(Reg dst, Reg src) {  // mov dst, src
        Rex(true, src, 0, dst);
        Byte(0x89);
        Byte(0xC0 | ((src & 7) << 3) | (dst & 7));
    }
    void Add(Reg dst, Reg src) {  // add dst, src
        Rex(true, src, 0, dst);
        Byte(0x01);
        Byte(0xC0 | ((src & 7) << 3) | (dst & 7));
    }
    void AddImm8(Reg r, int8_t v) {  // add r64, imm8
        Rex(true, 0, 0, r);
        Byte(0x83);
        Byte(0xC0 | (r & 7));
        Byte(static_cast<uint8_t>(v));
    }
    void Imul(Reg dst, Reg src) {  // imul dst, src
        Rex(true, dst, 0, src);
        Byte(0x0F);
        Byte(0xAF);
        Byte(0xC0 | ((dst & 7) << 3) | (src & 7));
    }
    void ImulImm32(Reg dst, Reg src, int32_t v) {  // imul dst, src, imm32
        Rex(true, dst, 0, src);
        Byte(0x69);
        Byte(0xC0 | ((dst & 7) << 3) | (src & 7));
        Imm(static_cast<uint32_t>(v), 4);
    }
    void Dec(Reg r) {  // dec r64
        Rex(true, 0, 0, r);
        Byte(0xFF);
        Byte(0xC8 | (r & 7));
    }
    void XorEaxEax() { Byte(0x31); Byte(0xC0); }
    void LoadMem(Reg dst, Reg base) {  // mov dst, [base]   (base must not be RSP/RBP/R12/R13)
        Rex(true, dst, 0, base);
        Byte(0x8B);
        Byte(((dst & 7) << 3) | (base & 7));
    }
    void StoreMem(Reg base, Reg src) {  // mov [base], src  (same base restriction)
        Rex(true, src, 0, base);
        Byte(0x89);
        Byte(((src & 7) << 3) | (base & 7));
    }
    void PushImm8(int8_t v) { Byte(0x6A); Byte(static_cast<uint8_t>(v)); }
    void MovsdRip(uint8_t xmm, size_t target) {  // movsd xmm, [rip + target]
        Byte(0xF2);
        Byte(0x0F);
        Byte(0x10);
        Byte(0x05 | ((xmm & 7) << 3));
        Rel32(target);
    }
    void MovqFromXmm(Reg dst, uint8_t xmm) {  // movq dst, xmm
        Byte(0x66);
        Rex(true, xmm, 0, dst);
        Byte(0x0F);
        Byte(0x7E);
        Byte(0xC0 | ((xmm & 7) << 3) | (dst & 7));
    }
    void MovRaxFs0() {  // mov rax, fs:[0]
        const uint8_t code[] = {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0};
        Raw(code, sizeof(code));
    }
    void Call(size_t target) { Byte(0xE8); Rel32(target); }
    size_t JnzForward() {  // jnz rel32, returns the fixup position
        Byte(0x0F);
        Byte(0x85);
        const size_t at = Pos();
        Imm(0, 4);
        return at;
    }
    void Jnz(size_t target) { Byte(0x0F); Byte(0x85); Rel32(target); }
    void Ret() { Byte(0xC3); }
    void Hlt() { Byte(0xF4); }
    void Align(size_t a) { while (b_.size() % a) Byte(0xCC); }
    void Data64(uint64_t v) { Imm(v, 8); }
    void Raw(const uint8_t* p, size_t n) { b_.insert(b_.end(), p, p + n); }
    void Reserve(size_t n) { b_.resize(b_.size() + n, 0xCC); }
    uint8_t* At(size_t pos) { return b_.data() + pos; }

private:
    void Byte(uint8_t v) { b_.push_back(v); }
    void Imm(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) b_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void Rel32(size_t target) {  // displacement from the end of this 4-byte field
        const int64_t rel = static_cast<int64_t>(target) - static_cast<int64_t>(Pos() + 4);
        Imm(static_cast<uint32_t>(static_cast<int32_t>(rel)), 4);
    }
    void Rex(bool w, uint8_t reg, uint8_t index, uint8_t rm) {
        const uint8_t rex = 0x40 | (w ? 8 : 0) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (rm >> 3);
        if (rex != 0x40) Byte(rex);
    }
    std::vector<uint8_t> b_;
};

}  // namespace sotc::guest::x86
