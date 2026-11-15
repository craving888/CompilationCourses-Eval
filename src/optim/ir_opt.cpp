#include "optim/ir_opt.hpp"
#include "ir/int_eval.hpp"
#include <algorithm>
#include <map>
#include <tuple>

namespace {
using namespace ir;
std::vector<int> successors(const Block& b) {
    if (b.empty()) return {};
    const auto& i = b.back();
    if (i.op == Op::Jump) return {i.data};
    if (i.op == Op::Branch) return {i.data,i.other};
    return {};
}
bool prune(ir::Function& f) {
    std::vector<bool> seen(f.blocks.size());
    std::vector<int> pending{0};
    while (!pending.empty()) {
        int b = pending.back(); pending.pop_back();
        if (seen[b]) continue;
        seen[b] = true;
        for (int s : successors(f.blocks[b])) pending.push_back(s);
    }
    bool changed = false;
    for (std::size_t b = 0; b < f.blocks.size(); ++b) {
        if (!seen[b] && !f.blocks[b].empty()) { f.blocks[b].clear(); changed = true; }
    }
    return changed;
}

struct Fact {
    enum Kind { Bottom, Known, Varying } kind = Bottom;
    int32_t value = 0;
    bool operator==(const Fact& f) const { return kind == f.kind && value == f.value; }
};
Fact join(Fact a,Fact b) {
    if (a.kind == Fact::Bottom) return b;
    if (b.kind == Fact::Bottom || a == b) return a;
    return {Fact::Varying};
}
using State = std::vector<Fact>;

bool propagate(ir::Function& f) {
    std::vector<Constant> values(f.values);
    for (auto& block : f.blocks)
        for (auto& i : block)
            if (i.op == Op::Constant) values[i.dst] = i.data;
    auto get = [&](int v) {
        return values[v] ? Fact{Fact::Known,*values[v]} : Fact{Fact::Varying};
    };
    auto effective = [&](const Inst& i, const State& s)->Constant {
        if (i.op == Op::Load && s[i.data].kind == Fact::Known) return s[i.data].value;
        if (i.op == Op::Unary && values[i.args[0]]) {
            int32_t a = *values[i.args[0]];
            if (i.name == "+") return a;
            if (i.name == "!") return !a;
            if (i.name == "-") return integer_result(-int64_t(a));
        }
        if (i.op == Op::Binary && values[i.args[0]] && values[i.args[1]])
            return eval_binary(i.name,*values[i.args[0]],*values[i.args[1]]);
        return {};
    };
    std::vector<State> in(f.blocks.size(), State(f.slots));
    std::fill(in[0].begin(),in[0].end(),Fact{Fact::Varying});
    bool update;
    do {
        update = false;
        for (std::size_t b = 0; b < f.blocks.size(); ++b) {
            State state = in[b];
            for (auto& i : f.blocks[b]) if (i.op == Op::Store) state[i.data] = get(i.args[0]);
            for (auto s : successors(f.blocks[b])) {
                for (int v = 0; v < f.slots; ++v) {
                    auto merged = join(in[s][v],state[v]);
                    if (!(merged == in[s][v])) { in[s][v] = merged; update = true; }
                }
            }
        }
    } while (update);
    bool changed = false;
    for (std::size_t b = 0; b < f.blocks.size(); ++b) {
        auto state = std::move(in[b]);
        for (auto& i : f.blocks[b]) {
            if (i.op == Op::Store) state[i.data] = get(i.args[0]);
            if (i.op == Op::Branch && values[i.args[0]]) {
                i = {Op::Jump,-1,{}, {},*values[i.args[0]] ? i.data : i.other};
                changed = true;
            } else if (auto result = effective(i,state)) {
                i = {Op::Constant,i.dst,{}, {},*result};
                changed = true;
            }
        }
    }
    return changed;
}

bool simplify_values(ir::Function& f) {
    std::vector<int> replace(f.values,-1);
    std::vector<Constant> constants(f.values);
    for (auto& b : f.blocks) for (auto& i : b)
        if (i.op == Op::Constant) constants[i.dst] = i.data;
    auto canonical = [&](int v) { while (replace[v] >= 0) v = replace[v]; return v; };
    bool changed = false;
    for (auto& b : f.blocks) {
        std::map<std::tuple<Op,std::string,int,int>,int> available;
        std::vector<int> slot_value(f.slots,-1);
        for (auto& i : b) {
            for (auto& v : i.args) v = canonical(v);
            if (i.op == Op::Store) slot_value[i.data] = i.args[0];
            if (i.op == Op::Load) {
                auto& value = slot_value[i.data];
                if (value < 0) value = i.dst;
                else { replace[i.dst] = value; changed = true; }
            }
            if (i.op != Op::Unary && i.op != Op::Binary) continue;
            const int a = i.args[0], c = i.args.size() == 2 ? i.args[1] : -1;
            int equivalent = -1;
            if (i.op == Op::Unary && i.name == "+") equivalent = a;
            if (i.op == Op::Binary) {
                if ((i.name == "+" || i.name == "-") && constants[c] == 0) equivalent = a;
                if (i.name == "+" && constants[a] == 0) equivalent = c;
                if ((i.name == "*" || i.name == "/") && constants[c] == 1) equivalent = a;
                if (i.name == "*" && constants[a] == 1) equivalent = c;
            }
            auto key = std::make_tuple(i.op,i.name,a,c);
            if (equivalent < 0) {
                auto [entry, inserted] = available.emplace(std::move(key),i.dst);
                if (!inserted) equivalent = entry->second;
            }
            if (equivalent >= 0) { replace[i.dst] = canonical(equivalent); changed = true; }
        }
    }
    if (changed)
        for (auto& b : f.blocks) for (auto& i : b) for (auto& v : i.args) v = canonical(v);
    return changed;
}

bool eliminate(ir::Function& f) {
    using Live = std::vector<bool>;
    std::vector<Live> live_in(f.blocks.size(), Live(f.slots));
    auto live_out = [&](int b) {
        Live live(f.slots);
        for (int s : successors(f.blocks[b]))
            for (int v = 0; v < f.slots; ++v) live[v] = live[v] || live_in[s][v];
        return live;
    };
    bool changed;
    do {
        changed = false;
        for (int b = static_cast<int>(f.blocks.size())-1; b >= 0; --b) {
            auto live = live_out(b);
            for (auto i = f.blocks[b].rbegin(); i != f.blocks[b].rend(); ++i) {
                if (i->op == Op::Store) live[i->data] = false;
                if (i->op == Op::Load) live[i->data] = true;
            }
            if (live != live_in[b]) { live_in[b] = std::move(live); changed = true; }
        }
    } while (changed);
    for (int b = 0; b < static_cast<int>(f.blocks.size()); ++b) {
        auto live = live_out(b);
        auto& block = f.blocks[b];
        int end = static_cast<int>(block.size());
        for (int x = end-1; x >= 0; --x) {
            auto& i = block[x];
            if (i.op == Op::Store) {
                if (!live[i.data]) { changed = true; continue; }
                live[i.data] = false;
            }
            if (i.op == Op::Load) live[i.data] = true;
            --end;
            if (end != x) block[end] = std::move(i);
        }
        block.erase(block.begin(),block.begin()+end);
    }
    std::vector<int> uses(f.values);
    for (auto& b : f.blocks) for (auto& i : b) for (int v : i.args) ++uses[v];
    for (auto& b : f.blocks) {
        const auto first_dead = std::remove_if(b.begin(),b.end(),[&](const Inst& i) {
            return i.dst >= 0 && !uses[i.dst] && i.op != Op::Call;
        });
        if (first_dead != b.end()) { b.erase(first_dead,b.end()); changed = true; }
    }
    return changed;
}

const Block* inline_body(const ir::Function& f) {
    const auto& entry = f.blocks.front();
    if (entry.empty() || entry.size() > 48 || entry.back().op != Op::Return) return nullptr;
    return &entry;
}

void inline_calls(ir::Function& caller, const ir::Module& module, std::size_t current) {
    for (auto& block : caller.blocks) {
        Block code;
        for (const auto& inst : block) {
            if (inst.op != Op::Call) { code.push_back(inst); continue; }
            const auto previous = module.begin()+current;
            const auto callee = std::find_if(module.begin(),previous,[&](const auto& f) { return f.name == inst.name; });
            const auto* body = callee != previous ? inline_body(*callee) : nullptr;
            if (!body) { code.push_back(inst); continue; }

            std::vector<int> values(callee->values,-1);
            const int slot_base = caller.slots;
            caller.slots += callee->slots;
            for (auto copy : *body) {
                if (copy.op == Op::Param) { values[copy.dst] = inst.args[copy.data]; continue; }
                for (auto& arg : copy.args) arg = values[arg];
                if (copy.op == Op::Return) {
                    if (inst.dst >= 0) code.push_back({Op::Unary,inst.dst,copy.args,"+"});
                    break;
                }
                if (copy.dst >= 0) copy.dst = values[copy.dst] = caller.values++;
                if (copy.op == Op::Load || copy.op == Op::Store) copy.data += slot_base;
                code.push_back(std::move(copy));
            }
        }
        block = std::move(code);
    }
}

bool tail_recursion(ir::Function& f) {
    const auto tail_call = [&](const Block& b) {
        if (b.size() < 2 || b.back().op != Op::Return || b.back().args.empty()) return false;
        const auto& call = b[b.size()-2];
        return call.op == Op::Call && call.name == f.name && call.dst == b.back().args[0];
    };
    if (std::none_of(f.blocks.begin(),f.blocks.end(),tail_call)) return false;
    auto& entry = f.blocks.front();
    // The lowering prologue writes each incoming parameter to its own slot.
    std::vector<int> slots;
    std::size_t first = 0;
    for (int i = 0; i < f.arity; ++i) {
        if (first+1 >= entry.size() || entry[first].op != Op::Param || entry[first].data != i ||
            entry[first+1].op != Op::Store || entry[first+1].args[0] != entry[first].dst) return false;
        slots.push_back(entry[first+1].data);
        first += 2;
    }
    const int loop = static_cast<int>(f.blocks.size());
    Block body(entry.begin()+first,entry.end());
    entry.resize(first);
    entry.push_back({Op::Jump,-1,{}, {},loop});
    f.blocks.push_back(std::move(body));
    for (auto& block : f.blocks) {
        if (!tail_call(block)) continue;
        auto args = std::move(block[block.size()-2].args);
        block.resize(block.size()-2);
        for (int i = 0; i < f.arity; ++i)
            block.push_back({Op::Store,-1,{args[i]},{},slots[i]});
        block.push_back({Op::Jump,-1,{}, {},loop});
    }
    return true;
}

bool optimize_simple_loops(ir::Function& f) {
    std::vector<std::vector<int>> predecessors(f.blocks.size());
    for (int b=0; b<static_cast<int>(f.blocks.size()); ++b)
        for (int s : successors(f.blocks[b])) predecessors[s].push_back(b);
    std::vector<Constant> constants(f.values);
    std::vector<int> definitions(f.values,-1);
    for (int b=0; b<static_cast<int>(f.blocks.size()); ++b)
        for (const auto& i : f.blocks[b]) {
            if (i.dst >= 0) definitions[i.dst] = b;
            if (i.op == Op::Constant) constants[i.dst] = i.data;
        }

    for (int head=1; head<static_cast<int>(f.blocks.size()); ++head) {
        auto& header = f.blocks[head];
        if (header.empty() || header.back().op != Op::Branch || predecessors[head].size() != 2) continue;
        const int body_id = header.back().data;
        if (body_id == head || predecessors[body_id] != std::vector<int>{head}) continue;
        auto& body = f.blocks[body_id];
        if (successors(body) != std::vector<int>{head}) continue;
        const int before = predecessors[head][0] == body_id ? predecessors[head][1] : predecessors[head][0];
        auto& preheader = f.blocks[before];
        if (successors(preheader) != std::vector<int>{head}) continue;

        // The only effect of a finite, otherwise empty counted loop is its
        // final private counter value. Remove it only when that value is dead.
        if (header.size() == 4 && body.size() == 5 &&
            header[0].op == Op::Load && header[1].op == Op::Constant &&
            header[2].op == Op::Binary && header[2].name == "<" &&
            header[2].args == std::vector<int>{header[0].dst,header[1].dst} &&
            header[3].args[0] == header[2].dst &&
            body[0].op == Op::Load && body[0].data == header[0].data &&
            body[1].op == Op::Constant && body[1].data == 1 &&
            body[2].op == Op::Binary && body[2].name == "+" &&
            body[2].args == std::vector<int>{body[0].dst,body[1].dst} &&
            body[3].op == Op::Store && body[3].data == header[0].data &&
            body[3].args[0] == body[2].dst) {
            const int counter = header[0].data;
            const auto init = std::find_if(preheader.rbegin(),preheader.rend(),[&](const Inst& i) {
                return i.op == Op::Store && i.data == counter;
            });
            bool live = false;
            for (int b=0; b<static_cast<int>(f.blocks.size()) && !live; ++b) {
                if (b == head || b == body_id) continue;
                for (const auto& i : f.blocks[b]) {
                    if (i.op == Op::Load && i.data == counter) live = true;
                    for (int v : i.args) if (definitions[v] == head || definitions[v] == body_id) live = true;
                }
            }
            if (init != preheader.rend() && constants[init->args[0]] && !live) {
                preheader.back().data = header.back().other;
                return true;
            }
        }

        std::vector<bool> written(f.slots);
        for (const auto* block : {&header,&body})
            for (const auto& i : *block) if (i.op == Op::Store) written[i.data] = true;
        std::vector<int> candidates(f.values,-1);
        for (std::size_t pos=0; pos<body.size(); ++pos) {
            const auto& i = body[pos];
            if (i.dst < 0) continue;
            bool pure = i.op == Op::Constant || i.op == Op::Unary ||
                        (i.op == Op::Load && !written[i.data]) || i.op == Op::Binary;
            if (i.op == Op::Binary && (i.name == "/" || i.name == "%")) {
                const auto divisor = constants[i.args[1]];
                pure = divisor && *divisor != 0 && *divisor != -1;
            }
            if (pure && std::all_of(i.args.begin(),i.args.end(),[&](int v) {
                return candidates[v] >= 0 || (definitions[v] != head && definitions[v] != body_id);
            })) candidates[i.dst] = static_cast<int>(pos);
        }
        std::vector<bool> move(body.size());
        for (std::size_t pos=0; pos<body.size(); ++pos) {
            const auto& i = body[pos];
            if (i.op == Op::Binary && candidates[i.dst] >= 0 && (i.name == "*" || i.name == "/" || i.name == "%"))
                move[pos] = true;
        }
        if (std::none_of(move.begin(),move.end(),[](bool m) {return m;})) continue;
        for (int pos=static_cast<int>(body.size())-1; pos>=0; --pos)
            if (move[pos])
                for (int v : body[pos].args)
                    if (candidates[v] >= 0) move[candidates[v]] = true;
        Block kept;
        for (std::size_t pos=0; pos<body.size(); ++pos) {
            if (move[pos]) preheader.insert(preheader.end()-1,std::move(body[pos]));
            else kept.push_back(std::move(body[pos]));
        }
        body = std::move(kept);
        return true;
    }
    return false;
}
}

void optimize_ir(ir::Module& m) {
    for (std::size_t index=0; index<m.size(); ++index) {
        auto& f = m[index];
        tail_recursion(f);
        inline_calls(f,m,index);
        bool changed;
        do {
            changed = prune(f);
            changed |= propagate(f);
            changed |= simplify_values(f);
            changed |= eliminate(f);
            if (!changed) changed = optimize_simple_loops(f);
        } while (changed);
    }
}
