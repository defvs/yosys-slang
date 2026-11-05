//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
// clang-format off
#include <functional>
#include <map>
#include <string>
#include <optional>
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/SemanticFacts.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/TimingControl.h"
#include "kernel/rtlil.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/text/SourceLocation.h"

#include "slang_frontend.h"
#include "statements.h"
#include "diag.h"

namespace slang_frontend {

// This portion was written by Louis-Emile Ploix "mndstrmr" (c) 2025; ISC licence
// Brought into Slang head by Mel Young 2026, since reworked and extended (see git history)

static slang::SourceLocation expr_loc(const ast::AssertionExpr& expr) {
	return expr.syntax ? expr.syntax->sourceRange().start() : slang::SourceLocation::NoLocation;
}

static ir::Value delay_sva_sample(EvalContext& eval, ir::Value sig, int64_t cycles, ir::Trit init,
								  slang::SourceLocation loc = slang::SourceLocation::NoLocation,
								  std::string_view name_hint = "sva_delay") {
	if (cycles == 0)
		return sig;

	ProceduralContext *procedural = eval.procedural;
	if (procedural == nullptr || procedural->timing.kind != ProcessTiming::EdgeTriggered ||
			procedural->timing.triggers.size() != 1 || !procedural->timing_matches_process) {
		eval.netlist.add_diag(diag::SVATemporalDelayRequiresClock, loc);
		return ir::Value(ir::Sx, sig.size());
	}

	// A signal delayed again by any number of ticks reuses the registers made for it
	// before, so a value sampled by the assertions of a module takes one line of
	// registers, however many paths read it on however many ticks (see SvaFormulas)
	auto &trigger = procedural->timing.triggers[0];
	auto &lines = eval.netlist.backend->sva_delay_lines;
	auto &line = lines[{sig.raw(), init, trigger.signal.raw(), trigger.edge_polarity,
						procedural->timing.background_enable.raw()}];
	while ((int64_t) line.size() < cycles) {
		ir::Value prev = line.empty() ? sig : line.back();
		ir::Value next = eval.netlist.add_placeholder_signal(sig.size(), name_hint);
		eval.netlist.set_initialization(next, ir::Const(init, sig.size()));
		eval.netlist.add_dffe(eval.netlist.backend->new_id(std::string(name_hint)),
							  trigger.signal, procedural->timing.background_enable, prev, next,
							  trigger.edge_polarity, true);
		line.push_back(next);
	}
	return line[cycles - 1];
}

// Boolean formulas over the values an attempt samples, each read by the tick of the
// attempt it's sampled on.
//
// The paths of a property are kept as such formulas while the property gets lowered.
// A path that ends later in the attempt reads the same ticks as before, so paths can be
// combined however far apart they end, with nothing to delay in between. Once the whole
// property is lowered, its formula becomes logic on the last tick an attempt can take,
// with each value delayed by the ticks since it was sampled. Every value then takes one
// line of registers whatever the number of paths reading it, and a formula shared by
// several paths gets built once.
class SvaFormulas {
public:
	using Node = int;
	static constexpr Node False = 0;
	static constexpr Node True = 1;

	EvalContext &eval;

	// With `ticked_constants`, a constant sampled after the first tick of the attempt is
	// kept as a value of its tick, not folded into the formulas reading it, so that a
	// prefix takes it for unknown until that tick has come (see early_rejection()). The
	// logic built for a formula has its constants folded in all the same, see folded().
	SvaFormulas(EvalContext &eval_, bool ticked_constants_): eval(eval_), ticked_constants(ticked_constants_) {
		ops.push_back({});
		ops.push_back({});
	}

	static bool is_const(Node node) { return node == False || node == True; }

	// An operand read by the rules of IEEE Std 1800-2017 section 16.14.6, with `frozen`
	// telling if it reads a value frozen when the attempt was queued. It's sampled the
	// same way on any tick, so its logic is built once however often it's placed.
	ir::Net sva(const ast::Expression &expr, bool &frozen) {
		auto it = evaluated.find(&expr);
		if (it == evaluated.end()) {
			ir::Net bit = eval.netlist.ReduceBool(eval.sva(expr));
			it = evaluated.emplace(&expr, std::make_pair(bit, eval.sva_read_frozen)).first;
		}
		frozen = it->second.second;
		return it->second.first;
	}

	// The value of `bit` sampled on tick `tick` of the attempt. A constant is folded unless
	// it's kept on its tick, and on the first tick it's known from the start anyway.
	Node value(ir::Net bit, int64_t tick) {
		if (bit.is_const() && (!ticked_constants || tick <= 0))
			return bit == ir::S1 ? True : False;

		auto [it, inserted] = values.emplace(std::make_pair(bit.raw(), tick), (Node) ops.size());
		if (inserted) {
			Op op;
			op.kind = Op::Value;
			op.sig = bit;
			op.tick = tick;
			op.last = tick;
			ops.push_back(op);
		}
		return it->second;
	}

	// The value of `logic`, built for `formula` on the tick its path ends on, sampled
	// `ticks` later on tick `tick`. The prefixes of the value are those of the formula,
	// `ticks` later.
	Node placed_logic(ir::Net logic, int64_t tick, Node formula, int64_t ticks) {
		Node node = value(logic, tick);
		if (!is_const(node))
			placed_from[node] = {formula, ticks};
		return node;
	}

	Node conj(Node a, Node b) {
		if (a == False || b == False)
			return False;
		if (a == True)
			return b;
		if (b == True || a == b)
			return a;
		return intern(Op::And, std::min(a, b), std::max(a, b), False);
	}

	Node disj(Node a, Node b) {
		if (a == True || b == True)
			return True;
		if (a == False)
			return b;
		if (b == False || a == b)
			return a;
		return intern(Op::Or, std::min(a, b), std::max(a, b), False);
	}

	Node neg(Node a) {
		if (is_const(a))
			return a == True ? False : True;
		if (ops[a].kind == Op::Not)
			return ops[a].a;
		return intern(Op::Not, a, False, False);
	}

	// `b` where `sel` holds, `a` elsewhere
	Node mux(Node sel, Node a, Node b) {
		if (sel == False || a == b)
			return a;
		if (sel == True)
			return b;
		if (b == True)
			return disj(a, sel);
		if (b == False)
			return conj(a, neg(sel));
		return intern(Op::Mux, sel, a, b);
	}

	// `root` with the constants kept on their ticks folded in, as they are once the
	// attempt has reached the ticks of the formula. It's the formula that folding them
	// right away would have given.
	Node folded(Node root) {
		if (!ticked_constants)
			return root;

		std::vector<Node> stack = {root};
		while (!stack.empty()) {
			Node node = stack.back();
			if (is_const(node) || folded_nodes.count(node)) {
				stack.pop_back();
				continue;
			}

			// Folding adds formulas, a reference would dangle
			const Op op = ops[node];
			if (op.kind == Op::Value) {
				folded_nodes[node] = !op.sig.is_const() ? node : op.sig == ir::S1 ? True : False;
				stack.pop_back();
				continue;
			}

			bool ready = true;
			for (Node operand : {op.a, op.b, op.c}) {
				if (!is_const(operand) && !folded_nodes.count(operand)) {
					stack.push_back(operand);
					ready = false;
				}
			}
			if (!ready)
				continue;
			stack.pop_back();

			auto get = [&](Node operand) {
				return is_const(operand) ? operand : folded_nodes.at(operand);
			};
			Node result;
			switch (op.kind) {
			case Op::And: result = conj(get(op.a), get(op.b)); break;
			case Op::Or: result = disj(get(op.a), get(op.b)); break;
			case Op::Not: result = neg(get(op.a)); break;
			case Op::Mux: result = mux(get(op.a), get(op.b), get(op.c)); break;
			case Op::Shifted: result = shifted(get(op.a), op.tick); break;
			default: assert_invariant(false && "unreachable");
			}
			folded_nodes[node] = result;
		}
		return is_const(root) ? root : folded_nodes.at(root);
	}

	// The number of gates making up `root`, counted up to `limit`
	size_t gates(Node root, size_t limit) {
		std::vector<Node> stack = {folded(root)};
		Yosys::pool<Node> seen;
		size_t count = 0;
		while (!stack.empty() && count <= limit) {
			Node node = stack.back();
			stack.pop_back();
			if (is_const(node) || ops[node].kind == Op::Value || !seen.insert(node).second)
				continue;
			count++;
			for (Node operand : {ops[node].a, ops[node].b, ops[node].c})
				stack.push_back(operand);
		}
		return count;
	}

	// Logic for `root` on tick `tick` of the attempt, a tick none of the values it reads
	// is sampled after
	ir::Net materialize(Node root, int64_t tick, slang::SourceLocation loc) {
		root = folded(root);
		// Formulas get deep, a delay chained over a long range is as deep as the range
		std::vector<Node> stack = {root};
		while (!stack.empty()) {
			Node node = stack.back();
			if (is_const(node) || materialized.count({node, tick})) {
				stack.pop_back();
				continue;
			}

			const Op &op = ops[node];
			if (op.kind == Op::Value) {
				assert_invariant(op.tick <= tick);
				ir::Value delayed = delay_sva_sample(eval, op.sig, tick - op.tick, ir::Sx, loc);
				materialized[{node, tick}] = delayed.as_net();
				stack.pop_back();
				continue;
			}

			bool ready = true;
			for (Node operand : {op.a, op.b, op.c}) {
				if (!is_const(operand) && !materialized.count({operand, tick})) {
					stack.push_back(operand);
					ready = false;
				}
			}
			if (!ready)
				continue;
			stack.pop_back();

			auto get = [&](Node operand) -> ir::Net {
				if (is_const(operand))
					return operand == True ? ir::S1 : ir::S0;
				return materialized.at({operand, tick});
			};
			auto &netlist = eval.netlist;
			ir::Net result;
			switch (op.kind) {
			case Op::And: result = netlist.LogicAnd(get(op.a), get(op.b)); break;
			case Op::Or:  result = netlist.LogicOr(get(op.a), get(op.b)); break;
			case Op::Not: result = netlist.LogicNot(get(op.a)); break;
			case Op::Mux: result = netlist.Mux(get(op.b), get(op.c), get(op.a)).as_net(); break;
			default: assert_invariant(false && "unreachable");
			}
			materialized[{node, tick}] = result;
		}

		if (is_const(root))
			return root == True ? ir::S1 : ir::S0;
		return materialized.at({root, tick});
	}

	// What the ticks of an attempt so far tell of a formula, before the values it reads
	// later on have been sampled: on each tick, a formula over the values sampled up to
	// that tick that holds wherever the formula can still turn out true (with `may`), or
	// that holds only where it's sure to turn out true (without).
	//
	// These are the formula in three-valued logic, with the values not sampled yet
	// unknown: `may` of a conjunction is the conjunction of the operands' `may`s, `may` of
	// a negation is the negation of the operand's other prefix. An attempt is doomed
	// wherever `may` is low, whatever comes next, since the logic never takes an unknown
	// for known. It can take a known value for unknown though, as with a value read both
	// ways on a later tick, so some doomed attempts only show on a later tick.
	//
	// A prefix changes on the ticks the formula reads values on and is kept as a list
	// of checkpoints, latest first, each holding up to the next one. From the last tick
	// of the formula on it's the formula itself, before its first checkpoint `may` holds
	// and `must` doesn't. Checkpoints are interned, so a prefix built on another one
	// shares its checkpoints, as with a path and the paths extending it.
	using Prefix = int;
	static constexpr Prefix AlwaysTrue = 0;
	static constexpr Prefix AlwaysFalse = 2;

	// Start over on prefixes that read the values up to tick `last` only, as if those
	// after were never sampled, with `budget` steps of merging. Past the budget,
	// checkpoints get dropped, which leaves some attempts to show doomed on a later tick.
	// A prefix that drops a checkpoint holds on to the one before it, and with a value
	// sampled only ever ruling out more, that still holds where the formula can.
	void start_prefixes(int64_t last, size_t budget) {
		horizon = last;
		prefixes.clear();
		merged.clear();
		shifted_prefixes.clear();
		merge_steps = 0;
		merge_budget = budget;
		dropped = false;
	}

	// Did the prefixes since start_prefixes() drop checkpoints over the budget?
	bool prefixes_dropped() const {
		return dropped;
	}

	// Bound the logic built for prefixes to `budget` gates and registers more
	void limit_prefix_logic(size_t budget) {
		logic_budget = logic_cost + budget;
	}

	size_t size() const {
		return ops.size();
	}

	Prefix prefix(Node root, bool may) {
		std::vector<std::pair<Node, bool>> stack = {{root, may}};
		while (!stack.empty()) {
			auto [node, node_may] = stack.back();
			if (prefixes.count({node, node_may})) {
				stack.pop_back();
				continue;
			}

			// Merging adds formulas, a reference would dangle
			const Op op = ops[node];
			std::vector<std::pair<Node, bool>> operands;
			if (op.kind == Op::Not) {
				// A negation reads the other prefix of its operand
				operands = {{op.a, !node_may}};
			} else if (op.kind == Op::Mux) {
				operands = {{op.a, true}, {op.a, false}, {op.b, node_may}, {op.c, node_may}};
			} else if (op.kind == Op::And || op.kind == Op::Or) {
				operands = {{op.a, node_may}, {op.b, node_may}};
			} else if (op.kind == Op::Value && placed_from.count(node)) {
				operands = {{placed_from.at(node).first, node_may}};
			}
			assert_invariant(op.kind != Op::Shifted);
			bool ready = true;
			for (auto operand : operands) {
				if (!is_const(operand.first) && !prefixes.count(operand)) {
					stack.push_back(operand);
					ready = false;
				}
			}
			if (!ready)
				continue;
			stack.pop_back();

			auto get = [&](Node operand, bool operand_may) {
				if (is_const(operand))
					return operand == True ? AlwaysTrue : AlwaysFalse;
				return prefixes.at({operand, operand_may});
			};
			Prefix result;
			switch (op.kind) {
			case Op::Const:
				result = node == True ? AlwaysTrue : AlwaysFalse;
				break;
			case Op::Value:
				if (placed_from.count(node)) {
					auto [formula, ticks] = placed_from.at(node);
					result = shift_prefix(get(formula, node_may), ticks, node_may);
				} else if (op.tick > horizon) {
					result = node_may ? AlwaysTrue : AlwaysFalse;
				} else {
					result = checkpoint(op.tick, node, node_may ? AlwaysTrue : AlwaysFalse);
				}
				break;
			case Op::Not:
				result = negated(get(op.a, !node_may));
				break;
			case Op::And:
			case Op::Or:
				result = merge(op.kind, node_may, get(op.a, node_may), get(op.b, node_may));
				break;
			case Op::Mux: {
				// `c` if the select can be (or is) high, `b` if it can be (or is) low
				Prefix high = merge(Op::And, node_may, get(op.a, node_may), get(op.c, node_may));
				Prefix low = merge(Op::And, node_may, negated(get(op.a, !node_may)), get(op.b, node_may));
				result = merge(Op::Or, node_may, high, low);
				break;
			}
			default:
				assert_invariant(false && "unreachable");
			}
			prefixes[{node, node_may}] = result;
		}
		return prefixes.at({root, may});
	}

	Prefix negated(Prefix p) const {
		// The two prefixes holding a constant throughout swap
		return is_bottom(p) ? (p ^ 2) : (p ^ 1);
	}

	// The tick of the latest checkpoint of `p`, below any tick if it's constant
	int64_t prefix_tick(Prefix p) const {
		return checkpoints[p >> 1].tick;
	}

	// What `p` holds from its latest checkpoint on
	Node prefix_value(Prefix p) {
		Node value = checkpoints[p >> 1].value;
		return (p & 1) ? neg(value) : value;
	}

	// The checkpoints of `p` before its latest
	Prefix prefix_earlier(Prefix p) const {
		return earlier(p);
	}

	// Logic for `root` on tick `tick` of the attempt, a prefix on a tick of its own, or
	// none if that takes more than the budget left (see limit_prefix_logic()). Prefixes
	// on many ticks are built one on the other, and materialize() would build all those
	// before again on each tick. A formula that's been built on an earlier tick gets
	// delayed from there instead.
	std::optional<ir::Net> materialize_prefix(Node root, int64_t tick, slang::SourceLocation loc) {
		// A prefix on a tick reads the values up to that tick, the constants among them
		// are known
		root = folded(root);
		std::vector<std::pair<Node, int64_t>> stack = {{root, tick}};
		while (!stack.empty()) {
			auto [node, at] = stack.back();
			if (is_const(node) || materialized.count({node, at})) {
				stack.pop_back();
				continue;
			}

			const Op op = ops[node];
			assert_invariant(op.last <= at);
			if (op.kind == Op::Value) {
				// The line of the value reaches that far for the last tick already
				materialized[{node, at}] = delay_sva_sample(eval, op.sig, at - op.tick, ir::Sx, loc).as_net();
				stack.pop_back();
				continue;
			}

			// What's been built so far is left for the checks on later ticks to use
			if (logic_cost >= logic_budget)
				return {};

			auto earlier_tick = built_on.find(node);
			if (earlier_tick != built_on.end() && earlier_tick->second < at) {
				ir::Net built = materialized.at({node, earlier_tick->second});
				materialized[{node, at}] = delay_sva_sample(eval, built, at - earlier_tick->second, ir::Sx, loc).as_net();
				logic_cost += at - earlier_tick->second;
				earlier_tick->second = at;
				stack.pop_back();
				continue;
			}

			// A shifted formula reads its values that much later
			std::vector<std::pair<Node, int64_t>> operands;
			if (op.kind == Op::Shifted)
				operands = {{op.a, at - op.tick}};
			else
				operands = {{op.a, at}, {op.b, at}, {op.c, at}};
			bool ready = true;
			for (auto operand : operands) {
				if (!is_const(operand.first) && !materialized.count(operand)) {
					stack.push_back(operand);
					ready = false;
				}
			}
			if (!ready)
				continue;
			stack.pop_back();

			auto get = [&](Node operand) -> ir::Net {
				if (is_const(operand))
					return operand == True ? ir::S1 : ir::S0;
				return materialized.at({operand, at});
			};
			auto &netlist = eval.netlist;
			ir::Net result;
			switch (op.kind) {
			case Op::And: result = netlist.LogicAnd(get(op.a), get(op.b)); break;
			case Op::Or:  result = netlist.LogicOr(get(op.a), get(op.b)); break;
			case Op::Not: result = netlist.LogicNot(get(op.a)); break;
			case Op::Mux: result = netlist.Mux(get(op.b), get(op.c), get(op.a)).as_net(); break;
			case Op::Shifted: result = materialized.at(operands[0]); break;
			default: assert_invariant(false && "unreachable");
			}
			if (op.kind != Op::Shifted)
				logic_cost++;
			materialized[{node, at}] = result;
			auto [it, inserted] = built_on.emplace(node, at);
			if (!inserted)
				it->second = std::max(it->second, at);
		}

		if (is_const(root))
			return root == True ? ir::S1 : ir::S0;
		return materialized.at({root, tick});
	}

private:
	struct Op {
		// A shifted formula is formula `a` with each value read `tick` ticks later. It
		// stands for prefixes of an operand placed later on, the logic of which is shared.
		enum Kind { Const, Value, And, Or, Not, Mux, Shifted } kind = Const;
		// Operands, `a` selecting between `b` and `c` for a mux
		Node a = False, b = False, c = False;
		ir::Net sig = ir::S0;
		int64_t tick = 0;
		int64_t last = INT64_MIN;
	};

	bool ticked_constants;
	std::vector<Op> ops;
	Yosys::dict<std::pair<RTLIL::SigBit, int64_t>, Node> values;
	Yosys::dict<Node, Node> folded_nodes;
	Yosys::dict<std::tuple<int, Node, Node, Node>, Node> interned;
	Yosys::dict<std::pair<Node, int64_t>, ir::Net> materialized;
	std::map<const ast::Expression *, std::pair<ir::Net, bool>> evaluated;

	Node intern(typename Op::Kind kind, Node a, Node b, Node c) {
		auto [it, inserted] = interned.emplace(std::make_tuple((int) kind, a, b, c), (Node) ops.size());
		if (inserted) {
			Op op;
			op.kind = kind;
			op.a = a;
			op.b = b;
			op.c = c;
			op.last = std::max({ops[a].last, ops[b].last, ops[c].last});
			ops.push_back(op);
		}
		return it->second;
	}

	// Prefixes, see prefix()

	struct Checkpoint {
		int64_t tick;
		Node value;
		Prefix earlier;
	};

	std::vector<Checkpoint> checkpoints = {{INT64_MIN, True, -1}, {INT64_MIN, False, -1}};
	Yosys::dict<std::tuple<int64_t, Node, Prefix>, Prefix> checkpoint_ids;
	Yosys::dict<std::pair<Node, bool>, Prefix> prefixes;
	Yosys::dict<std::tuple<int, bool, Prefix, Prefix>, Prefix> merged;
	Yosys::dict<std::tuple<Prefix, int64_t, bool>, Prefix> shifted_prefixes;
	Yosys::dict<std::pair<Node, int64_t>, Node> shifted_formulas;
	Yosys::dict<Node, std::pair<Node, int64_t>> placed_from;
	// The latest tick a formula has been built on for a prefix
	Yosys::dict<Node, int64_t> built_on;
	int64_t horizon = INT64_MAX;
	size_t merge_steps = 0, merge_budget = 0;
	bool dropped = false;
	size_t logic_cost = 0, logic_budget = 0;

	// `node` with each value read `ticks` later
	Node shifted(Node node, int64_t ticks) {
		if (is_const(node) || ticks == 0)
			return node;
		if (ops[node].kind == Op::Shifted)
			return shifted(ops[node].a, ops[node].tick + ticks);
		auto [it, inserted] = shifted_formulas.emplace(std::make_pair(node, ticks), (Node) ops.size());
		if (inserted) {
			Op op;
			op.kind = Op::Shifted;
			op.a = node;
			op.tick = ticks;
			op.last = ops[node].last + ticks;
			ops.push_back(op);
		}
		return it->second;
	}

	// `p` with each checkpoint `ticks` later, a `may` prefix or not
	Prefix shift_prefix(Prefix p, int64_t ticks, bool may) {
		std::vector<Prefix> steps;
		Prefix rest;
		while (true) {
			if (is_bottom(p)) {
				rest = p;
				break;
			}
			if (auto it = shifted_prefixes.find({p, ticks, may}); it != shifted_prefixes.end()) {
				rest = it->second;
				break;
			}
			if (merge_steps >= merge_budget) {
				rest = may ? AlwaysTrue : AlwaysFalse;
				dropped = true;
				break;
			}
			merge_steps++;
			// The value placed later can read past the horizon
			if (prefix_tick(p) + ticks <= horizon)
				steps.push_back(p);
			p = earlier(p);
		}
		for (auto step = steps.rbegin(); step != steps.rend(); step++) {
			rest = checkpoint(prefix_tick(*step) + ticks, shifted(prefix_value(*step), ticks), rest);
			shifted_prefixes[{*step, ticks, may}] = rest;
		}
		return rest;
	}

	static bool is_bottom(Prefix p) {
		return (p >> 1) < 2;
	}

	Prefix earlier(Prefix p) const {
		Prefix next = checkpoints[p >> 1].earlier;
		return (p & 1) ? negated(next) : next;
	}

	// A checkpoint on `tick`, ahead of those of `rest`. One holding what the checkpoint
	// before it holds tells nothing new, so it's left out.
	Prefix checkpoint(int64_t tick, Node value, Prefix rest) {
		if (prefix_value(rest) == value)
			return rest;
		auto [it, inserted] = checkpoint_ids.emplace(std::make_tuple(tick, value, rest),
													 (Prefix) (checkpoints.size() << 1));
		if (inserted)
			checkpoints.push_back({tick, value, rest});
		return it->second;
	}

	// The conjunction or disjunction of two values a prefix holds. Merging prefixes
	// goes over the checkpoints of both, which the absorption laws keep from piling up
	// copies of what they read.
	Node prefix_op(typename Op::Kind kind, Node a, Node b) {
		auto absorbs = [&](Node x, Node y) {
			// Is `x` an operation of the dual kind on `y`, or of the same kind?
			if (is_const(x) || (ops[x].kind != Op::And && ops[x].kind != Op::Or))
				return false;
			return ops[x].a == y || ops[x].b == y;
		};
		for (auto [x, y] : {std::make_pair(a, b), std::make_pair(b, a)}) {
			if (absorbs(x, y))
				return ops[x].kind == kind ? x : y;
		}
		return kind == Op::And ? conj(a, b) : disj(a, b);
	}

	// The prefix holding the conjunction (or disjunction) of what `x` and `y` hold, going
	// over the checkpoints of both, latest first, up to where they've been merged before
	Prefix merge(typename Op::Kind kind, bool may, Prefix x, Prefix y) {
		struct Step {
			Prefix x, y;
			int64_t tick;
			Node value;
		};
		std::vector<Step> steps;
		auto key = [&](Prefix p, Prefix q) {
			return std::make_tuple((int) kind, may, std::min(p, q), std::max(p, q));
		};

		Prefix rest;
		while (true) {
			if (x == y) {
				rest = x;
				break;
			}
			if (is_bottom(x) || is_bottom(y)) {
				// A prefix holding a constant throughout
				Prefix constant = is_bottom(x) ? x : y, other = is_bottom(x) ? y : x;
				if ((constant == AlwaysTrue) == (kind == Op::And))
					rest = other;
				else
					rest = constant;
				break;
			}
			if (auto it = merged.find(key(x, y)); it != merged.end()) {
				rest = it->second;
				break;
			}
			if (merge_steps >= merge_budget) {
				// Dropping checkpoints only ever makes a prefix hold on more ticks
				// (`may`) or on fewer (`must`), which is what either one stands for
				if (kind == Op::And)
					rest = may ? x : AlwaysFalse;
				else
					rest = may ? AlwaysTrue : x;
				dropped = true;
				break;
			}
			merge_steps++;

			int64_t x_tick = checkpoints[x >> 1].tick, y_tick = checkpoints[y >> 1].tick;
			steps.push_back({x, y, std::max(x_tick, y_tick), prefix_op(kind, prefix_value(x), prefix_value(y))});
			if (x_tick >= y_tick)
				x = earlier(x);
			if (y_tick >= x_tick)
				y = earlier(y);
		}

		for (auto step = steps.rbegin(); step != steps.rend(); step++) {
			rest = checkpoint(step->tick, step->value, rest);
			merged[key(step->x, step->y)] = rest;
		}
		return rest;
	}
};

struct AssertionMatch {
	SvaFormulas& f;
	SvaFormulas::Node sig;
	// Ticks from the operand start. Ranges are 32-bit, so their sums take 64 bits.
	int64_t start;
	bool empty;
	// Reads a value frozen when the attempt was queued, valid for the first tick only
	bool frozen;
	slang::SourceLocation loc;

	AssertionMatch(SvaFormulas& f_, SvaFormulas::Node sig_,
				   slang::SourceLocation loc_ = slang::SourceLocation::NoLocation):
		f(f_), sig(sig_), start(0), empty(false), frozen(false), loc(loc_) {}
	AssertionMatch(const AssertionMatch&) = default;

private:
	AssertionMatch(SvaFormulas& f_, SvaFormulas::Node sig_, int64_t start_, bool empty_,
				   bool frozen_, slang::SourceLocation loc_):
		f(f_), sig(sig_), start(start_), empty(empty_), frozen(frozen_), loc(loc_) {}

public:
	static AssertionMatch Empty(SvaFormulas& f, slang::SourceLocation loc) {
		return { f, SvaFormulas::True, 0, true, false, loc };
	}

	// An operand sampled on tick `tick` of the attempt
	static AssertionMatch Operand(SvaFormulas& f, const ast::Expression& expr, int64_t tick,
								  slang::SourceLocation loc) {
		bool frozen;
		ir::Net bit = f.sva(expr, frozen);
		AssertionMatch ret(f, f.value(bit, tick), loc);
		ret.frozen = frozen;
		return ret;
	}

	AssertionMatch& operator=(const AssertionMatch& other) {
		sig = other.sig;
		start = other.start;
		empty = other.empty;
		frozen = other.frozen;
		loc = other.loc;
		return *this;
	}

	// The path ending `time` ticks later. It reads the same ticks of the attempt as
	// before, so its formula stays as it is.
	AssertionMatch shift(int64_t time) const {
		return { f, sig, start + time, empty, frozen, loc };
	}

	AssertionMatch operator||(const AssertionMatch& other) const {
		assert_invariant(!empty && !other.empty);
		return { f, f.disj(sig, other.sig), std::max(other.start, start), false, frozen || other.frozen, loc };
	}

	AssertionMatch operator&&(const AssertionMatch& other) const {
		assert_invariant(!empty && !other.empty);
		return { f, f.conj(sig, other.sig), std::max(other.start, start), false, frozen || other.frozen, loc };
	}

	AssertionMatch operator!() const {
		assert_invariant(!empty);
		return { f, f.neg(sig), start, false, frozen, loc };
	}
};

// Automatic variables and const casts keep the value they had when the attempt
// was queued (IEEE Std 1800-2017 section 16.14.6.1), while we evaluate a path at
// the tick it's placed at, so such a path can't go past the attempt's first tick.
static void check_frozen_offset(const AssertionMatch& path, int64_t offset) {
	if (path.frozen && offset > 0)
		path.f.eval.netlist.add_diag(diag::SVAFrozenValueAfterFirstTick, path.loc);
}

// The disable condition of an attempt, by the ticks it's sampled on
class DisableSpans {
public:
	DisableSpans(const AssertionMatch& abort_, ir::Net condition_): abort(abort_), condition(condition_) {}

	// Has the condition been true on any of the ticks from `first` to `last` ticks before
	// tick `tick` of the attempt?
	AssertionMatch span(int64_t tick, int64_t first, int64_t last) {
		check_frozen_offset(abort, last);

		// Spans reach back to the start of an operand, so they're kept by the tick they
		// begin on and grow from there
		int64_t begin = tick - last;
		auto& from_begin = spans[begin];
		while ((int64_t) from_begin.size() <= last - first) {
			auto now = abort.f.value(condition, begin + (int64_t) from_begin.size());
			from_begin.push_back(from_begin.empty() ? now : abort.f.disj(from_begin.back(), now));
		}
		return AssertionMatch(abort.f, from_begin[last - first], abort.loc);
	}

private:
	AssertionMatch abort;
	ir::Net condition;
	std::map<int64_t, std::vector<SvaFormulas::Node>> spans;
};

// How a disable bears on what's being synthesized.
//
// Going by Annex F of IEEE Std 1800-2017, `disable iff (b) P` holds if P holds, or if P
// holds on the ticks before the first one with `b` extended by T letters. T satisfies
// any boolean and `not` swaps it for its dual, which satisfies none, not even 1'b1. A
// failure settled ahead of the disable hence stands, while a window over the attempt as
// a whole would cancel it. Instead of that each boolean gets rewritten for the letter
// it would see, leaving the disable no other part in lowering the attempt.
//
// Booleans see the disable from the start of the operand they're in. Where an operand is
// placed later on, the disable can also have come before it, in which case all of it is
// settled to the letter (see OperandPaths).
struct AttemptContext {
	// Nothing to rewrite if null
	DisableSpans *abort = nullptr;
	// Every boolean has the letter
	bool settled = false;
	// Is the letter the dual of T?
	bool dual = false;

	AttemptContext negated() const {
		return { abort, settled, !dual };
	}

	// A boolean on the first tick of the operand, tick `tick` of the attempt
	AssertionMatch atom(const AssertionMatch& boolean, int64_t tick) const {
		if (settled)
			return AssertionMatch(boolean.f, dual ? SvaFormulas::False : SvaFormulas::True, boolean.loc);

		// A constant can have the value of the letter to begin with
		if (abort == nullptr || (SvaFormulas::is_const(boolean.sig) && (boolean.sig == SvaFormulas::True) != dual))
			return boolean;

		auto now = abort->span(tick, 0, 0);
		return dual ? boolean && !now : boolean || now;
	}

	// A path of the 1'b1s of a delay, `length` ticks long from tick `tick` of the attempt.
	// Only the last one counts, as the next boolean in a path sees the disable over the
	// ticks before it as well. It's sampled on its tick like any other boolean.
	AssertionMatch true_path(SvaFormulas& f, int64_t tick, int64_t length, slang::SourceLocation loc) const {
		if (settled)
			return AssertionMatch(f, dual ? SvaFormulas::False : SvaFormulas::True, loc).shift(length);
		AssertionMatch path = AssertionMatch(f, f.value(ir::S1, tick + length), loc).shift(length);
		if (!dual || abort == nullptr)
			return path;
		return path && !abort->span(tick + length, 0, length);
	}
};

// Paths of an operand by the tick it starts on, counted from the start of the operand
// whose part it is. If the disable came ahead of that tick, the operand gets settled to
// the letter. Otherwise it's as if the operand started off the attempt, so we synthesize
// it on that tick, settle it once, and pick per tick.
class OperandPaths {
public:
	using Synthesize = std::function<std::vector<AssertionMatch>(const AttemptContext&, int64_t)>;
	using SynthesizeAfter = std::function<std::optional<std::vector<AssertionMatch>>(
			const AttemptContext&, int64_t, const std::vector<AssertionMatch>&)>;

	// `base` is the tick of the attempt the offsets count from. With `synthesize_after`,
	// the operand can also be synthesized after paths, see after().
	OperandPaths(const AttemptContext& ctx_, int64_t base_, Synthesize synthesize_,
				 SynthesizeAfter synthesize_after_ = nullptr):
		ctx(ctx_), base(base_), synthesize(std::move(synthesize_)),
		synthesize_after(std::move(synthesize_after_)), from_start(synthesize(ctx, base)) {}

	int64_t base_tick() const {
		return base;
	}

	// The operand placed on each tick one of `starts` ends on (counted from the base), as
	// far as that path matched, if it can be synthesized for all these ticks at once
	std::optional<std::vector<AssertionMatch>> after(const std::vector<AssertionMatch>& starts) {
		if (!synthesize_after)
			return {};
		return synthesize_after(ctx, base, starts);
	}

	const std::vector<AssertionMatch>& at(int64_t offset) {
		// A settled operand reads nothing of the attempt, it's the same on any tick
		if (offset == 0 || ctx.settled)
			return from_start;

		auto it = by_offset.find(offset);
		if (it != by_offset.end())
			return it->second;

		std::vector<AssertionMatch> paths;
		if (const auto& logic = shared_logic(); !logic.empty()) {
			// The logic of the operand placed `offset` ticks later is read that much later
			paths = from_start;
			for (size_t i = 0; i < paths.size(); i++)
				if (!paths[i].empty)
					paths[i].sig = paths[i].f.placed_logic(logic[i], base + offset + paths[i].start,
														   from_start[i].sig, offset);
		} else {
			paths = synthesize(ctx, base + offset);
		}
		assert_invariant(paths.size() == from_start.size());
		if (ctx.abort == nullptr)
			return by_offset.emplace(offset, std::move(paths)).first->second;

		if (!settled_paths.has_value()) {
			AttemptContext settled_ctx = ctx;
			settled_ctx.settled = true;
			settled_paths = synthesize(settled_ctx, base);
			assert_invariant(settled_paths->size() == from_start.size());
		}

		for (size_t i = 0; i < paths.size(); i++) {
			AssertionMatch& path = paths[i];
			const AssertionMatch& other = (*settled_paths)[i];
			assert_invariant(path.empty == other.empty && path.start == other.start);
			if (path.empty || path.sig == other.sig)
				continue;
			assert_invariant(SvaFormulas::is_const(other.sig));

			// Reads the disable from before the operand start, on the tick the path ends
			auto before = ctx.abort->span(base + offset + path.start, path.start + 1, path.start + offset);
			path.sig = path.f.mux(before.sig, path.sig, other.sig);
		}
		return by_offset.emplace(offset, std::move(paths)).first->second;
	}

private:
	AttemptContext ctx;
	int64_t base;
	Synthesize synthesize;
	SynthesizeAfter synthesize_after;
	std::vector<AssertionMatch> from_start;
	std::optional<std::vector<AssertionMatch>> settled_paths;
	std::map<int64_t, std::vector<AssertionMatch>> by_offset;
	std::optional<std::vector<ir::Net>> shared;

	// Logic for each path of the operand, on the tick the path ends, if it's to be shared
	// by the ticks the operand is placed on. Synthesized again on each tick, an operand
	// takes its gates again each time, while its logic takes a line of registers delaying
	// it, and that's fewer if the operand is a path of more than a few gates. An operand of
	// many paths would take a line for each, as many registers as gates are saved.
	const std::vector<ir::Net>& shared_logic() {
		static constexpr size_t few_gates = 8;

		if (!shared.has_value()) {
			shared.emplace();
			auto non_empty = std::count_if(from_start.begin(), from_start.end(),
										   [](const AssertionMatch& path) { return !path.empty; });
			auto path = std::find_if(from_start.begin(), from_start.end(),
									 [](const AssertionMatch& path) { return !path.empty; });
			if (non_empty == 1 && path->f.gates(path->sig, few_gates) > few_gates) {
				for (auto& path : from_start)
					shared->push_back(path.empty ? ir::S1 : path.f.materialize(path.sig, base + path.start, path.loc));
			}
		}
		return *shared;
	}
};

static std::vector<AssertionMatch> compress_paths(std::vector<AssertionMatch> paths) {
	std::vector<AssertionMatch> empty_paths;
	std::vector<AssertionMatch> non_empty_paths;
	for (auto path : paths) {
		if (path.empty)
			empty_paths.push_back(path);
		else
			non_empty_paths.push_back(path);
	}
	paths = non_empty_paths;

	struct amcmp {
		bool operator()(AssertionMatch& a, AssertionMatch& b) const {
			return a.start > b.start;
		}
	};
	std::sort(paths.begin(), paths.end(), amcmp {});

	std::vector<AssertionMatch> grouped;
	std::optional<AssertionMatch> group;
	int64_t time = -1;
	for (auto path : paths) {
		if (path.start != time) {
			if (group.has_value()) grouped.push_back(group.value());
			time = path.start;
			group = path;
		} else {
			group = group.value() || path;
		}
	}
	if (group.has_value()) grouped.push_back(group.value());
	if (!empty_paths.empty())
		grouped.push_back(empty_paths[0]);

	return grouped;
}

static AssertionMatch collapse_or(std::vector<AssertionMatch> paths) {
	assert_invariant(!paths.empty());

	// Going by start, what's collected so far is delayed up to the start of the next
	// path. That takes as many delay registers as the longest path, rather than as all
	// the paths together.
	std::sort(paths.begin(), paths.end(), [](const AssertionMatch& a, const AssertionMatch& b) {
		return a.start < b.start;
	});

	AssertionMatch collapsed = paths[0];
	assert_invariant(!collapsed.empty);
	for (size_t i = 1; i < paths.size(); i++) {
		assert_invariant(!paths[i].empty);
		collapsed = collapsed.shift(paths[i].start - collapsed.start) || paths[i];
	}
	return collapsed;
}

// Paths of a sequence used as a property. An empty match is no success, and with no
// other match to go by the property fails on every attempt.
static std::vector<AssertionMatch> property_paths(SvaFormulas& f, const AttemptContext& ctx,
												  std::vector<AssertionMatch> paths,
												  slang::SourceLocation loc, int64_t tick) {
	std::erase_if(paths, [](const AssertionMatch& path) { return path.empty; });
	if (paths.empty())
		paths.push_back(ctx.atom(AssertionMatch(f, f.value(ir::S0, tick), loc), tick));
	return paths;
}

// Negation of paths synthesized in the context `ctx`, starting on tick `tick` of the attempt
static std::vector<AssertionMatch> not_vec(SvaFormulas& f, const AttemptContext& ctx,
										   std::vector<AssertionMatch> in, slang::SourceLocation loc,
										   int64_t tick) {
	return { !collapse_or(property_paths(f, ctx, std::move(in), loc, tick)) };
}

// The disjunction of the paths ending within a window of ticks, for windows of a width
// that slide along the paths. The ticks are cut in blocks the width of the windows,
// and the paths collected from the start and from the end of each block, so that a
// window is the end of a block and the start of the next one. That takes three gates
// a tick at most, however wide the windows.
class EndWindows {
public:
	// `paths` going by end, with one path to a tick
	EndWindows(std::vector<AssertionMatch> paths_, int64_t width_): paths(std::move(paths_)), width(width_) {
		assert_invariant(!paths.empty() && width > 0);
		for (size_t i = 0; i < paths.size(); i++) {
			block.push_back((paths[i].start - paths[0].start) / width);
			bool block_start = i == 0 || block[i] != block[i - 1];
			from_block_start.push_back(block_start ? paths[i] : from_block_start.back().shift(paths[i].start - from_block_start.back().start) || paths[i]);
		}
		to_block_end.resize(paths.size(), paths.back());
		for (size_t i = paths.size(); i-- > 0;) {
			bool block_end = i == paths.size() - 1 || block[i] != block[i + 1];
			to_block_end[i] = block_end ? paths[i] : paths[i].shift(to_block_end[i + 1].start - paths[i].start) || to_block_end[i + 1];
		}
	}

	// Paths ending on ticks `first` to `last`, a window as wide as the blocks or one
	// tick narrower. Within a block, such a window takes in its start or its end.
	std::optional<AssertionMatch> get(int64_t first, int64_t last) const {
		assert_invariant(last - first + 1 <= width && last - first + 2 >= width);
		auto by_end = [](const AssertionMatch& path, int64_t tick) { return path.start < tick; };
		size_t lo = std::lower_bound(paths.begin(), paths.end(), first, by_end) - paths.begin();
		size_t hi = std::lower_bound(paths.begin(), paths.end(), last + 1, by_end) - paths.begin();
		if (lo >= hi)
			return {};
		hi--;

		if (block[lo] != block[hi]) {
			assert_invariant(block[hi] == block[lo] + 1);
			return to_block_end[lo].shift(paths[hi].start - to_block_end[lo].start) || from_block_start[hi];
		}
		if (lo == 0 || block[lo - 1] != block[lo])
			return from_block_start[hi];
		assert_invariant(hi == paths.size() - 1 || block[hi + 1] != block[hi]);
		return to_block_end[lo];
	}

private:
	std::vector<AssertionMatch> paths;
	int64_t width;
	std::vector<int64_t> block;
	std::vector<AssertionMatch> from_block_start;
	std::vector<AssertionMatch> to_block_end;
};

// `a` being synthesized in the context `ctx`
static std::vector<AssertionMatch> seq_vec(const AttemptContext& ctx, std::vector<AssertionMatch> a,
										   int64_t min, int64_t max, OperandPaths& b) {
	std::vector<AssertionMatch> new_own_paths;
	std::vector<AssertionMatch> ends;
	for (auto path : a) {
		if (!path.empty) {
			ends.push_back(path);
			continue;
		}

		for (int64_t offset = std::max<int64_t>(min, 1); offset <= max; offset++) {
			// `empty ##0 seq` has no match
			for (auto inner : b.at(offset - 1)) {
				if (inner.empty) {
					// `empty ##N empty` spans N - 1 ticks
					if (offset == 1)
						new_own_paths.push_back(path);
					else
						new_own_paths.push_back(ctx.true_path(path.f, b.base_tick(), offset - 2, path.loc));
					continue;
				}
				// `empty ##N seq` is `##(N-1) seq`
				check_frozen_offset(inner, offset - 1);
				new_own_paths.push_back(AssertionMatch(path.f, SvaFormulas::True, path.loc).shift(offset - 1 + inner.start) && inner);
			}
		}
	}
	if (ends.empty())
		return compress_paths(new_own_paths);

	// Paths ending on the same tick lead to the same placements of `b`, and so do paths
	// ending within the range of each other. Rather than place `b` after each path, we
	// place it once on each tick, after any of the paths ending in the range before.
	ends = compress_paths(ends);
	std::reverse(ends.begin(), ends.end());
	// The windows are the width of the range, or one tick narrower without `##0`
	EndWindows windows(ends, max - min + 1);
	std::vector<AssertionMatch> starts;
	for (int64_t tick = ends.front().start + min; tick <= ends.back().start + max; tick++) {
		auto before = windows.get(tick - max, tick - min);
		if (before.has_value())
			starts.push_back(before->shift(tick - before->start));
	}

	if (auto after = b.after(starts); after.has_value()) {
		new_own_paths.insert(new_own_paths.end(), after->begin(), after->end());
		return compress_paths(new_own_paths);
	}

	for (auto& before : starts) {
		int64_t tick = before.start;
		for (auto inner : b.at(tick)) {
			if (inner.empty) {
				// `seq ##N empty` is `seq ##(N-1) 1'b1`, with no match for `##0`
				auto nonzero = windows.get(tick - max, tick - std::max<int64_t>(min, 1));
				if (nonzero.has_value())
					new_own_paths.push_back(nonzero->shift(tick - 1 - nonzero->start) && ctx.true_path(nonzero->f, b.base_tick(), tick - 1, nonzero->loc));
				continue;
			}
			check_frozen_offset(inner, tick);
			new_own_paths.push_back(before.shift(inner.start) && inner);
		}
	}
	return compress_paths(new_own_paths);
}

static std::vector<AssertionMatch> seq_prefix_vec(SvaFormulas& f, const AttemptContext& ctx, int64_t min, int64_t max,
												  OperandPaths& b, slang::SourceLocation loc) {
	std::vector<AssertionMatch> paths;
	for (int64_t offset = min; offset <= max; offset++) {
		for (auto inner : b.at(offset)) {
			if (inner.empty) {
				// `##N empty` is `##(N-1) 1'b1`, for N = 0 there's no match
				if (offset > 0)
					paths.push_back(ctx.true_path(f, b.base_tick(), offset - 1, loc));
			} else {
				check_frozen_offset(inner, offset);
				paths.push_back(AssertionMatch(f, SvaFormulas::True, loc).shift(offset + inner.start) && inner);
			}
		}
	}
	return compress_paths(paths);
}

static std::vector<AssertionMatch> apply_repetition(SvaFormulas& f, const AttemptContext& ctx,
													const ast::AssertionExpr& expr,
													std::optional<ast::SequenceRepetition> repetition,
													OperandPaths& operand) {
	if (!repetition.has_value())
		return operand.at(0);

	if (repetition->kind != ast::SequenceRepetition::Consecutive || !repetition->range.max.has_value()) {
		f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		return {};
	}

	std::vector<AssertionMatch> repeated;
	if (repetition->range.min == 0)
		repeated.push_back(AssertionMatch::Empty(f, expr_loc(expr)));

	// Each count of repetitions is the count before followed by one more
	std::vector<AssertionMatch> count_paths;
	for (uint32_t count = 1; count <= repetition->range.max.value(); count++) {
		count_paths = count == 1 ? operand.at(0) : seq_vec(ctx, count_paths, 1, 1, operand);
		// No match for this count, none for the counts above
		if (count_paths.empty())
			break;
		if (count >= repetition->range.min)
			repeated.insert(repeated.end(), count_paths.begin(), count_paths.end());
	}

	return compress_paths(repeated);
}

// Is the expression a property proper? With `and` and `or` it's enough for one of the
// operands to be, the other one is then used as a property even if it's a sequence.
static bool is_property(const ast::AssertionExpr& expr) {
	if (expr.kind == ast::AssertionExprKind::Simple) {
		// slang gives these a sequence length, the type is what tells them apart
		const auto& operand = expr.as<ast::SimpleAssertionExpr>().expr;
		return operand.kind == ast::ExpressionKind::AssertionInstance && operand.type->isPropertyType();
	}

	if (expr.kind == ast::AssertionExprKind::Binary) {
		const auto& biop = expr.as<ast::BinaryAssertionExpr>();
		if (biop.op == ast::BinaryAssertionOperator::And || biop.op == ast::BinaryAssertionOperator::Or)
			return is_property(biop.left) || is_property(biop.right);
	}

	return !expr.computeSequenceLength().has_value();
}

static std::vector<AssertionMatch> synthesize_assertion_expr(SvaFormulas& f, const AttemptContext& ctx,
															 const ast::AssertionExpr& expr, int64_t base);
static const ast::AssertionExpr &unwrap_property_instances(const ast::AssertionExpr &expr);
static OperandPaths::SynthesizeAfter concat_after(SvaFormulas& f, const ast::AssertionExpr& expr);

// The concatenation `sequence` starting on `base`, or else after the paths `starts`, on
// each tick one of them ends on (counted from `base`), as far as that path matched.
//
// Placed after many paths, a concatenation of ranges would be synthesized again on each
// tick, with as many paths each time as its ranges are wide. Its elements are placed after
// the paths instead, as if part of the sequence before, which takes as many paths as there
// are ticks. That's the same placement: the disable from `base` on settles an element
// the same way, whether read by the concatenation from its start or by the element, and
// an element can't be placed past the first tick if it couldn't be before. None if the
// first element can be empty, as an empty match from a tick has nothing in common with a
// path ending there.
static std::optional<std::vector<AssertionMatch>> synthesize_concat(SvaFormulas& f, const AttemptContext& ctx,
																	const ast::SequenceConcatExpr& sequence, int64_t base,
																	const std::vector<AssertionMatch>* starts) {
	std::vector<AssertionMatch> own_paths;
	for (size_t i = 0; i < sequence.elements.size(); i++) {
		const auto& element = sequence.elements[i];
		OperandPaths inner_paths(ctx, base, [&](const AttemptContext& at, int64_t tick) {
			return synthesize_assertion_expr(f, at, *element.sequence, tick);
		}, concat_after(f, *element.sequence));

		auto delay = element.delay;
		if (!delay.max.has_value()) {
			f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(sequence));
			return std::vector<AssertionMatch>{};
		}

		// slang gives a bare first element a delay of zero too, which is
		// not the same as an explicit `##0` if the element can be empty
		bool bare = !element.delayRange.start().valid();
		if (i != 0) {
			own_paths = seq_vec(ctx, own_paths, delay.min, delay.max.value(), inner_paths);
		} else if (starts != nullptr) {
			const auto& first = inner_paths.at(0);
			if (bare && std::any_of(first.begin(), first.end(), [](const AssertionMatch& path) { return path.empty; }))
				return {};
			own_paths = seq_vec(ctx, *starts, delay.min, delay.max.value(), inner_paths);
		} else if (!bare) {
			own_paths = seq_prefix_vec(f, ctx, delay.min, delay.max.value(), inner_paths, expr_loc(sequence));
		} else {
			own_paths = compress_paths(inner_paths.at(0));
		}
	}
	return own_paths;
}

// Synthesis of `expr` after paths, if it's a concatenation, seen through instances of
// named sequences
static OperandPaths::SynthesizeAfter concat_after(SvaFormulas& f, const ast::AssertionExpr& expr) {
	const auto& unwrapped = unwrap_property_instances(expr);
	if (unwrapped.kind != ast::AssertionExprKind::SequenceConcat)
		return nullptr;

	const auto& sequence = unwrapped.as<ast::SequenceConcatExpr>();
	return [&f, &sequence](const AttemptContext& at, int64_t base, const std::vector<AssertionMatch>& starts) {
		return synthesize_concat(f, at, sequence, base, &starts);
	};
}

// An empty result stands for a sequence with no possible match. Constructs we
// can't lower also yield an empty result, but those have raised an error
// diagnostic so the design won't be read in.
//
// `base` is the tick of the attempt the expression starts on.
static std::vector<AssertionMatch> synthesize_assertion_expr(SvaFormulas& f, const AttemptContext& ctx,
															 const ast::AssertionExpr& expr, int64_t base) {
	switch (expr.kind) {
		case slang::ast::AssertionExprKind::Invalid:
			assert_invariant(false && "unreachable");
		case slang::ast::AssertionExprKind::Simple:
			{
				const auto& simple = expr.as<ast::SimpleAssertionExpr>();
				const ast::AssertionInstanceExpression *instance = nullptr;
				if (simple.expr.kind == ast::ExpressionKind::AssertionInstance)
					instance = &simple.expr.as<ast::AssertionInstanceExpression>();
				OperandPaths operand(ctx, base, [&](const AttemptContext& at, int64_t tick) -> std::vector<AssertionMatch> {
					if (instance) {
						if (instance->isRecursiveProperty) {
							f.eval.netlist.add_diag(diag::AssertionUnsupported, simple.expr.sourceRange);
							return {};
						}
						return synthesize_assertion_expr(f, at, instance->body, tick);
					}
					return { at.atom(simple.isNullExpr ? AssertionMatch(f, f.value(ir::S0, tick), expr_loc(expr))
													   : AssertionMatch::Operand(f, simple.expr, tick, expr_loc(expr)), tick) };
				}, instance && !instance->isRecursiveProperty ? concat_after(f, instance->body) : nullptr);
				return apply_repetition(f, ctx, expr, simple.repetition, operand);
			}
		case slang::ast::AssertionExprKind::SequenceConcat:
			return *synthesize_concat(f, ctx, expr.as<ast::SequenceConcatExpr>(), base, nullptr);
		case slang::ast::AssertionExprKind::Unary:
			{
				const auto& uop = expr.as<ast::UnaryAssertionExpr>();
				switch (uop.op) {
				case slang::ast::UnaryAssertionOperator::Not:
					return not_vec(f, ctx.negated(), synthesize_assertion_expr(f, ctx.negated(), uop.expr, base), expr_loc(expr),
								   base);

				case slang::ast::UnaryAssertionOperator::Eventually:
				case slang::ast::UnaryAssertionOperator::SEventually:
				{
					if (!uop.range.has_value() || !uop.range->max.has_value()) {
						f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
						return {};
					}

					// A property holds by any of its paths, so the operand placed on each tick
					// of the range can be the one path
					std::vector<AssertionMatch> true_path = {{f, SvaFormulas::True, expr_loc(expr)}};
					OperandPaths operand(ctx, base, [&](const AttemptContext& at, int64_t tick) {
						auto paths = property_paths(f, at, synthesize_assertion_expr(f, at, uop.expr, tick), expr_loc(expr), tick);
						return std::vector<AssertionMatch>{collapse_or(paths)};
					});
					return seq_vec(ctx, true_path, uop.range->min, uop.range->max.value(), operand);
				}

				case slang::ast::UnaryAssertionOperator::NextTime:
				case slang::ast::UnaryAssertionOperator::SNextTime:
				{
					uint32_t delay = 1;
					if (uop.range.has_value()) {
						if (!uop.range->max.has_value() || uop.range->min != uop.range->max.value()) {
							f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
							return {};
						}
						delay = uop.range->min;
					}

					std::vector<AssertionMatch> true_path = {{f, SvaFormulas::True, expr_loc(expr)}};
					OperandPaths operand(ctx, base, [&](const AttemptContext& at, int64_t tick) {
						return property_paths(f, at, synthesize_assertion_expr(f, at, uop.expr, tick), expr_loc(expr), tick);
					});
					return seq_vec(ctx, true_path, delay, delay, operand);
				}

				case slang::ast::UnaryAssertionOperator::Always:
				case slang::ast::UnaryAssertionOperator::SAlways:
					f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
					return {};
				}
				assert_invariant(false && "unreachable");
			}
		case slang::ast::AssertionExprKind::Binary:
			{
				const auto& biop = expr.as<ast::BinaryAssertionExpr>();

				bool implication = biop.op == ast::BinaryAssertionOperator::OverlappedImplication ||
								   biop.op == ast::BinaryAssertionOperator::NonOverlappedImplication;
				auto left_ctx = implication ? ctx.negated() : ctx;

				OperandPaths left_at(left_ctx, base, [&](const AttemptContext& at, int64_t tick) {
					return synthesize_assertion_expr(f, at, biop.left, tick);
				});
				// `R |-> P` is `not (R ##0 not P)`, and `R |=> P` the same with a `##1`
				OperandPaths right_at(ctx, base, [&](const AttemptContext& at, int64_t tick) {
					auto paths = synthesize_assertion_expr(f, at, biop.right, tick);
					return implication ? not_vec(f, at, std::move(paths), expr_loc(expr), tick) : paths;
				});
				auto left = left_at.at(0);
				auto right = right_at.at(0);

				switch (biop.op) {
				case ast::BinaryAssertionOperator::And:
				{
					/*
					When te1 and te2 are sequences, then the composite sequence te1 and te2 matches if te1 and te2 match.
					The end time is the end time of either te1 or te2, whichever matches last.
					*/
					if (is_property(biop)) {
						left = property_paths(f, ctx, std::move(left), expr_loc(expr), base);
						right = property_paths(f, ctx, std::move(right), expr_loc(expr), base);
					}

					// An empty match puts no constraint on the other operand
					std::vector<AssertionMatch> results;
					std::vector<AssertionMatch> left_ends, right_ends;
					for (auto& a : left) {
						if (a.empty)
							results.insert(results.end(), right.begin(), right.end());
						else
							left_ends.push_back(a);
					}
					for (auto& b : right) {
						if (b.empty)
							results.insert(results.end(), left_ends.begin(), left_ends.end());
						else
							right_ends.push_back(b);
					}

					// Two matches together end with the later one. The pairs ending on a
					// tick are thus a match of one operand ending there with a match of the
					// other ending there or before, and collecting the matches of each operand
					// as the ticks go by pairs them all with a few gates a tick.
					left_ends = compress_paths(left_ends);
					right_ends = compress_paths(right_ends);
					std::optional<AssertionMatch> left_so_far, right_so_far;
					auto collect = [](std::optional<AssertionMatch>& so_far, const AssertionMatch& path) {
						so_far = so_far.has_value() ? so_far->shift(path.start - so_far->start) || path : path;
					};
					// compress_paths() leaves the paths going by end, latest first
					auto l = left_ends.rbegin(), r = right_ends.rbegin();
					while (l != left_ends.rend() || r != right_ends.rend()) {
						int64_t end = std::min(l != left_ends.rend() ? l->start : INT64_MAX,
											   r != right_ends.rend() ? r->start : INT64_MAX);
						bool left_here = l != left_ends.rend() && l->start == end;
						bool right_here = r != right_ends.rend() && r->start == end;
						if (left_here)
							collect(left_so_far, *l);
						if (right_here)
							collect(right_so_far, *r);
						if (left_here && right_so_far.has_value())
							results.push_back(*l && right_so_far->shift(end - right_so_far->start));
						if (right_here && left_so_far.has_value())
							results.push_back(*r && left_so_far->shift(end - left_so_far->start));
						if (left_here)
							l++;
						if (right_here)
							r++;
					}
					return compress_paths(results);
				}
				case ast::BinaryAssertionOperator::Or:
				{
					/*
					If the operands te1 and te2 are expressions, then te1 or te2 matches at any clock tick on which at least
					one of te1 and te2 evaluates to true.
					*/
					left.insert(left.end(), right.begin(), right.end());
					return left;
				}
				case ast::BinaryAssertionOperator::Throughout:
				{
					if (left.empty() || right.empty()) return {};

					auto condition = collapse_or(left);
					if (condition.start != 0) {
						f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
						return {};
					}

					// The condition on all the ticks up to a tick is the condition up to the
					// tick before and on that tick, so it extends by a gate a tick for all the
					// paths at once
					std::vector<AssertionMatch> held;
					std::vector<AssertionMatch> results;
					for (auto path : right) {
						if (path.empty) {
							results.push_back(path);
							continue;
						}

						check_frozen_offset(condition, path.start);
						while ((int64_t) held.size() <= path.start) {
							int64_t t = held.size();
							auto condition_at_t = collapse_or(left_at.at(t)).shift(t);
							held.push_back(t == 0 ? condition_at_t : held.back() && condition_at_t);
						}
						results.push_back(path && held[path.start]);
					}
					return compress_paths(results);
				}
				case ast::BinaryAssertionOperator::Intersect:
				{
					std::vector<AssertionMatch> results;
					for (auto a : left) {
						for (auto b : right) {
							if (a.empty || b.empty) {
								if (a.empty && b.empty)
									results.push_back(a);
								continue;
							}
							if (a.start == b.start)
								results.push_back(a && b);
						}
					}
					return compress_paths(results);
				}
				case ast::BinaryAssertionOperator::OverlappedImplication:
				case ast::BinaryAssertionOperator::NonOverlappedImplication:
				{
					int delay = biop.op == ast::BinaryAssertionOperator::NonOverlappedImplication ? 1 : 0;
					return not_vec(f, left_ctx, seq_vec(left_ctx, left, delay, delay, right_at), expr_loc(expr), base);
				}

				case ast::BinaryAssertionOperator::Within:
				case ast::BinaryAssertionOperator::Iff:
				case ast::BinaryAssertionOperator::Until:
				case ast::BinaryAssertionOperator::SUntil:
				case ast::BinaryAssertionOperator::UntilWith:
				case ast::BinaryAssertionOperator::SUntilWith:
				case ast::BinaryAssertionOperator::Implies:
				case ast::BinaryAssertionOperator::OverlappedFollowedBy:
				case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
					f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
					return {};
				}
				assert_invariant(false && "unreachable");
			}
		case slang::ast::AssertionExprKind::Clocking:
			{
				f.eval.netlist.add_diag(diag::UnsupportedSVAFeature, expr_loc(expr));
				return {};
			}
		case slang::ast::AssertionExprKind::DisableIff:
			// eval_assertion() handles the one a property is allowed, at the top of it.
			// slang only rejects the others in direct instances of a property.
			f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return {};

		case slang::ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
					return {};
				}

				OperandPaths operand(ctx, base, [&](const AttemptContext& at, int64_t tick) {
					return synthesize_assertion_expr(f, at, with_match.expr, tick);
				}, concat_after(f, with_match.expr));
				return apply_repetition(f, ctx, expr, with_match.repetition, operand);
			}
		case slang::ast::AssertionExprKind::FirstMatch:
			{
				const auto& first_match = expr.as<ast::FirstMatchAssertionExpr>();
				if (!first_match.matchItems.empty()) {
					f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
					return {};
				}

				auto paths = compress_paths(synthesize_assertion_expr(f, ctx, first_match.seq, base));
				std::vector<AssertionMatch> results;

				// compress_paths() keeps any empty match as the last path
				if (!paths.empty() && paths.back().empty)
					return { paths.back() };

				// A path matches first if none of those ending before it did. Those are
				// collected going by end, once for all the paths.
				std::optional<AssertionMatch> earlier;
				for (auto path = paths.rbegin(); path != paths.rend(); path++) {
					if (earlier.has_value()) {
						auto earlier_here = earlier->shift(path->start - earlier->start);
						results.push_back(*path && !earlier_here);
						earlier = earlier_here || *path;
					} else {
						results.push_back(*path);
						earlier = *path;
					}
				}

				return compress_paths(results);
			}
		case slang::ast::AssertionExprKind::StrongWeak:
			{
				// The sequence used as a property. A bounded one has settled every match
				// it has once its longest path is over, where weak and strong agree.
				const auto& strong_weak = expr.as<ast::StrongWeakAssertionExpr>();
				return property_paths(f, ctx, synthesize_assertion_expr(f, ctx, strong_weak.expr, base), expr_loc(expr), base);
			}
		case slang::ast::AssertionExprKind::Abort:
		case slang::ast::AssertionExprKind::Conditional:
		case slang::ast::AssertionExprKind::Case:
			f.eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return {};
	}
	assert_invariant(false && "unreachable");
}

struct AssertionResult {
	ir::Value a;
	ir::Value en;
};

// An assumption constrains a trace on the ticks it gets checked on only. Checked on the
// last tick of an attempt, it leaves a trace that ends sooner unconstrained, even when no
// way of going on would satisfy the property. A bounded or inductive proof, or a cover,
// then gets to pick traces the environment rules out, where IEEE Std 1800-2017 section
// 16.14.2 has the tools constrain the environment so that the property holds. So an
// assumption gets checked as well on each tick on which what has been sampled of an
// attempt so far can show it doomed, and those checks are folded into one cell with the
// one on the last tick.
//
// Doomed means no way of going on satisfies the property, and one way is the clock
// stopping for good: a process clock gated by `iff` can tick no more, and so can any
// clock in a flow with several clocks. A weak operator whose ticks never come then holds
// (sections 16.12.10 and 16.12.13), and a weak sequence holds if it can match once the
// missing ticks are padded with T letters (Annex F.5.3.1), which satisfy every boolean,
// 1'b0 included. So the prefixes take whatever is sampled on a later tick for unknown,
// constants too (see SvaFormulas::value()). An unknown stands for T and its dual as well
// as for any value, so an attempt shown doomed fails whichever way the trace goes on, the
// clock stopping included, and if it keeps ticking, the check on the last tick fails too.
//
// Over many ticks, prefixes can take more merging than the budget for them allows, in
// proportion to the property, and then the earliest checkpoints are the ones to go. Those
// matter the most though, a trace ending on the first ticks of an attempt the soonest. So
// the prefixes get worked out over the first tick of the attempt first, then over twice as
// many ticks each time, for as long as they take no more than the budget.
static void early_rejection(SvaFormulas& f, const AssertionMatch& sig, ir::Net attempt,
							AssertionResult& result) {
	// Prefixes built one on the other take a step or two per formula
	static constexpr size_t work_per_node = 4;
	const size_t budget = work_per_node * f.size();

	// Up to tick `exact`, the checks are from prefixes that dropped no checkpoint
	std::map<int64_t, SvaFormulas::Node> checks;
	int64_t exact = -1;
	for (int64_t horizon = 0; exact < sig.start - 1; horizon = std::min(2 * horizon + 1, sig.start - 1)) {
		f.start_prefixes(horizon, budget);
		for (auto p = f.prefix(sig.sig, true);; p = f.prefix_earlier(p)) {
			// Before its first checkpoint a prefix is constant, from the start of the attempt on
			int64_t tick = std::max<int64_t>(f.prefix_tick(p), 0);
			if (tick <= exact)
				break;
			SvaFormulas::Node value = f.folded(f.prefix_value(p));
			if (tick < sig.start && value != SvaFormulas::True)
				checks.emplace(tick, value);
			if (tick == 0)
				break;
		}
		// The checks after `exact` hold all the same, some come later than they could
		if (f.prefixes_dropped())
			break;
		exact = horizon;
	}

	auto &netlist = f.eval.netlist;
	ir::Net en = netlist.ReduceBool(result.en);
	std::optional<ir::Net> a;
	f.limit_prefix_logic(budget);
	// The earliest first, those are the ones a check on the last tick misses most
	Yosys::pool<SvaFormulas::Node> checked;
	for (auto [tick, value] : checks) {
		// The same formula on a later tick reads the same samples of the attempt, it can
		// only fail where the earlier check has failed already. Constants on their ticks
		// make for many of these.
		if (!checked.insert(value).second)
			continue;
		auto holds = f.materialize_prefix(value, tick, sig.loc);
		if (!holds.has_value())
			break;
		if (!a.has_value())
			a = netlist.LogicOr(netlist.ReduceBool(result.a), netlist.LogicNot(en));
		ir::Net attempted = delay_sva_sample(f.eval, attempt, tick, ir::S0, sig.loc).as_net();
		en = netlist.LogicOr(en, attempted);
		a = netlist.LogicAnd(*a, netlist.LogicOr(*holds, netlist.LogicNot(attempted)));
	}
	if (a.has_value())
		result = { *a, en };
}

static bool timing_from_sva_clocking(NetlistContext &netlist, const ast::TimingControl &clocking,
									 ProcessTiming &timing);

// Unwrap non-recursive property instances down to the actual top expression. A repeated
// instance is a top expression itself, its body only makes up one of the iterations.
static const ast::AssertionExpr &unwrap_property_instances(const ast::AssertionExpr &expr)
{
	if (expr.kind != ast::AssertionExprKind::Simple)
		return expr;

	const auto &simple = expr.as<ast::SimpleAssertionExpr>();
	if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
		return expr;

	const auto &instance = simple.expr.as<ast::AssertionInstanceExpression>();
	if (instance.isRecursiveProperty || simple.repetition.has_value())
		return expr;

	return unwrap_property_instances(instance.body);
}

static const ast::ClockingAssertionExpr *get_top_clocking_expr(const ast::AssertionExpr &expr)
{
	const ast::AssertionExpr &unwrapped = unwrap_property_instances(expr);
	if (ast::ClockingAssertionExpr::isKind(unwrapped.kind))
		return &unwrapped.as<ast::ClockingAssertionExpr>();
	return nullptr;
}

// Does an explicit clocking event have the same effect as the enclosing process timing?
// An `iff` guard or an asynchronous load stops the process, but not the clocking event.
static bool clocking_matches_process(const ProcessTiming &clocking, const ProcessTiming &process)
{
	if (clocking.kind != process.kind)
		return false;
	if (clocking.triggers.size() != process.triggers.size())
		return false;
	if (process.background_enable != ir::S1)
		return false;

	for (size_t i = 0; i < clocking.triggers.size(); i++) {
		if (clocking.triggers[i].edge_polarity != process.triggers[i].edge_polarity)
			return false;
		if (clocking.triggers[i].signal != process.triggers[i].signal)
			return false;
	}

	return true;
}

// `attempt` is the condition of the procedural code around the assertion. It's read
// when the statement is reached and decides if an attempt is queued, the attempts
// in flight run to completion regardless (IEEE Std 1800-2017 section 16.14.6).
//
// An attempt is reported on the last tick it can take, which is when an assertion or
// a cover gets checked. An assumption also gets checked on each tick on which a doomed
// attempt can show, see early_rejection().
static AssertionResult eval_assertion(EvalContext& eval, const ast::AssertionExpr& assertion,
									  ir::Net attempt, bool cover, bool assume) {
	const ast::AssertionExpr *property = &assertion;
	const ast::Expression *condition = nullptr;
	slang::SourceLocation disable_loc = expr_loc(assertion);

	const ast::AssertionExpr &top = unwrap_property_instances(assertion);
	if (ast::DisableIffAssertionExpr::isKind(top.kind)) {
		const auto& disableiff = top.as<ast::DisableIffAssertionExpr>();
		property = &disableiff.expr;
		condition = &disableiff.condition;
		disable_loc = expr_loc(top);
	} else if (eval.netlist.process_scope) {
		auto scope = eval.netlist.process_scope;
		condition = scope->getCompilation().getDefaultDisable(*scope);
	}

	SvaFormulas f(eval, assume);
	AttemptContext ctx;
	std::optional<DisableSpans> spans;
	if (condition) {
		// IEEE Std 1800-2017 section 16.12 has a simulator test the disable condition on
		// current values at any time during the attempt. Like the other operands, we read
		// it sampled on the ticks of the assertion's clock, as formal tools do. A condition
		// changing on a clock edge thus takes effect one tick later than in simulation,
		// and a pulse between two ticks goes unseen.
		auto abort = AssertionMatch::Operand(f, *condition, 0, disable_loc);
		bool frozen;
		ir::Net sampled = f.sva(*condition, frozen);
		if (sampled != ir::S0) {
			ctx.abort = &spans.emplace(abort, sampled);
			// A cover is the dual of an assertion of the negation, its letters are the
			// dual of T. It succeeds where the attempt was settled ahead of the disable.
			ctx.dual = cover;
		}
	}

	auto paths = property_paths(f, ctx, synthesize_assertion_expr(f, ctx, *property, 0), expr_loc(assertion), 0);
	auto sig = collapse_or(paths);

	// Starts out low, so no attempt gets reported before it had the time to finish
	auto attempted = delay_sva_sample(eval, attempt, sig.start, ir::S0, sig.loc);
	AssertionResult result = { f.materialize(sig.sig, sig.start, sig.loc), attempted };
	if (assume)
		early_rejection(f, sig, attempt, result);
	return result;
}

// The body of a sequence or property names signals of the instance it's declared in.
// Unless that instance is part of the netlist we're building, those are hierarchical
// references across a preserved module boundary.
static bool check_instance_realms(NetlistContext &netlist, const ast::AssertionExpr &expr)
{
	bool ok = true;
	auto visitor = ast::makeVisitor([&](auto &visitor, const ast::AssertionInstanceExpression &instance) {
		// Declarations in a package belong to no realm
		if (netlist.find_symbol_realm(instance.symbol) &&
				!netlist.check_hier_ref(instance.symbol, instance.sourceRange)) {
			ok = false;
			return;
		}
		if (!instance.isRecursiveProperty)
			visitor.visitDefault(instance);
	});
	expr.visit(visitor);
	return ok;
}

// Lower a concurrent assertion to a check on the timing of `procedural`, with `attempt`
// for the procedural code around the assertion queueing an attempt
//
// Any top level clocking expressions have been stripped. Clocking is part
// of the procedural context.
static void lower_sva_property(const ast::ConcurrentAssertionStatement &statement,
							   const ast::StatementBlockSymbol *block,
							   ProceduralContext &procedural, const ast::AssertionExpr &top_expr,
							   ir::Net attempt)
{
	auto &netlist = procedural.netlist;

	const ast::AssertionExpr *expr = &top_expr;
	if (auto clocking_expr = get_top_clocking_expr(*expr)) {
		ProcessTiming timing(ProcessTiming::EdgeTriggered);
		if (!timing_from_sva_clocking(netlist, clocking_expr->clocking, timing))
			return;
		if (!clocking_matches_process(timing, procedural.timing)) {
			netlist.add_diag(diag::UnsupportedSVAFeature, expr_loc(*expr));
			return;
		}
		expr = &clocking_expr->expr;
	}

	AssertionResult result = eval_assertion(procedural.eval, *expr, attempt,
											statement.assertionKind == ast::AssertionKind::CoverProperty,
											statement.assertionKind == ast::AssertionKind::Assume);

	std::string flavor;
	switch (statement.assertionKind) {
	case ast::AssertionKind::Assert:        flavor = "assert"; break;
	case ast::AssertionKind::Assume:        flavor = "assume"; break;
	case ast::AssertionKind::CoverProperty: flavor = "cover"; break;
	default:                                netlist.add_diag(diag::AssertionUnsupported, statement.sourceRange); return;
	}

	std::string cell_name;

	if (block && unwrap_statement(block->tryGetStatement()) == &statement && !block->name.empty()) {
		// If we are the sole statement in a block, use the block's label. Loops can
		// get us here more than once, a suffix tells those apart.
		cell_name = netlist.backend->canvas->uniquify(netlist.id(*block)).str();
	} else {
		cell_name = netlist.backend->new_id();
	}

	ir::Net a = netlist.ReduceBool(result.a);

	auto cell = netlist.backend->canvas->addCell(std::move(cell_name), ID($check));
	procedural.timing.extract_trigger(netlist, cell, netlist.ReduceBool(result.en));
	cell->setParam(ID::FLAVOR, flavor);
	cell->setParam(ID::FORMAT, std::string(""));
	cell->setParam(ID::ARGS_WIDTH, 0);
	cell->setParam(ID::PRIORITY, --procedural.effects_priority);
	cell->setPort(ID::ARGS, {});
	cell->setPort(ID::A, {a});
	transfer_attrs(netlist, statement, cell);
}

static bool timing_from_sva_clocking(NetlistContext &netlist, const ast::TimingControl &clocking,
									 ProcessTiming &timing)
{
	if (ast::EventListControl::isKind(clocking.kind)) {
		const auto &event_list = clocking.as<ast::EventListControl>();
		if (event_list.events.size() != 1) {
			netlist.add_diag(diag::UnsupportedSVAFeature, clocking.sourceRange);
			return false;
		}
		return timing_from_sva_clocking(netlist, *event_list.events[0], timing);
	}

	if (!ast::SignalEventControl::isKind(clocking.kind)) {
		netlist.add_diag(diag::UnsupportedSVAFeature, clocking.sourceRange);
		return false;
	}

	const auto &signal_event = clocking.as<ast::SignalEventControl>();
	switch (signal_event.edge) {
	case ast::EdgeKind::None:
		netlist.add_diag(diag::SVAClockingRequiresEdge, signal_event.sourceRange);
		return false;

	case ast::EdgeKind::PosEdge:
	case ast::EdgeKind::NegEdge: {
		// Unlike the operands, the clock isn't read through EvalContext::sva(), which
		// checks the functions called in them
		check_assertion_calls(netlist, signal_event.expr);
		ir::Value signal = netlist.eval(signal_event.expr);
		if (signal.size() != 1) {
			netlist.add_diag(diag::UnsupportedSVAFeature, signal_event.sourceRange);
			return false;
		}
		timing.triggers.push_back(ProcessTiming::Sensitivity {
			.signal = signal.as_net(),
			.edge_polarity = (signal_event.edge == ast::EdgeKind::PosEdge),
			.ast_node = &clocking
		});
		break;
	}

	case ast::EdgeKind::BothEdges:
		netlist.add_diag(diag::BothEdgesUnsupported, signal_event.sourceRange);
		return false;
	}

	if (signal_event.iffCondition) {
		// TODO
		netlist.add_diag(diag::IffUnsupported, signal_event.iffCondition->sourceRange);
		return false;
	}

	return true;
}

// The clock of an assertion with none inferred from its surroundings: its own, which
// gets stripped from `expr`, or else the default clocking (IEEE Std 1800-2017 sections
// 16.14.6 and 16.16)
static const ast::TimingControl *assertion_clocking(NetlistContext &netlist,
													const ast::AssertionExpr *&expr)
{
	if (auto clocking_expr = get_top_clocking_expr(*expr)) {
		expr = &clocking_expr->expr;
		return &clocking_expr->clocking;
	}

	const ast::Scope *scope = netlist.process_scope;
	if (!scope)
		return nullptr;

	auto default_clocking = scope->getCompilation().getDefaultClocking(*scope);
	return default_clocking ? &default_clocking->as<ast::ClockingBlockSymbol>().getEvent() : nullptr;
}

// Lower a concurrent assertion on a clock of its own. In a process the attempts are
// queued by the code around the assertion, as of the clock tick, outside of one there's
// an attempt on each tick.
static void process_clocked_sva_property(NetlistContext &netlist,
										 const ast::ConcurrentAssertionStatement &statement,
										 const ast::StatementBlockSymbol *block,
										 const ast::TimingControl &clocking,
										 const ast::AssertionExpr &expr,
										 ProceduralContext *process = nullptr)
{
	ProcessTiming timing(ProcessTiming::EdgeTriggered);
	if (!timing_from_sva_clocking(netlist, clocking, timing))
		return;

	ProceduralContext procedure(netlist, timing);
	ir::Net attempt;
	if (process) {
		// Read the variables of the process as they are at the assertion
		procedure.eval.scope_nest_level = process->eval.scope_nest_level;
		procedure.inherit_state(*process);
		attempt = process->case_enable();
	} else {
		attempt = procedure.case_enable();
	}
	lower_sva_property(statement, block, procedure, expr, attempt);

	RTLIL::Process *rtlil_proc = netlist.backend->canvas->addProcess(netlist.backend->new_id());
	transfer_attrs(netlist, statement, rtlil_proc);
#ifndef SLANG_MUX_LOWERING
	procedure.copy_case_tree_into(rtlil_proc->root_case);
#endif
}

// Process a concurrent assertion in procedural code
void process_sva_property(const ast::ConcurrentAssertionStatement &statement,
						  const ast::StatementBlockSymbol *block,
						  ProceduralContext &procedural, const ast::AssertionExpr &top_expr)
{
	auto &netlist = procedural.netlist;

	if (!check_instance_realms(netlist, top_expr))
		return;

	// No clock gets inferred from a process with no edge in its event control. The
	// branches of an asynchronous reset have none either, but belong to one with an edge.
	if (procedural.timing.kind == ProcessTiming::Implicit && procedural.timing_matches_process) {
		const ast::AssertionExpr *expr = &top_expr;
		if (auto clocking = assertion_clocking(netlist, expr)) {
			process_clocked_sva_property(netlist, statement, block, *clocking, *expr, &procedural);
			return;
		}
	}

	lower_sva_property(statement, block, procedural, top_expr, procedural.case_enable());
}

void process_freestanding_sva_property(NetlistContext &netlist,
									   const ast::ConcurrentAssertionStatement &statement,
						  			   const ast::StatementBlockSymbol *block)
{
	const ast::AssertionExpr *expr = &statement.propertySpec;

	if (!check_instance_realms(netlist, *expr))
		return;

	if (auto clocking = assertion_clocking(netlist, expr)) {
		process_clocked_sva_property(netlist, statement, block, *clocking, *expr);
		return;
	}

	// No clocking
	ProceduralContext procedure(netlist, ProcessTiming::implicit);
	lower_sva_property(statement, block, procedure, *expr, procedure.case_enable());

	RTLIL::Process *rtlil_proc = netlist.backend->canvas->addProcess(netlist.backend->new_id());
	transfer_attrs(netlist, statement, rtlil_proc);
#ifndef SLANG_MUX_LOWERING
	procedure.copy_case_tree_into(rtlil_proc->root_case);
#endif
}

};
