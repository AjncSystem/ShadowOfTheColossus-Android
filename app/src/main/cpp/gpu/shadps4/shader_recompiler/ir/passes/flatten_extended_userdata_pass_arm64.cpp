// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// sotc-android: the SRT walker for hosts that are not x86-64. flatten_extended_userdata_pass.cpp
// emits x86 code with Xbyak and runs it at every draw; here the same walk is emitted as a small
// stack bytecode and run by RunSrtWalker(). The IR analysis (pointer use lists, deduplication,
// ReadConst address folding, dynamic windows) is the same as there; keep the two in step.

#include "common/arch.h"

#if !defined(ARCH_X86_64)

#include <algorithm>
#include <atomic>
#include <cstring>
#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>
#include <boost/container/flat_map.hpp>
#include <boost/container/small_vector.hpp>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/value.h"

// Loads that read zero at an unmapped address (the app's guest/host_bridge.S + SIGSEGV handler).
extern "C" uint64_t sotc_safe_load64(uint64_t address);
extern "C" uint32_t sotc_safe_load32(uint64_t address);

namespace Shader {

namespace {

// ---- bytecode ----------------------------------------------------------------------------------
// One u32 per opcode, operands follow. Value stack: u32 offsets; pointer stack: the table being
// walked (starts at user_data, like rdi in the x86 walker).
enum SrtOp : u32 {
    OpEnd = 0,
    OpPushImm,     // imm            -> push imm
    OpPushFlat,    // dw             -> push flat[dw]   (GetUserData / ReadConst already placed)
    OpAdd, OpSub, OpMul, OpShl, OpShr, OpAnd, OpOr, OpXor, OpUMin, OpUMax,  // a b -> a op b
    OpNot,         // a              -> ~a
    OpBfe,         // v off cnt      -> (v >> off) & ((1 << cnt) - 1)
    OpPtrPushImm,  // dw             push ptr; ptr = load64(ptr + dw*4) & 48 bits
    OpPtrPushDyn,  // (pop off)      push ptr; ptr = load64(ptr + u32(off*4)) & 48 bits
    OpPtrPop,      //                ptr = pop
    OpLoadImm,     // src dst        flat[dst] = load32(ptr + src*4)
    OpLoadDyn,     // dst (pop off)  flat[dst] = load32(ptr + u32(off*4))
    OpCopyWindow,  // dst            SrtCopyWindow(flat + dst, ptr, window)
};
constexpr u32 kMagic = 0x42545253; // "SRTB"
constexpr u32 kVersion = 1;
constexpr u64 kPtrMask = 0xFFFFFFFFFFFFULL;

// Walkers live for the whole session (pipelines keep pointers to them).
std::mutex g_arena_mutex;
std::vector<std::unique_ptr<u32[]>> g_arena;
std::atomic<u64> g_null_reads{0};

PFN_SrtWalker StoreWalker(const u32* words, size_t count) {
    auto block = std::make_unique<u32[]>(count);
    std::memcpy(block.get(), words, count * sizeof(u32));
    const u32* ptr = block.get();
    std::scoped_lock lock{g_arena_mutex};
    g_arena.push_back(std::move(block));
    return reinterpret_cast<PFN_SrtWalker>(const_cast<u32*>(ptr));
}

bool NoCleanLoad(u64, u32, u64*) {
    return false;
}

SrtWalkerContext& WalkerContext() {
    static SrtWalkerContext context{
        .clean_pages = [] {
            auto* const pages =
                static_cast<u64*>(std::calloc(SrtCleanPageLimit >> 18, sizeof(u64)));
            ASSERT_MSG(pages, "No memory for the SRT clean page bitmap");
            return pages;
        }(),
        .clean_load = &NoCleanLoad,
    };
    return context;
}

/// Guest read for the walker: through clean_load on pages the page manager read-protects (as the
/// x86 walker does), zero for null pointers (the SotC x86 fault handler reads zero there too).
template <typename T>
T GuestLoad(const SrtWalkerContext* ctx, u64 address) {
    if (address < 0x10000) {
        if (g_null_reads.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_WARNING(Render_Recompiler, "SRT walker read zero at null table address {:#x}",
                        address);
        }
        return 0;
    }
    if (ctx && ctx->clean_pages && address < SrtCleanPageLimit) {
        const u64 page = address >> 12;
        if (__atomic_load_n(&ctx->clean_pages[page >> 6], __ATOMIC_RELAXED) & (1ULL << (page & 63))) {
            u64 value = 0;
            if (ctx->clean_load(address, sizeof(T), &value)) {
                return static_cast<T>(value);
            }
        }
    }
    // A stale table pointer (seen: 0x9bc00) reads zero instead of crashing the pipeline worker,
    // as the SotC x86 fault handler does for the Xbyak walker.
    if constexpr (sizeof(T) == 8) {
        return static_cast<T>(sotc_safe_load64(address));
    } else {
        static_assert(sizeof(T) == 4);
        return static_cast<T>(sotc_safe_load32(address));
    }
}

} // namespace

void SrtCopyWindow(u32* dst, u64 src, u64 bytes) {
    Core::Memory::Instance()->CopySparseMemory(src, reinterpret_cast<u8*>(dst), bytes);
}

bool IsSrtWalkerCode(const void*) {
    return false; // the interpreter is ordinary host code
}

const SrtWalkerContext* GetSrtWalkerContext() {
    return &WalkerContext();
}

void SetSrtCleanLoad(bool (*clean_load)(u64, u32, u64*)) {
    WalkerContext().clean_load = clean_load;
}

void SetSrtReadProtectedPages(u64 address, u64 size, bool read_protected) {
    auto& context = WalkerContext();
    if (!context.clean_pages || size == 0 || address >= SrtCleanPageLimit) {
        return;
    }
    const u64 last = std::min<u64>((address + size - 1) >> 12, (SrtCleanPageLimit >> 12) - 1);
    for (u64 page = address >> 12; page <= last; ++page) {
        u64* word = &context.clean_pages[page >> 6];
        const u64 bit = 1ULL << (page & 63);
        if (read_protected) {
            __atomic_fetch_or(word, bit, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_and(word, ~bit, __ATOMIC_RELAXED);
        }
    }
}

void MarkSrtCleanPages(u64 address, u32 size) {
    auto& context = WalkerContext();
    if (!context.clean_pages) {
        return;
    }
    for (u64 page = address >> 12; page <= (address + size - 1) >> 12; ++page) {
        if ((page << 12) < SrtCleanPageLimit) {
            context.clean_pages[page >> 6] |= 1ULL << (page & 63);
        }
    }
}

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    if (size < 3 * sizeof(u32) || size % sizeof(u32)) {
        return nullptr;
    }
    u32 header[2];
    std::memcpy(header, ptr, sizeof(header));
    if (header[0] != kMagic || header[1] != kVersion) {
        return nullptr; // cache written by another walker format: the shader is recompiled
    }
    std::vector<u32> words(size / sizeof(u32));
    std::memcpy(words.data(), ptr, size);
    return StoreWalker(words.data(), words.size());
}

void RunSrtWalker(PFN_SrtWalker walker, const u32* user_data, u32* flat,
                  const SrtWalkerContext* ctx) {
    const u32* pc = reinterpret_cast<const u32*>(walker) + 2; // past magic and version
    u32 values[64];
    u64 ptrs[64];
    int vsp = 0, psp = 0;
    u64 ptr = reinterpret_cast<u64>(user_data);
    for (;;) {
        const u32 op = *pc++;
        switch (op) {
        case OpEnd:
            return;
        case OpPushImm:
            values[vsp++] = *pc++;
            break;
        case OpPushFlat:
            values[vsp++] = flat[*pc++];
            break;
        case OpNot:
            values[vsp - 1] = ~values[vsp - 1];
            break;
        case OpBfe: {
            const u32 cnt = values[--vsp], off = values[--vsp], v = values[vsp - 1];
            values[vsp - 1] = (v >> (off & 31)) & (cnt >= 32 ? ~0u : ((1u << cnt) - 1));
            break;
        }
        case OpPtrPushImm: {
            const u32 dw = *pc++;
            ptrs[psp++] = ptr;
            ptr = GuestLoad<u64>(ctx, ptr + u64{dw} * 4) & kPtrMask;
            break;
        }
        case OpPtrPushDyn: {
            const u32 off = values[--vsp];
            ptrs[psp++] = ptr;
            ptr = GuestLoad<u64>(ctx, ptr + u64{u32(off << 2)}) & kPtrMask;
            break;
        }
        case OpPtrPop:
            ptr = ptrs[--psp];
            break;
        case OpLoadImm: {
            const u32 src = *pc++, dst = *pc++;
            flat[dst] = GuestLoad<u32>(ctx, ptr + u64{src} * 4);
            break;
        }
        case OpLoadDyn: {
            const u32 dst = *pc++;
            const u32 off = values[--vsp];
            flat[dst] = GuestLoad<u32>(ctx, ptr + u64{u32(off << 2)});
            break;
        }
        case OpCopyWindow: {
            const u32 dst = *pc++;
            SrtCopyWindow(flat + dst, ptr, u64{ReadConstDynamicWindowDwords} * sizeof(u32));
            break;
        }
        default: {
            // Binary operators: a b -> a op b (shifts mask the count like x86 does).
            const u32 b = values[--vsp];
            u32& a = values[vsp - 1];
            switch (op) {
            case OpAdd: a += b; break;
            case OpSub: a -= b; break;
            case OpMul: a *= b; break;
            case OpShl: a <<= (b & 31); break;
            case OpShr: a >>= (b & 31); break;
            case OpAnd: a &= b; break;
            case OpOr: a |= b; break;
            case OpXor: a ^= b; break;
            case OpUMin: a = std::min(a, b); break;
            case OpUMax: a = std::max(a, b); break;
            default: UNREACHABLE_MSG("bad SRT walker opcode {}", op);
            }
            break;
        }
        }
    }
}

} // namespace Shader

// ---- generator (same IR analysis as the x86 walker) ----------------------------------------------

namespace {

using namespace Shader;

struct PassInfo {
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block;
                }
                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate();
            }
        }
    };
    using PtrUserList =
        boost::container::flat_map<IR::Value, Shader::IR::Inst*, PointerListCompare>;

    Optimization::SrtGvnTable gvn_table;
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;
    std::unordered_map<u32, IR::Inst*> vn_to_inst;
    u16 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        return it != pointer_uses.end() ? &it->second : nullptr;
    }
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};

u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        return inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

void CopyFlatbufPlacement(IR::Inst* dst, const IR::Inst* src) {
    if (src->GetOpcode() == IR::Opcode::ReadConst) {
        dst->SetFlags(src->Flags<u32>());
        return;
    }
    auto inst_info = dst->Flags<IR::BufferInstInfo>();
    inst_info.flatbuf_off_dw.Assign(GetFlatbufOffset(src));
    dst->SetFlags(inst_info);
}

void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        inst_info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(inst_info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}

bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
        return true;
    default:
        return false;
    }
}

/// Appends the postfix code computing off_dw to out; false (out unchanged) when the expression
/// cannot be computed by the walker (the caller falls back, as the x86 walker does).
bool EmitOffset(std::vector<u32>& out, PassInfo& pass_info, const IR::Value& value) {
    if (value.IsImmediate()) {
        out.push_back(OpPushImm);
        out.push_back(value.U32());
        return true;
    }
    const size_t mark = out.size();
    auto fail = [&] {
        out.resize(mark);
        return false;
    };
    IR::Inst* inst = value.Inst();
    const auto binary = [&](u32 op) {
        if (!EmitOffset(out, pass_info, inst->Arg(0)) || !EmitOffset(out, pass_info, inst->Arg(1))) {
            return fail();
        }
        out.push_back(op);
        return true;
    };
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        out.push_back(OpPushFlat);
        out.push_back(static_cast<u32>(inst->Arg(0).ScalarReg()));
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        if (u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst)); offset != 0) {
            out.push_back(OpPushFlat);
            out.push_back(offset);
            return true;
        }
        return fail();
    case IR::Opcode::IAdd32: return binary(OpAdd);
    case IR::Opcode::ISub32: return binary(OpSub);
    case IR::Opcode::IMul32: return binary(OpMul);
    case IR::Opcode::ShiftLeftLogical32: return binary(OpShl);
    case IR::Opcode::ShiftRightLogical32: return binary(OpShr);
    case IR::Opcode::BitwiseAnd32: return binary(OpAnd);
    case IR::Opcode::BitwiseOr32: return binary(OpOr);
    case IR::Opcode::BitwiseXor32: return binary(OpXor);
    case IR::Opcode::UMin32: return binary(OpUMin);
    case IR::Opcode::UMax32: return binary(OpUMax);
    case IR::Opcode::BitwiseNot32:
        if (!EmitOffset(out, pass_info, inst->Arg(0))) {
            return fail();
        }
        out.push_back(OpNot);
        return true;
    case IR::Opcode::BitFieldUExtract:
        if (!EmitOffset(out, pass_info, inst->Arg(0)) || !EmitOffset(out, pass_info, inst->Arg(1)) ||
            !EmitOffset(out, pass_info, inst->Arg(2))) {
            return fail();
        }
        out.push_back(OpBfe);
        return true;
    default:
        if (static std::atomic<u32> shown{0}; shown.fetch_add(1, std::memory_order_relaxed) < 4) {
            LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                      magic_enum::enum_name(inst->GetOpcode()));
        }
        return fail();
    }
}

void VisitPointer(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pass_info,
                  std::vector<u32>& c) {
    if (subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0 ||
        subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0) {
        return;
    }
    if (off_dw.IsImmediate()) {
        c.push_back(OpPtrPushImm);
        c.push_back(off_dw.U32());
    } else {
        if (!EmitOffset(c, pass_info, off_dw)) {
            LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
            return;
        }
        c.push_back(OpPtrPushDyn);
    }
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    std::optional<u32> dynamic_window;
    for (auto [src_off_dw, use] : *use_list) {
        if (src_off_dw.IsImmediate()) {
            c.push_back(OpLoadImm);
            c.push_back(src_off_dw.U32());
            c.push_back(pass_info.dst_off_dw);
        } else {
            if (!EmitOffset(c, pass_info, src_off_dw)) {
                if (use->GetOpcode() != IR::Opcode::ReadConst) {
                    LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                    continue;
                }
                if (!dynamic_window) {
                    dynamic_window = pass_info.dst_off_dw;
                    c.push_back(OpCopyWindow);
                    c.push_back(pass_info.dst_off_dw);
                    pass_info.dst_off_dw += ReadConstDynamicWindowDwords;
                }
                use->SetFlags(*dynamic_window | ReadConstDynamicWindowFlag);
                continue;
            }
            c.push_back(OpLoadDyn);
            c.push_back(pass_info.dst_off_dw);
        }
        SetFlatbufOffset(use, pass_info.dst_off_dw);
        pass_info.dst_off_dw++;
    }
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, c);
        }
    }
    c.push_back(OpPtrPop);
}

void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    if (pass_info.srt_roots.empty()) {
        return;
    }
    std::vector<u32> code{kMagic, kVersion};
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);
    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, code);
    }
    code.push_back(OpEnd);
    info.srt_info.walker_func = StoreWalker(code.data(), code.size());
    info.srt_info.walker_func_size = code.size() * sizeof(u32);
    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    if (inst.GetOpcode() != IR::Opcode::ReadConst) {
        return;
    }
    IR::Inst* addr = inst.Arg(0).Inst();
    IR::Inst* hi = addr->Arg(1).TryInst();
    IR::Inst* lo = addr->Arg(0).TryInst();
    bool lo_readfirstlane = false;
    if (lo && lo->GetOpcode() == IR::Opcode::ReadFirstLane && hi &&
        hi->GetOpcode() == IR::Opcode::ReadFirstLane) {
        lo = lo->Arg(0).TryInst();
        hi = hi->Arg(0).TryInst();
        lo_readfirstlane = true;
    }
    if (!hi || !lo) {
        return;
    }
    if (lo->GetOpcode() != IR::Opcode::IAdd32 || hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }
    IR::Value add_offset;
    IR::Value base;
    if (base = lo->Arg(0); IsReadConstSource(base)) {
        add_offset = lo->Arg(1);
    } else if (base = lo->Arg(1); IsReadConstSource(base)) {
        add_offset = lo->Arg(0);
    } else {
        return;
    }
    IR::Inst* sel = hi->Arg(1).TryInst();
    if (!sel || sel->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }
    IR::Value sel_true = sel->Arg(1);
    IR::Value sel_false = sel->Arg(2);
    if (!sel_true.IsImmediate() || !sel_false.IsImmediate() || sel_true.U32() != 1 ||
        sel_false.U32() != 0) {
        return;
    }
    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (auto [user, operand] : addr->Uses()) {
        ASSERT(user->GetOpcode() == IR::Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        IR::U32 offset = IR::U32{user->Arg(1)};
        const IR::U32 byte_offset = add_offset.IsImmediate() || !lo_readfirstlane
                                        ? IR::U32{add_offset}
                                        : ir.ReadFirstLane(IR::U32{add_offset});
        IR::U32 dw_add_offset = add_offset.IsImmediate()
                                    ? ir.Imm32(add_offset.U32() >> 2u)
                                    : IR::U32{ir.ShiftRightLogical(byte_offset, ir.Imm32(2u))};
        if (offset.IsImmediate() && dw_add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + dw_add_offset.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, dw_add_offset));
        }
    }
}

} // namespace

namespace Shader::Optimization {

void FlattenExtendedUserdataPass(IR::Program& program) {
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }
            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                if (!inst.Flags<IR::BufferInstInfo>().sharp_source) {
                    continue;
                }
            }
            SimplifyReadConstAddressAdd(inst);
            all_readconsts.push_back(&inst);
            auto offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }
            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* inst{queue.front()};
                queue.pop();
                if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = inst->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(inst);
                        auto offset = inst->Arg(1);
                        if (!offset.IsImmediate()) {
                            auto arg_inst = offset.Inst();
                            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                                visited.push_back(arg_inst);
                                queue.push(arg_inst);
                            }
                        }
                    }
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                    inst->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(inst)) {
                    continue;
                }
                for (size_t arg = inst->NumArgs(); arg--;) {
                    const IR::Value arg_value = inst->Arg(arg);
                    if (arg_value.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* arg_inst = arg_value.Inst();
                    if (std::ranges::find(visited, arg_inst) == visited.end()) {
                        visited.push_back(arg_inst);
                        queue.push(arg_inst);
                    }
                }
            }
        }
    }

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            continue;
        }
        IR::Inst* base = inst->Arg(0).Inst();
        if (auto* inst = base->Arg(0).TryInst();
            inst && inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        ASSERT_MSG(IsReadConstSource(base->Arg(0)), "ReadConst base low not from constant memory");
        ASSERT_MSG(IsReadConstSource(base->Arg(1)), "ReadConst base high not from constant memory");
        IR::Inst* ptr_lo = base->Arg(0).Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);
        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;
        user_list[inst->Arg(1)] = inst;
        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        CopyFlatbufPlacement(readconst, original);
    }
    program.info.RefreshFlatBuf();
}

} // namespace Shader::Optimization

#endif // !ARCH_X86_64
