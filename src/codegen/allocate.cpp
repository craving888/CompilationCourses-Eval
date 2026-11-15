#include "codegen/allocate.hpp"
#include <algorithm>
#include <array>
#include <numeric>

void peephole_virtual(ir::Function& f) {
    // Consecutive reads of the same private slot see the same value until a write.
    std::vector<int> alias(f.values,-1);
    for (auto& block : f.blocks) {
        std::vector<int> available(f.slots,-1);
        block.erase(std::remove_if(block.begin(),block.end(),[&](const ir::Inst& i) {
            if (i.op == ir::Op::Store) available[i.data] = -1;
            if (i.op != ir::Op::Load) return false;
            int& old = available[i.data];
            if (old < 0) { old = i.dst; return false; }
            alias[i.dst] = old;
            return true;
        }),block.end());
    }
    for (auto& block : f.blocks)
        for (auto& i : block)
            for (auto& a : i.args)
                if (alias[a] >= 0) a = alias[a];
}

Allocation allocate_values(const ir::Function& f, bool optimize) {
    Allocation a{std::vector<int>(f.values,-1),std::vector<int>(f.values,-1),
                 std::vector<int>(f.slots,-1)};
    if (!optimize) {
        std::iota(a.spill.begin(),a.spill.end(),0);
        a.spills = f.values;
        return a;
    }
    struct Range { int value, start = -1, finish = -1, block = -1; bool external = false; };
    std::vector<Range> ranges;
    for (int v = 0; v < f.values; ++v) ranges.push_back({v});
    std::vector<int> calls, heat(f.slots);
    int pos = 0;
    for (int b = 0; b < static_cast<int>(f.blocks.size()); ++b)
        for (const auto& i : f.blocks[b]) {
            ++pos;
            if (i.op == ir::Op::Call) calls.push_back(pos);
            if (i.op == ir::Op::Load || i.op == ir::Op::Store) ++heat[i.data];
            if (i.dst >= 0) {
                auto& r = ranges[i.dst];
                if (i.op != ir::Op::Constant) r.start = pos;
                r.finish = std::max(r.finish,pos);
                if (r.block >= 0 && r.block != b) r.external = true;
                r.block = b;
            }
            for (int v : i.args) {
                auto& r = ranges[v];
                r.finish = std::max(r.finish,pos);
                if (r.block >= 0 && r.block != b) r.external = true;
                if (r.block < 0) r.block = b;
            }
        }
    std::vector<int> locals(f.slots);
    std::iota(locals.begin(),locals.end(),0);
    std::stable_sort(locals.begin(),locals.end(),[&](int x,int y){return heat[x]>heat[y];});
    for (int s : locals) {
        if (a.saved == 11 || heat[s] < 3) break;
        a.locals[s] = a.saved++;
    }
    std::sort(ranges.begin(),ranges.end(),[](auto x,auto y){return x.start<y.start;});
    std::array<int,4> active{{-1,-1,-1,-1}};
    std::vector<int> finish(f.values);
    for (auto r : ranges) {
        if (r.start < 0) continue;
        finish[r.value] = r.finish;
        auto call = std::upper_bound(calls.begin(),calls.end(),r.start);
        if (r.external || (call != calls.end() && *call < r.finish)) {
            a.spill[r.value] = a.spills++;
            continue;
        }
        auto slot = std::find_if(active.begin(),active.end(),[&](int v) { return v < 0 || finish[v] <= r.start; });
        if (slot == active.end()) {
            slot = std::max_element(active.begin(),active.end(),[&](int x,int y) { return finish[x]<finish[y]; });
            if (finish[*slot] <= r.finish) { a.spill[r.value] = a.spills++; continue; }
            a.regs[*slot] = -1;
            a.spill[*slot] = a.spills++;
        }
        *slot = r.value;
        a.regs[r.value] = static_cast<int>(slot-active.begin());
    }
    return a;
}
