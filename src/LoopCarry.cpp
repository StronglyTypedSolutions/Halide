#include "LoopCarry.h"
#include "CSE.h"
#include "ExprUsesVar.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Simplify.h"
#include "Substitute.h"

#include <algorithm>

namespace Halide {
namespace Internal {

using std::pair;
using std::set;
using std::string;
using std::vector;

namespace {

/** If an integer expression varies linearly with the variables in the
 * scope, return the linear term. Otherwise return an undefined
 * Expr. */
Expr is_linear(const Expr &e, const Scope<Expr> &linear) {
    if (e.type() != Int(32)) {
        return Expr();
    }
    if (const Variable *v = e.as<Variable>()) {
        if (linear.contains(v->name)) {
            return linear.get(v->name);
        } else {
            return make_zero(v->type);
        }
    } else if (const IntImm *op = e.as<IntImm>()) {
        return make_zero(op->type);
    } else if (const Add *add = e.as<Add>()) {
        Expr la = is_linear(add->a, linear);
        Expr lb = is_linear(add->b, linear);
        if (is_const_zero(lb)) {
            return la;
        } else if (is_const_zero(la)) {
            return lb;
        } else if (la.defined() && lb.defined()) {
            return la + lb;
        } else {
            return Expr();
        }
    } else if (const Sub *sub = e.as<Sub>()) {
        Expr la = is_linear(sub->a, linear);
        Expr lb = is_linear(sub->b, linear);
        if (is_const_zero(lb)) {
            return la;
        } else if (la.defined() && lb.defined()) {
            return la - lb;
        } else {
            return Expr();
        }
    } else if (const Mul *mul = e.as<Mul>()) {
        Expr la = is_linear(mul->a, linear);
        Expr lb = is_linear(mul->b, linear);
        if (is_const_zero(la) && is_const_zero(lb)) {
            return la;
        } else if (is_const_zero(la) && lb.defined()) {
            return mul->a * lb;
        } else if (la.defined() && is_const_zero(lb)) {
            return la * mul->b;
        } else {
            return Expr();
        }
    } else if (const Ramp *r = e.as<Ramp>()) {
        Expr la = is_linear(r->base, linear);
        Expr lb = is_linear(r->stride, linear);
        if (is_const_zero(lb)) {
            return la;
        } else {
            return Expr();
        }
    } else if (const Broadcast *b = e.as<Broadcast>()) {
        return is_linear(b->value, linear);
    } else {
        return Expr();
    }
}

class FindLoads : public IRGraphVisitor {
    using IRVisitor::visit;

    // We don't use this set as the result directly, to avoid
    // non-deterministic behavior due to pointers changing value from
    // one run to the next.
    set<const Load *> found;

    void visit(const Load *op) override {
        if (found.count(op) == 0) {
            found.insert(op);
            result.push_back(op);
        }
        // Don't consider nested loads inside the index
        // IRVisitor::visit(op);
    }

public:
    vector<const Load *> result;
};

/** A helper for block_to_vector below. */
void block_to_vector(const Stmt &s, vector<Stmt> &v) {
    const Block *b = s.as<Block>();
    if (!b) {
        v.push_back(s);
    } else {
        block_to_vector(b->first, v);
        block_to_vector(b->rest, v);
    }
}

/** Unpack a block into its component Stmts. */
vector<Stmt> block_to_vector(const Stmt &s) {
    vector<Stmt> result;
    block_to_vector(s, result);
    return result;
}

Expr scratch_index(int i, Type t) {
    if (t.is_scalar()) {
        return i;
    } else {
        return Ramp::make(i * t.lanes(), 1, t.lanes());
    }
}

/** Given a scope of things that move linearly over time, come up with
 * the next time step's version of some arbitrary Expr (which may be a
 * nasty graph). Variables that move non-linearly through time are
 * undefined Exprs in the scope. */
class StepForwards : public IRGraphMutator {
    const Scope<Expr> &linear;

    using IRGraphMutator::visit;

    Expr visit(const Variable *op) override {
        if (linear.contains(op->name)) {
            Expr step = linear.get(op->name);
            if (!step.defined()) {
                // It's non-linear
                success = false;
                return op;
            } else if (is_const_zero(step)) {
                // It's a known inner constant
                return op;
            } else {
                // It's linear
                return Expr(op) + step;
            }
        } else {
            // It's some external constant
            return op;
        }
    }

public:
    bool success = true;
    StepForwards(const Scope<Expr> &s)
        : linear(s) {
    }
};

Expr step_forwards(Expr e, const Scope<Expr> &linear) {
    StepForwards step(linear);
    e = step.mutate(e);
    if (!step.success) {
        return Expr();
    } else {
        // We need to simplify it to reduce it to a canonical form,
        // but it's a full graph, so we'll need to CSE it first.
        e = common_subexpression_elimination(e);
        e = simplify(e);
        e = substitute_in_all_lets(e);
        return e;
    }
}

/** Carry loads over a single For loop body. */
class LoopCarryOverLoop : public IRMutator {
    // Track vars that step linearly with loop iterations
    Scope<Expr> linear;
    vector<pair<string, Expr>> containing_lets;

    // Productions we're in a consume node for. They're fixed and safe
    // to lift out.
    const Scope<> &in_consume;

    int max_carried_values;

    using IRMutator::visit;

    Stmt visit(const LetStmt *op) override {
        // Track containing LetStmts and their linearity w.r.t. the
        // loop variable.
        Expr value = mutate(op->value);
        Expr step = is_linear(value, linear);
        ScopedBinding<Expr> bind(linear, op->name, step);

        containing_lets.emplace_back(op->name, value);

        Stmt stmt;
        Stmt body = mutate(op->body);
        if (value.same_as(op->value) &&
            body.same_as(op->body)) {
            stmt = op;
        } else {
            stmt = LetStmt::make(op->name, value, body);
        }

        containing_lets.pop_back();
        return stmt;
    }

    Stmt visit(const Store *op) override {
        return lift_carried_values_out_of_stmt(op);
    }

    Stmt visit(const Block *op) override {
        vector<Stmt> v = block_to_vector(op);

        vector<Stmt> stores;
        vector<Stmt> result;
        for (Stmt &stmt : v) {
            if (stmt.as<Store>()) {
                stores.push_back(stmt);
            } else {
                if (!stores.empty()) {
                    result.push_back(lift_carried_values_out_of_stmt(Block::make(stores)));
                    stores.clear();
                }
                result.push_back(mutate(stmt));
            }
        }
        if (!stores.empty()) {
            result.push_back(lift_carried_values_out_of_stmt(Block::make(stores)));
        }

        return Block::make(result);
    }

    Stmt lift_carried_values_out_of_stmt(const Stmt &orig_stmt) {
        debug(4) << "About to lift carried values out of stmt: " << orig_stmt << "\n";

        // The stmts, as graphs (lets subtituted in). We must only use
        // graph-aware methods to touch these, lest we incur
        // exponential runtime.
        Stmt graph_stmt = substitute_in_all_lets(orig_stmt);

        // Find all the loads in these stmts.
        FindLoads find_loads;
        graph_stmt.accept(&find_loads);

        debug(4) << "Found " << find_loads.result.size() << " loads\n";

        // Group equal loads
        vector<vector<const Load *>> loads;
        for (const Load *load : find_loads.result) {
            // Check if it's safe to lift out.
            bool safe = (load->image.defined() ||
                         load->param.defined() ||
                         in_consume.contains(load->name));
            if (!safe) {
                continue;
            }

            bool represented = false;
            for (vector<const Load *> &v : loads) {
                if (graph_equal(Expr(load), Expr(v[0]))) {
                    v.push_back(load);
                    represented = true;
                }
            }
            if (!represented) {
                loads.push_back({load});
            }
        }

        // For each load, move the load index forwards by one loop iteration
        vector<Expr> indices, next_indices, predicates, next_predicates;
        // CSE-d versions of the above, so can_prove can be safely used on them.
        vector<Expr> indices_csed, next_indices_csed, predicates_csed, next_predicates_csed;
        for (const vector<const Load *> &v : loads) {
            indices.push_back(v[0]->index);
            next_indices.push_back(step_forwards(v[0]->index, linear));
            predicates.push_back(v[0]->predicate);
            next_predicates.push_back(step_forwards(v[0]->predicate, linear));

            if (indices.back().defined()) {
                indices_csed.push_back(common_subexpression_elimination(indices.back()));
            } else {
                indices_csed.emplace_back();
            }
            if (next_indices.back().defined()) {
                next_indices_csed.push_back(common_subexpression_elimination(next_indices.back()));
            } else {
                next_indices_csed.emplace_back();
            }
            if (predicates.back().defined()) {
                predicates_csed.push_back(common_subexpression_elimination(predicates.back()));
            } else {
                predicates_csed.emplace_back();
            }
            if (next_predicates.back().defined()) {
                next_predicates_csed.push_back(common_subexpression_elimination(next_predicates.back()));
            } else {
                next_predicates_csed.emplace_back();
            }
        }

        // Find loads done on this loop iteration that will be
        // reusable as some other Expr on the next loop iteration.
        vector<vector<int>> chains;
        for (int i = 0; i < (int)indices.size(); i++) {
            for (int j = 0; j < (int)indices.size(); j++) {
                // Don't catch loop invariants here.
                if (i == j) {
                    continue;
                }
                // can_prove is stronger than graph_equal, because it doesn't require index expressions to be
                // exactly the same, but evaluate to the same value. We keep the graph_equal check, because
                // it's faster and should be executed before the more expensive check.
                if (loads[i][0]->name == loads[j][0]->name &&
                    next_indices[j].defined() &&
                    (graph_equal(indices[i], next_indices[j]) ||
                     ((indices[i].type() == next_indices[j].type()) && can_prove(indices_csed[i] == next_indices_csed[j]))) &&
                    next_predicates[j].defined() &&
                    (graph_equal(predicates[i], next_predicates[j]) ||
                     ((predicates[i].type() == next_predicates[j].type()) && can_prove(predicates_csed[i] == next_predicates_csed[j])))) {
                    chains.push_back({j, i});
                    debug(3) << "Found carried value:\n"
                             << i << ":  -> " << Expr(loads[i][0]) << "\n"
                             << j << ":  -> " << Expr(loads[j][0]) << "\n";
                }
            }
        }

        if (chains.empty()) {
            return orig_stmt;
        }

        // Agglomerate chains of carries
        bool done = false;
        while (!done) {
            done = true;
            for (size_t i = 0; i < chains.size(); i++) {
                if (chains[i].empty()) {
                    continue;
                }
                for (size_t j = 0; j < chains.size(); j++) {
                    if (chains[j].empty()) {
                        continue;
                    }
                    if (chains[i].back() == chains[j].front()) {
                        chains[i].insert(chains[i].end(), chains[j].begin() + 1, chains[j].end());
                        chains[j].clear();
                        done = false;
                    }
                }
            }

            for (size_t i = 0; i < chains.size(); i++) {
                while (i < chains.size() && chains[i].empty()) {
                    chains[i].swap(chains.back());
                    chains.pop_back();
                }
            }
        }

        // Sort the carry chains by decreasing order of size. The
        // longest ones get the most reuse of each value.
        //
        // Use of stable_sort is just so that IR generated by different C++ compilers
        // is identical; it doesn't appear to make any meaningful difference
        // in code output, but makes debugging IR output easier to deal with.
        std::stable_sort(chains.begin(), chains.end(),
                         [&](const vector<int> &c1, const vector<int> &c2) { return c1.size() > c2.size(); });

        for (const vector<int> &c : chains) {
            debug(3) << "Found chain of carried values:\n";
            for (int i : c) {
                debug(3) << i << ":  <- " << indices[i] << "\n";
            }
        }

        // Only keep the top N carried values. Otherwise we'll just
        // spray stack spills everywhere. This is ugly, because we're
        // relying on a heuristic.
        vector<vector<int>> trimmed;
        size_t sz = 0;
        for (const vector<int> &c : chains) {
            if (sz + c.size() > (size_t)max_carried_values) {
                if (sz < (size_t)max_carried_values - 1) {
                    // Take a partial chain
                    trimmed.emplace_back(c.begin(), c.begin() + max_carried_values - sz);
                }
                break;
            }
            trimmed.push_back(c);
            sz += c.size();
        }
        chains.swap(trimmed);

        // We now have chains of the form:
        // f[x] <- f[x+1] <- ... <- f[x+N-1]

        // There are N values in that chain. N-1 of these can be
        // reused from the previous loop iteration. We make a scratch
        // buffer of size N. First we load the leading edge (the end
        // of the chain) into in the last position. Then we run the
        // original stmt with the loads replaced with loads from the
        // scratch buffer, then we move the last N-1 values in the
        // scratch buffer down one, so they're in the right place for
        // the next loop iteration. If it's the first loop iteration,
        // we need to populate the entire scratch buffer.

        vector<Stmt> not_first_iteration_scratch_stores;
        vector<Stmt> scratch_shuffles;
        Stmt core = graph_stmt;

        for (const vector<int> &c : chains) {
            string scratch = unique_name('c');
            vector<Expr> initial_scratch_values;

            for (size_t i = 0; i < c.size(); i++) {
                const Load *orig_load = loads[c[i]][0];
                Expr scratch_idx = scratch_index(i, orig_load->type);
                // Don't worry about alignment - the load is at a constant address.
                Expr load_from_scratch = Load::make(orig_load->type, scratch, scratch_idx,
                                                    Buffer<>(), Parameter(), const_true(orig_load->type.lanes()), ModulusRemainder());
                for (const Load *l : loads[c[i]]) {
                    core = graph_substitute(l, load_from_scratch, core);
                }

                if (i == c.size() - 1) {
                    Stmt store_to_scratch = Store::make(scratch, orig_load, scratch_idx,
                                                        Parameter(), const_true(orig_load->type.lanes()), ModulusRemainder());
                    not_first_iteration_scratch_stores.push_back(store_to_scratch);
                } else {
                    initial_scratch_values.emplace_back(orig_load);
                }
                if (i > 0) {
                    Stmt shuffle = Store::make(scratch, load_from_scratch,
                                               scratch_index(i - 1, orig_load->type),
                                               Parameter(), const_true(orig_load->type.lanes()), ModulusRemainder());
                    scratch_shuffles.push_back(shuffle);
                }
            }

            // Do joint CSE on the initial scratch values instead of
            // cse'ing each independently. They'll shared common
            // values and they originated from the same Expr.
            vector<pair<string, Expr>> initial_lets;
            // Group them into a single expression with a call node
            Expr call = Call::make(Int(32), unique_name('b'), initial_scratch_values, Call::PureIntrinsic);
            // Run CSE
            call = simplify(common_subexpression_elimination(call));
            // Peel off lets
            while (const Let *l = call.as<Let>()) {
                initial_lets.emplace_back(l->name, l->value);
                call = l->body;
            }
            internal_assert(call.as<Call>());
            initial_scratch_values = call.as<Call>()->args;

            // Create the initial stores to scratch
            vector<Stmt> initial_scratch_stores;
            for (size_t i = 0; i < c.size() - 1; i++) {
                Expr scratch_idx = scratch_index(i, initial_scratch_values[i].type());
                Stmt store_to_scratch = Store::make(scratch, initial_scratch_values[i],
                                                    scratch_idx, Parameter(),
                                                    const_true(scratch_idx.type().lanes()),
                                                    ModulusRemainder());
                initial_scratch_stores.push_back(store_to_scratch);
            }

            Stmt initial_stores = Block::make(initial_scratch_stores);

            // Wrap them in the appropriate lets
            for (size_t i = initial_lets.size(); i > 0; i--) {
                auto l = initial_lets[i - 1];
                initial_stores = LetStmt::make(l.first, l.second, initial_stores);
            }
            // We may be lifting the initial stores out of let stmts,
            // so rewrap them in the necessary ones.
            for (size_t i = containing_lets.size(); i > 0; i--) {
                auto l = containing_lets[i - 1];
                if (stmt_uses_var(initial_stores, l.first)) {
                    initial_stores = LetStmt::make(l.first, l.second, initial_stores);
                }
            }

            allocs.push_back({scratch,
                              loads[c.front()][0]->type.element_of(),
                              (int)c.size() * loads[c.front()][0]->type.lanes(),
                              initial_stores});
        }

        Stmt s = Block::make(not_first_iteration_scratch_stores);
        s = Block::make(s, core);
        s = Block::make(s, Block::make(scratch_shuffles));
        s = common_subexpression_elimination(s);
        return s;
    }

    Stmt visit(const For *op) override {
        // Don't lift loads out of code that might not run. Besides,
        // stashing things in registers while we run an inner loop
        // probably isn't a good use of registers.
        return op;
    }

    Stmt visit(const IfThenElse *op) override {
        // Don't lift loads out of code that might not run.
        return op;
    }

public:
    LoopCarryOverLoop(const string &var, const Scope<> &s, int max_carried_values)
        : in_consume(s), max_carried_values(max_carried_values) {
        linear.push(var, 1);
    }

    struct ScratchAllocation {
        string name;
        Type type;
        int size;
        Stmt initial_stores;
    };

    vector<ScratchAllocation> allocs;
};

class LoopCarry : public IRMutator {
    using IRMutator::visit;

    int max_carried_values;
    Scope<> in_consume;

    Stmt visit(const ProducerConsumer *op) override {
        if (op->is_producer) {
            return IRMutator::visit(op);
        } else {
            ScopedBinding<> bind(in_consume, op->name);
            Stmt body = mutate(op->body);
            return ProducerConsumer::make(op->name, op->is_producer, body);
        }
    }

    Stmt visit(const For *op) override {
        if (op->for_type == ForType::Serial && !is_const_one(op->extent)) {
            Stmt stmt;
            Stmt body = mutate(op->body);
            LoopCarryOverLoop carry(op->name, in_consume, max_carried_values);
            body = carry.mutate(body);
            if (body.same_as(op->body)) {
                stmt = op;
            } else {
                stmt = For::make(op->name, op->min, op->extent, op->for_type, op->partition_policy, op->device_api, body);
            }

            // Inject the scratch buffer allocations.
            for (const auto &alloc : carry.allocs) {
                stmt = Block::make(substitute(op->name, op->min, alloc.initial_stores), stmt);
                stmt = Allocate::make(alloc.name, alloc.type, MemoryType::Stack, {alloc.size}, const_true(), stmt);
            }
            if (!carry.allocs.empty()) {
                stmt = IfThenElse::make(op->extent > 0, stmt);
            }
            return stmt;
        } else {
            return IRMutator::visit(op);
        }
    }

public:
    LoopCarry(int max_carried_values)
        : max_carried_values(max_carried_values) {
    }
};

}  // namespace

Stmt loop_carry(Stmt s, int max_carried_values) {
    s = LoopCarry(max_carried_values).mutate(s);
    return s;
}

}  // namespace Internal
}  // namespace Halide
