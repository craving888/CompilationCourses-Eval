#include "codegen/riscv.hpp"
#include "codegen/allocate.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace {
using ir::Op;
using ir::Inst;

std::string symbol(const std::string& name) {
    if (name == "main" || name == "getint" || name == "putint") return name;
    return ".Ltoy_fn_" + name;
}

struct Emitter {
    const ir::Function& f;
    std::ostream& out;
    int index;
    bool optimize;
    Allocation allocation;
    int next_block = -2;
    int64_t outgoing = 0, frame = 0;
    bool save_ra = true;
    std::vector<std::optional<int32_t>> constants = {};
    std::vector<int> uses = {};
    std::string label(int block) const {
        return ".Ltoy_" + std::to_string(index) + "_" + (block < 0 ? "exit" : std::to_string(block));
    }
    void ins(const std::string& text) { out << "    " << text << '\n'; }
    void memory(const std::string& op,const std::string& reg,int64_t offset) {
        if (offset <= 2047) ins(op + " " + reg + ", " + std::to_string(offset) + "(sp)");
        else {
            ins("li t6, " + std::to_string(offset));
            ins("add t6, sp, t6");
            ins(op + " " + reg + ", 0(t6)");
        }
    }
    void adjust(int64_t delta) {
        if (delta >= -2048 && delta <= 2047) ins("addi sp, sp, " + std::to_string(delta));
        else {
            ins("li t6, " + std::to_string(delta));
            ins("add sp, sp, t6");
        }
    }
    int64_t slot(int id) const { return outgoing + int64_t(4) * id; }
    std::string virtual_reg(int id) const { return "t" + std::to_string(2+allocation.regs[id]); }
    std::string local_reg(int id) const { return "s" + std::to_string(1+allocation.locals[id]); }
    void move(const std::string& dst,const std::string& src) {
        if (dst != src) ins("mv " + dst + ", " + src);
    }
    void get(const std::string& reg,int id) {
        if (optimize && constants[id]) ins("li " + reg + ", " + std::to_string(*constants[id]));
        else if (allocation.regs[id] >= 0) move(reg,virtual_reg(id));
        else memory("lw",reg,slot(f.slots+allocation.spill[id]));
    }
    void put(const std::string& reg,int id) {
        if (allocation.regs[id] >= 0) move(virtual_reg(id),reg);
        else memory("sw",reg,slot(f.slots+allocation.spill[id]));
    }
    void jump(int target) {
        if (!optimize || target != next_block) ins("j " + label(target));
    }
    static std::string arg(int id) { return "a" + std::to_string(id); }

    std::string operand(int id,const std::string& scratch) {
        if (optimize && constants[id] == 0) return "zero";
        if (allocation.regs[id] >= 0) return virtual_reg(id);
        get(scratch,id);
        return scratch;
    }
    std::string destination(int id) const {
        return allocation.regs[id] >= 0 ? virtual_reg(id) : "t0";
    }
    void emit_binary(const Inst& i) {
        const auto& op = i.name;
        const auto dst = destination(i.dst);
        int lhs = i.args[0], rhs = i.args[1];
        if (optimize && constants[lhs] && (op == "+" || op == "*")) std::swap(lhs,rhs);
        if (optimize && constants[rhs]) {
            int64_t c = *constants[rhs];
            const int64_t imm = op == "-" ? -c : c;
            if ((op == "+" || op == "-") && imm >= -2048 && imm <= 2047) {
                const auto src = operand(lhs,"t0");
                ins("addiw " + dst + ", " + src + ", " + std::to_string(imm));
                put(dst,i.dst);
                return;
            }
            if (op == "*" && c > 0 && !(c & (c-1))) {
                const auto src = operand(lhs,"t0");
                int shift = __builtin_ctz(static_cast<unsigned>(c));
                ins("slliw " + dst + ", " + src + ", " + std::to_string(shift));
                put(dst,i.dst);
                return;
            }
        }
        const auto a = operand(lhs,"t0");
        const auto b = operand(rhs,"t1");
        binary(op,dst,a,b);
        put(dst,i.dst);
    }

    void binary(const std::string& op,const std::string& dst,const std::string& a,const std::string& b) {
        static const std::unordered_map<std::string,std::string> plain{
            {"+","addw"},{"-","subw"},{"*","mulw"},{"/","divw"},{"%","remw"},{"<","slt"}
        };
        auto p = plain.find(op);
        if (p != plain.end()) ins(p->second + " " + dst + ", " + a + ", " + b);
        else if (op == ">") ins("slt " + dst + ", " + b + ", " + a);
        else if (op == "==" || op == "!=") {
            ins("xor " + dst + ", " + a + ", " + b);
            ins(std::string(op == "==" ? "seqz " : "snez ") + dst + ", " + dst);
        } else if (op == "<=" || op == ">=") {
            ins("slt " + dst + (op == "<=" ? ", " + b + ", " + a : ", " + a + ", " + b));
            ins("xori " + dst + ", " + dst + ", 1");
        } else throw std::logic_error("RV64: unsupported binary operator " + op);
    }

    void branch(const Inst& i,const std::string& yes,const std::string& no,
                const std::string& a,const std::string& b) {
        if (i.data == next_block) {
            ins(no + " " + a + ", " + b + ", " + label(i.other));
        } else {
            ins(yes + " " + a + ", " + b + ", " + label(i.data));
            jump(i.other);
        }
    }

    bool fuse_compare(const Inst& compare,const Inst& next) {
        if (!optimize || compare.op != Op::Binary || next.op != Op::Branch ||
            next.args[0] != compare.dst || uses[compare.dst] != 1) return false;
        auto a = compare.args[0], b = compare.args[1];
        std::string yes, no;
        if (compare.name == "==" || compare.name == "!=") {
            yes = compare.name == "==" ? "beq" : "bne";
            no = compare.name == "==" ? "bne" : "beq";
        } else if (compare.name == "<" || compare.name == ">" || compare.name == "<=" || compare.name == ">=") {
            const auto& op = compare.name;
            if (op == ">" || op == "<=") std::swap(a,b);
            yes = op == "<" || op == ">" ? "blt" : "bge";
            no = yes == "blt" ? "bge" : "blt";
        } else return false;
        const auto src_a = operand(a,"t0");
        const auto src_b = operand(b,"t1");
        branch(next,yes,no,src_a,src_b);
        return true;
    }

    void emit(const Inst& i) {
        switch (i.op) {
        case Op::Constant:
            if (!optimize) { ins("li t0, " + std::to_string(i.data)); put("t0",i.dst); }
            break;
        case Op::Param:
            if (i.data < 8) put(arg(i.data),i.dst);
            else { memory("lw","t0",frame + 8LL*(i.data-8)); put("t0",i.dst); }
            break;
        case Op::Load:
            if (allocation.locals[i.data] >= 0) put(local_reg(i.data),i.dst);
            else {
                const auto dst = destination(i.dst);
                memory("lw",dst,slot(i.data));
                put(dst,i.dst);
            }
            break;
        case Op::Store:
            if (allocation.locals[i.data] >= 0) get(local_reg(i.data),i.args[0]);
            else memory("sw",operand(i.args[0],"t0"),slot(i.data));
            break;
        case Op::Unary: {
            const auto src = operand(i.args[0],"t0");
            const auto dst = destination(i.dst);
            if (i.name == "-") ins("negw " + dst + ", " + src);
            else if (i.name == "!") ins("seqz " + dst + ", " + src);
            else move(dst,src);
            put(dst,i.dst);
            break;
        }
        case Op::Binary: emit_binary(i); break;
        case Op::Call:
            for (int j = 0; j < static_cast<int>(i.args.size()); ++j) {
                if (j < 8) get(arg(j),i.args[j]);
                else { get("t0",i.args[j]); memory("sd","t0",8LL*(j-8)); }
            }
            ins("call " + symbol(i.name));
            if (i.dst >= 0) put("a0",i.dst);
            break;
        case Op::Jump: jump(i.data); break;
        case Op::Branch:
            if (optimize) {
                const auto src = operand(i.args[0],"t0");
                branch(i,"bne","beq",src,"zero");
                break;
            }
            get("t0",i.args[0]);
            ins("beqz t0, 1f");
            ins("j " + label(i.data));
            out << "1:\n";
            jump(i.other);
            break;
        case Op::Return:
            if (!i.args.empty()) get("a0",i.args[0]);
            jump(-1);
            break;
        default: throw std::logic_error("RV64: unsupported IR instruction");
        }
    }
    void run() {
        constants.resize(f.values);
        uses.resize(f.values);
        save_ra = !optimize;
        for (auto& b : f.blocks) {
            for (auto& i : b) {
                if (i.op == Op::Constant) constants[i.dst] = i.data;
                for (int v : i.args) ++uses[v];
                if (i.op == Op::Call) {
                    save_ra = true;
                    if (i.args.size() > 8) outgoing = std::max(outgoing,8*int64_t(i.args.size()-8));
                }
            }
        }
        frame = (slot(f.slots+allocation.spills) + 8LL*(save_ra+allocation.saved) + 15) & ~int64_t(15);
        const auto name = symbol(f.name);
        if (f.name == "main") out << ".globl main\n";
        out << ".type " << name << ", @function\n.p2align 2\n" << name << ":\n";
        if (frame) adjust(-frame);
        if (save_ra) memory("sd","ra",frame-8);
        for (int s = 0; s < allocation.saved; ++s) memory("sd","s"+std::to_string(s+1),frame-8LL*(s+1+save_ra));
        for (int b = 0; b < static_cast<int>(f.blocks.size()); ++b) {
            if (f.blocks[b].empty()) continue;
            next_block = b+1;
            while (next_block < static_cast<int>(f.blocks.size()) && f.blocks[next_block].empty()) ++next_block;
            if (next_block == static_cast<int>(f.blocks.size())) next_block = -1;
            out << label(b) << ":\n";
            const auto& code = f.blocks[b];
            for (std::size_t n = 0; n < code.size(); ++n) {
                if (n+1 < code.size() && fuse_compare(code[n],code[n+1])) ++n;
                else emit(code[n]);
            }
        }
        out << label(-1) << ":\n";
        for (int s = 0; s < allocation.saved; ++s) memory("ld","s"+std::to_string(s+1),frame-8LL*(s+1+save_ra));
        if (save_ra) memory("ld","ra",frame-8);
        if (frame) adjust(frame);
        ins("ret");
        out << ".size " << name << ", .-" << name << '\n';
    }
};
}

void emit_riscv(const ir::Module& module, std::ostream& out, bool optimize) {
    out << ".text\n";
    for (int i = 0; i < static_cast<int>(module.size()); ++i) {
        auto f = module[i];
        if (optimize) peephole_virtual(f);
        Emitter{f,out,i,optimize,allocate_values(f,optimize)}.run();
    }
    out << ".section .note.GNU-stack,\"\",@progbits\n";
}
