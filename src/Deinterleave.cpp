#include "Deinterleave.h"
#include "IRMutator.h"
#include "Simplify.h"
#include "IROperator.h"
#include "IREquality.h"
#include "IRPrinter.h"
#include "ModulusRemainder.h"
#include "Debug.h"
#include "Scope.h"

namespace Halide {
namespace Internal {

using std::pair;
using std::make_pair;

class StoreCollector : public IRMutator {
  public:
    const std::string store_name;
    const int store_stride;
    std::vector<Store>& stores;

    StoreCollector(const std::string& name, int stride, std::vector<Store>& ss) :
            store_name(name), store_stride(stride), stores(ss) {}
  private:

    using IRMutator::visit;

    void visit(const Store *op) {
        if (op->name == store_name) {
            const Ramp *r = op->index.as<Ramp>();

            if (r && is_const(r->stride) &&
                *as_const_int(r->stride) == store_stride) {
                stores.push_back(*op);
                stmt = Stmt();
                return;
            }
        }

        stmt = op;
    }

    void visit(const Block *op) {
        const Store *s = op->first.as<Store>();
        if (s && s->name == store_name) {
            const Ramp *r = s->index.as<Ramp>();

            if (r && is_const(r->stride) &&
                *as_const_int(r->stride) == store_stride) {
                stores.push_back(*s);
                stmt = mutate(op->rest);
                return;
            }
        }

        stmt = Block::make(op->first, mutate(op->rest));
    }
};

Stmt collect_strided_stores(Stmt stmt, const std::string& name, int stride, std::vector<Store>& stores) {
    StoreCollector collect(name, stride, stores);
    return collect.mutate(stmt);
}


class Deinterleaver : public IRMutator {
  public:
    int starting_lane;
    int new_width;
    int lane_stride;

    // lets for which we have even and odd lane specializations
    const Scope<int> &external_lets;

    Deinterleaver(const Scope<int> &lets) : external_lets(lets) {}
  private:
    Scope<Expr> internal;

    using IRMutator::visit;

    void visit(const Broadcast *op) {
        if (new_width == 1) {
            expr = op->value;
        } else {
            expr = Broadcast::make(op->value, new_width);
        }
    }

    void visit(const Load *op) {
        if (op->type.is_scalar()) {
            expr = op;
        } else {
            Type t = op->type;
            t.width = new_width;
            expr = Load::make(t, op->name, mutate(op->index), op->image, op->param);
        }
    }

    void visit(const Ramp *op) {
        expr = op->base + starting_lane * op->stride;
        if (new_width > 1) {
            expr = Ramp::make(expr, op->stride * lane_stride, new_width);
        }
    }

    void visit(const Variable *op) {
        if (op->type.is_scalar()) {
            expr = op;
        } else {

            Type t = op->type;
            t.width = new_width;
            if (internal.contains(op->name)) {
                expr = internal.get(op->name);
            } else if (external_lets.contains(op->name) &&
                       starting_lane == 0 &&
                       lane_stride == 2) {
                expr = Variable::make(t, op->name + ".even_lanes", op->image, op->param, op->reduction_domain);
            } else if (external_lets.contains(op->name) &&
                       starting_lane == 1 &&
                       lane_stride == 2) {
                expr = Variable::make(t, op->name + ".odd_lanes", op->image, op->param, op->reduction_domain);
            } else if (external_lets.contains(op->name) &&
                       starting_lane == 0 &&
                       lane_stride == 3) {
                expr = Variable::make(t, op->name + ".lanes_0_of_3", op->image, op->param, op->reduction_domain);
            } else if (external_lets.contains(op->name) &&
                       starting_lane == 1 &&
                       lane_stride == 3) {
                expr = Variable::make(t, op->name + ".lanes_1_of_3", op->image, op->param, op->reduction_domain);
            } else if (external_lets.contains(op->name) &&
                       starting_lane == 2 &&
                       lane_stride == 3) {
                expr = Variable::make(t, op->name + ".lanes_2_of_3", op->image, op->param, op->reduction_domain);
            } else {
                // Uh-oh, we don't know how to deinterleave this vector expression
                // Make llvm do it
                std::vector<Expr> args;
                args.push_back(op);
                for (int i = 0; i < new_width; i++) {
                    args.push_back(starting_lane + lane_stride * i);
                }
                expr = Call::make(t, Call::shuffle_vector, args, Call::Intrinsic);
            }
        }
    }

    void visit(const Cast *op) {
        if (op->type.is_scalar()) {
            expr = op;
        } else {
            Type t = op->type;
            t.width = new_width;
            expr = Cast::make(t, mutate(op->value));
        }
    }

    void visit(const Call *op) {
        // Don't mutate scalars
        if (op->type.is_scalar()) {
            expr = op;
        } else if (op->name == Call::interleave_vectors &&
                   op->call_type == Call::Intrinsic &&
                   op->args.size() == 2 &&
                   starting_lane == 0 && lane_stride == 2) {
            expr = op->args[0];
        } else if (op->name == Call::interleave_vectors &&
                   op->call_type == Call::Intrinsic &&
                   op->args.size() == 2 &&
                   starting_lane == 1 && lane_stride == 2) {
            expr = op->args[1];
        } else if (op->name == Call::interleave_vectors &&
                   op->call_type == Call::Intrinsic &&
                   op->args.size() == 3 &&
                   starting_lane == 0 && lane_stride == 3) {
            expr = op->args[0];
        } else if (op->name == Call::interleave_vectors &&
                   op->call_type == Call::Intrinsic &&
                   op->args.size() == 3 &&
                   starting_lane == 1 && lane_stride == 3) {
            expr = op->args[1];
        } else if (op->name == Call::interleave_vectors &&
                   op->call_type == Call::Intrinsic &&
                   op->args.size() == 3 &&
                   starting_lane == 2 && lane_stride == 3) {
            expr = op->args[2];
        } else {

            Type t = op->type;
            t.width = new_width;

            // Vector calls are always parallel across the lanes, so we
            // can just deinterleave the args.

            // TODO: beware of other intrinsics for which this is not true!

            std::vector<Expr> args(op->args.size());
            for (size_t i = 0; i < args.size(); i++) {
                args[i] = mutate(op->args[i]);
            }

            expr = Call::make(t, op->name, args, op->call_type,
                              op->func, op->value_index, op->image, op->param);
        }
    }

    void visit(const Let *op) {
        if (op->type.is_vector()) {
            Expr new_value = mutate(op->value);
            std::string new_name = unique_name('t');
            Type new_type = new_value.type();
            Expr new_var = Variable::make(new_type, new_name);
            internal.push(op->name, new_var);
            Expr body = mutate(op->body);
            internal.pop(op->name);

            // Define the new name.
            expr = Let::make(new_name, new_value, body);

            // Someone might still use the old name.
            expr = Let::make(op->name, op->value, expr);
        } else {
            IRMutator::visit(op);
        }
    }
};

Expr extract_odd_lanes(Expr e, const Scope<int> &lets) {
    Deinterleaver d(lets);
    d.starting_lane = 1;
    d.lane_stride = 2;
    d.new_width = e.type().width/2;
    e = d.mutate(e);
    return simplify(e);
}

Expr extract_even_lanes(Expr e, const Scope<int> &lets) {
    Deinterleaver d(lets);
    d.starting_lane = 0;
    d.lane_stride = 2;
    d.new_width = (e.type().width+1)/2;
    e = d.mutate(e);
    return simplify(e);
}

Expr extract_even_lanes(Expr e) {
    Scope<int> lets;
    return extract_even_lanes(e, lets);
}

Expr extract_odd_lanes(Expr e) {
    Scope<int> lets;
    return extract_odd_lanes(e, lets);
}

Expr extract_mod3_lanes(Expr e, int lane, const Scope<int> &lets) {
    Deinterleaver d(lets);
    d.starting_lane = lane;
    d.lane_stride = 3;
    d.new_width = (e.type().width+2)/3;
    e = d.mutate(e);
    return simplify(e);
}

Expr extract_lane(Expr e, int lane) {
    Scope<int> lets;
    Deinterleaver d(lets);
    d.starting_lane = lane;
    d.lane_stride = 0;
    d.new_width = 1;
    e = d.mutate(e);
    return simplify(e);
}

class Interleaver : public IRMutator {
    Scope<ModulusRemainder> alignment_info;

    Scope<int> vector_lets;

    using IRMutator::visit;

    bool should_deinterleave;
    int num_lanes;

    Expr deinterleave_expr(Expr e) {
        if (e.type().width <= 2) {
            // Just scalarize
            return e;
        } else if (num_lanes == 2) {
            Expr a = extract_even_lanes(e, vector_lets);
            Expr b = extract_odd_lanes(e, vector_lets);
            return Call::make(e.type(), Call::interleave_vectors,
                              vec(a, b), Call::Intrinsic);
        } else if (num_lanes == 3) {
            Expr a = extract_mod3_lanes(e, 0, vector_lets);
            Expr b = extract_mod3_lanes(e, 1, vector_lets);
            Expr c = extract_mod3_lanes(e, 2, vector_lets);
            return Call::make(e.type(), Call::interleave_vectors,
                              vec(a, b, c), Call::Intrinsic);
        } else { // if (num_lanes == 4)
            Expr a = extract_even_lanes(e, vector_lets);
            Expr b = extract_odd_lanes(e, vector_lets);
            Expr aa = extract_even_lanes(a, vector_lets);
            Expr ab = extract_odd_lanes(a, vector_lets);
            Expr ba = extract_even_lanes(b, vector_lets);
            Expr bb = extract_odd_lanes(b, vector_lets);
            return Call::make(e.type(), Call::interleave_vectors,
                              vec(aa, ab, ba, bb), Call::Intrinsic);
        }
    }

    template<typename T, typename Body>
    Body visit_let(const T *op) {
        Expr value = mutate(op->value);
        if (value.type() == Int(32)) {
            alignment_info.push(op->name, modulus_remainder(value, alignment_info));
        }

        if (value.type().is_vector()) {
            vector_lets.push(op->name, 0);
        }
        Body body = mutate(op->body);
        if (value.type().is_vector()) {
            vector_lets.pop(op->name);
        }
        if (value.type() == Int(32)) {
            alignment_info.pop(op->name);
        }

        Body result;
        if (value.same_as(op->value) && body.same_as(op->body)) {
            result = op;
        } else {
            result = T::make(op->name, value, body);
        }

        // For vector lets, we may additionally need a let defining the even and odd lanes only
        if (value.type().is_vector()) {
            result = T::make(op->name + ".even_lanes", extract_even_lanes(value, vector_lets), result);
            result = T::make(op->name + ".odd_lanes", extract_odd_lanes(value, vector_lets), result);
            result = T::make(op->name + ".lanes_0_of_3", extract_mod3_lanes(value, 0, vector_lets), result);
            result = T::make(op->name + ".lanes_1_of_3", extract_mod3_lanes(value, 1, vector_lets), result);
            result = T::make(op->name + ".lanes_2_of_3", extract_mod3_lanes(value, 2, vector_lets), result);
        }

        return result;
    }

    void visit(const Let *op) {
        expr = visit_let<Let, Expr>(op);
    }

    void visit(const LetStmt *op) {
        stmt = visit_let<LetStmt, Stmt>(op);
    }

    void visit(const Mod *op) {
        const Ramp *r = op->a.as<Ramp>();
        for (int i = 2; i <= 4; ++i) {
            if (r && is_const(op->b, i)) {
                should_deinterleave = true;
                num_lanes = i;
                break;
            }
        }
        IRMutator::visit(op);
    }

    void visit(const Div *op) {
        const Ramp *r = op->a.as<Ramp>();
        for (int i = 2; i <= 4; ++i) {
            if (r && is_const(op->b, i)) {
                should_deinterleave = true;
                num_lanes = i;
                break;
            }
        }
        IRMutator::visit(op);
    }

    void visit(const Load *op) {
        bool old_should_deinterleave = should_deinterleave;
        int old_num_lanes = num_lanes;

        should_deinterleave = false;
        Expr idx = mutate(op->index);
        expr = Load::make(op->type, op->name, idx, op->image, op->param);
        if (should_deinterleave) {
            expr = deinterleave_expr(expr);
        }

        should_deinterleave = old_should_deinterleave;
        num_lanes = old_num_lanes;
    }

    void visit(const Store *op) {
        bool old_should_deinterleave = should_deinterleave;
        int old_num_lanes = num_lanes;

        should_deinterleave = false;
        Expr idx = mutate(op->index);
        if (should_deinterleave) {
            idx = deinterleave_expr(idx);
        }

        should_deinterleave = false;
        Expr value = mutate(op->value);
        if (should_deinterleave) {
            value = deinterleave_expr(value);
        }

        stmt = Store::make(op->name, value, idx);

        should_deinterleave = old_should_deinterleave;
        num_lanes = old_num_lanes;
    }

    void visit(const Block *op) {
        const Store *s = op->first.as<Store>();
        if (s) {
            const Ramp *r = s->index.as<Ramp>();

            if (r && is_const(r->stride)) {
                const int *stride = as_const_int(r->stride);
                const int width = r->width;

                std::vector<Store> stores;
                stores.push_back(*s);

                Stmt rest = collect_strided_stores(op->rest, s->name, *stride, stores);

                if (stores.size() == (size_t)*stride) {
                  bool okay_to_interleave = true;
                  int min_offset = 0;
                  std::vector<int> offsets(*stride);
                  for (int i = 0; i < *stride; ++i) {
                    const Ramp *r0 = stores[0].index.as<Ramp>();
                    const Ramp *ri = stores[i].index.as<Ramp>();
                    if (ri->width != width) {
                      okay_to_interleave = false;
                      break;
                    }

                    Expr diff = simplify(ri->base - r0->base);

                    const int *offs = as_const_int(diff);
                    if (offs != 0) {
                      offsets[i] = *offs;
                      if (*offs < min_offset) {
                        min_offset = *offs;
                      }
                    } else {
                      okay_to_interleave = false;
                      break;
                    }
                  }

                  if (okay_to_interleave) {
                    bool should_interleave = true;
                    Expr base;
                    std::vector<Expr> args(*stride);
                    for (int i = 0; i < *stride; ++i) {
                      int j = offsets[i] - min_offset;
                      if (j == 0) {
                        base = stores[i].index.as<Ramp>()->base;
                      }

                      if (args[j].defined()) {
                        should_interleave = false;
                        break;
                      }

                      args[j] = stores[i].value;
                    }

                    if (should_interleave) {
                      Type t = s->value.type();
                      t.width = width*(*stride);
                      Expr index = Ramp::make(base, make_one(Int(32)), t.width);
                      Expr value = Call::make(t, Call::interleave_vectors, args, Call::Intrinsic);
                      stmt = Block::make(Store::make(s->name, value, index), mutate(rest));
                      return;
                    }
                  }
                }
            }
        }

        stmt = Block::make(mutate(op->first), mutate(op->rest));
    }
public:
    Interleaver() : should_deinterleave(false) {}
};

Stmt rewrite_interleavings(Stmt s) {
    return Interleaver().mutate(s);
}

namespace {
void check(Expr a, Expr even, Expr odd) {
    a = simplify(a);
    Expr correct_even = extract_even_lanes(a);
    Expr correct_odd = extract_odd_lanes(a);
    if (!equal(correct_even, even)) {
        internal_error << correct_even << " != " << even << "\n";
    }
    if (!equal(correct_odd, odd)) {
        internal_error << correct_odd << " != " << odd << "\n";
    }
}
}

void deinterleave_vector_test() {
    std::pair<Expr, Expr> result;
    Expr x = Variable::make(Int(32), "x");
    Expr ramp = Ramp::make(x + 4, 3, 7);
    Expr ramp_a = Ramp::make(x + 4, 6, 4);
    Expr ramp_b = Ramp::make(x + 7, 6, 3);
    Expr broadcast = Broadcast::make(x + 4, 16);
    Expr broadcast_a = Broadcast::make(x + 4, 8);
    Expr broadcast_b = broadcast_a;

    check(ramp, ramp_a, ramp_b);
    check(broadcast, broadcast_a, broadcast_b);

    check(Load::make(ramp.type(), "buf", ramp, Buffer(), Parameter()),
          Load::make(ramp_a.type(), "buf", ramp_a, Buffer(), Parameter()),
          Load::make(ramp_b.type(), "buf", ramp_b, Buffer(), Parameter()));

    std::cout << "deinterleave_vector test passed" << std::endl;
}

}
}
