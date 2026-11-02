//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
// clang-format off
#include <string>
#include <optional>
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/SemanticFacts.h"
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
// Brought into Slang head by Mel Young 2026, no additional work

static constexpr uint32_t SVA_ENUMERATION_LIMIT = 1024;

static slang::SourceLocation expr_loc(const ast::AssertionExpr& expr) {
	return expr.syntax ? expr.syntax->sourceRange().start() : slang::SourceLocation::NoLocation;
}

// SVA monitors clock their state from the procedure they are lowered in
static bool sva_clock_ready(EvalContext& eval, slang::SourceLocation loc) {
	ProceduralContext *procedural = eval.procedural;
	if (procedural == nullptr || procedural->timing.kind != ProcessTiming::EdgeTriggered ||
			procedural->timing.triggers.size() != 1 || !procedural->timing_matches_process) {
		eval.netlist.add_diag(diag::SVATemporalDelayRequiresClock, loc);
		return false;
	}
	return true;
}

// Must be preceded by a successful sva_clock_ready() check
static void add_sva_flop(EvalContext& eval, ir::Value d, ir::Value q, std::string_view name) {
	auto &timing = eval.procedural->timing;
	auto &trigger = timing.triggers[0];
	eval.netlist.add_dffe(eval.netlist.backend->new_id(std::string(name)), trigger.signal,
						  timing.background_enable, d, q, trigger.edge_polarity, true);
}

static ir::Net add_sva_state(EvalContext& eval, std::string_view name) {
	ir::Value state = eval.netlist.add_placeholder_signal(1, name);
	eval.netlist.set_initialization(state, ir::Const(ir::S0, 1));
	return state.as_net();
}

static ir::Net sva_not(EvalContext& eval, ir::Net value) {
	if (value.is_const() && value != ir::Sx)
		return value == ir::S0 ? ir::S1 : ir::S0;
	return eval.netlist.LogicNot(value);
}

static ir::Value delay_sva_sample(EvalContext& eval, ir::Value sig, int cycles, ir::Trit init,
								  slang::SourceLocation loc = slang::SourceLocation::NoLocation,
								  std::string_view name_hint = "sva_delay") {
	if (cycles == 0)
		return sig;

	if (!sva_clock_ready(eval, loc))
		return ir::Value(ir::Sx, sig.size());

	for (int i = 0; i < cycles; i++) {
		ir::Value next = eval.netlist.add_placeholder_signal(sig.size(), name_hint);
		eval.netlist.set_initialization(next, ir::Const(init, sig.size()));
		add_sva_flop(eval, sig, next, name_hint);
		sig = next;
	}
	return sig;
}

// This regular-sequence monitor follows the NFA representation used by Yosys'
// Verific SVA frontend.  Keeping the representation here, instead of expanding
// unbounded ranges, is what makes unbounded SVA synthesis finite and exact.
//
// The graph has two kinds of arcs:
//   * links are evaluated without consuming a sampled clock tick;
//   * edges transfer active states on the next sampled clock tick.
//
// Copyright (C) 2017-2026 Claire Xenia Wolf and Yosys contributors.
// Adapted for the Slang AST and yosys-slang under the ISC license.
struct SvaNfaNode {
	std::vector<std::pair<int, ir::Net>> edges;
	std::vector<std::pair<int, ir::Net>> links;
};

class SvaSequenceNfa {
	EvalContext& eval;
	ir::Net trigger;
	ir::Net disable = ir::S0;
	ir::Net throughout = ir::S1;
	std::vector<ir::Net> throughout_stack;
	std::vector<SvaNfaNode> nodes;
	bool materialized = false;

	void make_link_order(std::vector<int>& order, int node, int minimum) const {
		if (order[node] >= minimum)
			return;
		order[node] = minimum;
		for (const auto& link : nodes[node].links)
			make_link_order(order, link.first, minimum + 1);
	}

public:
	const int start_node;
	const int accept_node;

	SvaSequenceNfa(EvalContext& eval_, ir::Net trigger_, ir::Net disable_):
		eval(eval_), trigger(trigger_), disable(disable_),
		start_node(create_node()), accept_node(create_node()) {}

	int create_node(int link_from = -1) {
		assert_invariant(!materialized);
		int index = int(nodes.size());
		nodes.push_back({});
		if (link_from >= 0)
			create_link(link_from, index);
		return index;
	}

	void create_edge(int from, int to, ir::Net control = ir::S1) {
		assert_invariant(!materialized);
		assert_invariant(0 <= from && from < int(nodes.size()));
		assert_invariant(0 <= to && to < int(nodes.size()));
		assert_invariant(to != start_node);
		control = eval.netlist.LogicAnd(control, throughout);
		nodes[from].edges.emplace_back(to, control);
	}

	void create_link(int from, int to, ir::Net control = ir::S1) {
		assert_invariant(!materialized);
		assert_invariant(0 <= from && from < int(nodes.size()));
		assert_invariant(0 <= to && to < int(nodes.size()));
		assert_invariant(to != start_node);
		control = eval.netlist.LogicAnd(control, throughout);
		nodes[from].links.emplace_back(to, control);
	}

	void push_throughout(ir::Net condition) {
		throughout_stack.push_back(throughout);
		throughout = eval.netlist.LogicAnd(throughout, condition);
	}

	void pop_throughout() {
		assert_invariant(!throughout_stack.empty());
		throughout = throughout_stack.back();
		throughout_stack.pop_back();
	}

	ir::Net materialize_accept() {
		assert_invariant(!materialized);
		materialized = true;

		std::vector<ir::Net> state_wires(nodes.size());
		std::vector<ir::Net> states(nodes.size());
		std::vector<ir::Net> next_states(nodes.size(), ir::S0);
		ir::Net enabled = sva_not(eval, disable);

		for (int i = 0; i < int(nodes.size()); i++) {
			state_wires[i] = add_sva_state(eval, "sva_nfa_state");
			states[i] = state_wires[i];
			if (i == start_node)
				states[i] = eval.netlist.LogicOr(states[i], trigger);
			states[i] = eval.netlist.LogicAnd(states[i], enabled);
		}

		std::vector<int> order(nodes.size(), -1);
		for (int i = 0; i < int(nodes.size()); i++)
			make_link_order(order, i, 0);
		std::vector<std::vector<int>> ordered_nodes;
		for (int i = 0; i < int(nodes.size()); i++) {
			if (order[i] >= int(ordered_nodes.size()))
				ordered_nodes.resize(order[i] + 1);
			ordered_nodes[order[i]].push_back(i);
		}

		for (const auto& level : ordered_nodes) {
			for (int node : level) {
				for (const auto& link : nodes[node].links) {
					ir::Net active = eval.netlist.LogicAnd(states[node], link.second);
					states[link.first] = eval.netlist.LogicOr(states[link.first], active);
				}
			}
		}

		std::vector<ir::Value> activations(nodes.size());
		for (int i = 0; i < int(nodes.size()); i++) {
			for (const auto& edge : nodes[i].edges)
				activations[edge.first].append(eval.netlist.LogicAnd(states[i], edge.second));
		}
		for (int i = 0; i < int(nodes.size()); i++) {
			if (activations[i].size() == 1)
				next_states[i] = activations[i].as_net();
			else if (!activations[i].empty())
				next_states[i] = eval.netlist.ReduceBool(activations[i]);
		}

		for (int i = 0; i < int(nodes.size()); i++) {
			if (next_states[i] == ir::S0)
				eval.netlist.connect(state_wires[i], ir::S0);
			else
				add_sva_flop(eval, next_states[i], state_wires[i], "sva_nfa_state");
		}

		return states[accept_node];
	}
};

class SvaSequenceBuilder {
public:
	// `end` is active on the last tick of a non-empty match. An empty match has
	// no such tick, so it is flagged here and resolved by the enclosing concat.
	struct Fragment {
		int end;
		bool empty;
	};

private:
	EvalContext& eval;
	SvaSequenceNfa& fsm;
	bool valid = true;

	void unsupported(const ast::AssertionExpr& expr) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		valid = false;
	}

	ir::Net expression_bit(const ast::Expression& expr) {
		return eval.netlist.ReduceBool(eval.sva(expr));
	}

	int tick(int node) {
		int next = fsm.create_node();
		fsm.create_edge(node, next);
		return next;
	}

	bool check_range(const ast::SequenceRange& range, const ast::AssertionExpr& owner) {
		if (range.min > SVA_ENUMERATION_LIMIT ||
			(range.max.has_value() && range.max.value() > SVA_ENUMERATION_LIMIT)) {
			unsupported(owner);
			return false;
		}
		return true;
	}

	// The delays of `range` which are at least `ticks` long, shortened by `ticks`
	static std::optional<ast::SequenceRange> shorten(const ast::SequenceRange& range,
													 uint32_t ticks) {
		if (range.max.has_value() && range.max.value() < ticks)
			return std::nullopt;
		ast::SequenceRange shortened;
		shortened.min = range.min > ticks ? range.min - ticks : 0;
		if (range.max.has_value())
			shortened.max = range.max.value() - ticks;
		return shortened;
	}

	// Activate `to` on each tick of `range` counted from `from`
	void add_delay(int from, const ast::SequenceRange& range, int to) {
		int node = fsm.create_node(from);
		for (uint32_t i = 0; i < range.min; i++)
			node = tick(node);
		fsm.create_link(node, to);

		if (!range.max.has_value()) {
			fsm.create_edge(node, node);
			return;
		}
		for (uint32_t i = range.min; i < range.max.value(); i++) {
			node = tick(node);
			fsm.create_link(node, to);
		}
	}

	// `##[delay] expr`, where `##n empty` is `##(n-1) `true`
	Fragment parse_delayed(int start, const ast::SequenceRange& delay,
						   const ast::AssertionExpr& expr) {
		int right_start = fsm.create_node();
		add_delay(start, delay, right_start);

		Fragment right = parse(right_start, expr);
		Fragment result {right.end, false};
		if (right.empty) {
			if (auto shorter = shorten(delay, 1))
				add_delay(start, *shorter, right.end);
			result.empty = delay.min == 0;
		}
		return result;
	}

	// Append `##[delay] right` to `left`, which began on `start`. An empty match
	// is no tick for a delay to count from, so IEEE 1800-2017 16.9.2.1 gives it
	// separate rules:
	//   empty ##0 seq, seq ##0 empty    no match
	//   empty ##n seq                   ##(n-1) seq
	//   seq ##n empty                   seq ##(n-1) `true
	template <typename ParseRight>
	Fragment concat(int start, Fragment left, const ast::SequenceRange& delay,
					ParseRight parse_right) {
		auto shorter = shorten(delay, 1);

		int right_start = fsm.create_node();
		add_delay(left.end, delay, right_start);
		if (left.empty && shorter.has_value())
			add_delay(start, *shorter, right_start);

		Fragment right = parse_right(right_start);
		Fragment result {right.end, false};
		if (right.empty) {
			if (shorter.has_value())
				add_delay(left.end, *shorter, right.end);
			if (left.empty) {
				// `empty ##n empty` spans n - 1 ticks
				result.empty = shorter.has_value() && shorter->min == 0;
				if (auto shortest = shorten(delay, 2))
					add_delay(start, *shortest, right.end);
			}
		}
		return result;
	}

	// Repetitions are chained with ##1, which is transparent to an empty match:
	// an iteration matching empty just doesn't add to the length of the chain
	template <typename ParseUnit>
	Fragment parse_repeat(int start, const ast::SequenceRange& range,
						  const ast::AssertionExpr& owner, ParseUnit parse_unit) {
		if (!check_range(range, owner))
			return {start, false};

		Fragment result {fsm.create_node(), range.min == 0};
		if (range.max == uint32_t(0))
			return result;

		uint32_t units = range.max.value_or(std::max(range.min, uint32_t(1)));
		int unit_start = -1;
		int unit_end = -1;
		for (uint32_t i = 0; i < units && valid; i++) {
			unit_start = i == 0 ? fsm.create_node(start) : tick(unit_end);
			Fragment unit = parse_unit(unit_start);
			unit_end = unit.end;
			result.empty |= unit.empty;
			if (i + 1 >= range.min || unit.empty)
				fsm.create_link(unit.end, result.end);
		}
		if (valid && !range.max.has_value())
			fsm.create_edge(unit_end, unit_start);
		return result;
	}

	bool simple_condition(const ast::AssertionExpr& expr, ir::Net& condition) {
		const ast::AssertionExpr *current = &expr;
		if (current->kind == ast::AssertionExprKind::SequenceWithMatch) {
			const auto& with_match = current->as<ast::SequenceWithMatchExpr>();
			if (!with_match.matchItems.empty() || with_match.repetition.has_value())
				return false;
			current = &with_match.expr;
		}
		if (current->kind != ast::AssertionExprKind::Simple)
			return false;
		const auto& simple = current->as<ast::SimpleAssertionExpr>();
		if (simple.repetition.has_value() ||
			simple.expr.kind == ast::ExpressionKind::AssertionInstance)
			return false;
		condition = simple.isNullExpr ? ir::S0 : expression_bit(simple.expr);
		return true;
	}

	Fragment parse_nonconsecutive(int start, const ast::AssertionExpr& body,
								  const ast::SequenceRepetition& repetition,
								  const ast::AssertionExpr& owner) {
		ir::Net condition;
		if (!simple_condition(body, condition)) {
			unsupported(owner);
			return {start, false};
		}
		ir::Net not_condition = sva_not(eval, condition);

		// `b[->n]` is `(!b[*0:$] ##1 b)[*n]`
		Fragment result = parse_repeat(start, repetition.range, owner, [&](int node) {
			int wait = fsm.create_node(node);
			fsm.create_edge(wait, wait, not_condition);
			int hit = fsm.create_node();
			fsm.create_link(wait, hit, condition);
			return Fragment {hit, false};
		});
		if (repetition.kind == ast::SequenceRepetition::GoTo)
			return result;

		// `b[=n]` is `b[->n] ##1 !b[*0:$]`
		return concat(start, result, {1, 1}, [&](int node) {
			int wait = fsm.create_node(node);
			int quiet = fsm.create_node();
			fsm.create_link(wait, quiet, not_condition);
			fsm.create_edge(quiet, wait);
			return Fragment {quiet, true};
		});
	}

	Fragment apply_repetition(int start, const ast::AssertionExpr& body,
							  const std::optional<ast::SequenceRepetition>& repetition,
							  const ast::AssertionExpr& owner) {
		if (!repetition.has_value())
			return parse(start, body);
		if (repetition->kind != ast::SequenceRepetition::Consecutive)
			return parse_nonconsecutive(start, body, *repetition, owner);
		return parse_repeat(start, repetition->range, owner,
							[&](int node) { return parse(node, body); });
	}

public:
	SvaSequenceBuilder(EvalContext& eval_, SvaSequenceNfa& fsm_):
		eval(eval_), fsm(fsm_) {}

	bool good() const {
		return valid;
	}

	Fragment parse(int start, const ast::AssertionExpr& expr) {
		if (!valid)
			return {start, false};

		switch (expr.kind) {
		case ast::AssertionExprKind::Simple:
			{
				const auto& simple = expr.as<ast::SimpleAssertionExpr>();
				if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
					const auto& instance =
						simple.expr.as<ast::AssertionInstanceExpression>();
					if (instance.isRecursiveProperty) {
						unsupported(expr);
						return {start, false};
					}
					return apply_repetition(start, instance.body,
										   simple.repetition, expr);
				}
				if (simple.repetition.has_value()) {
					ast::SimpleAssertionExpr body(simple.expr, std::nullopt,
												 simple.isNullExpr);
					body.syntax = expr.syntax;
					return apply_repetition(start, body, simple.repetition, expr);
				}
				int node = fsm.create_node();
				fsm.create_link(start, node,
					simple.isNullExpr ? ir::S0 :
					expression_bit(simple.expr));
				return {node, false};
			}

		case ast::AssertionExprKind::SequenceConcat:
			{
				const auto& sequence = expr.as<ast::SequenceConcatExpr>();
				Fragment result {start, false};
				for (size_t index = 0; index < sequence.elements.size(); index++) {
					const auto& element = sequence.elements[index];
					if (!check_range(element.delay, expr))
						return {start, false};
					if (index == 0)
						result = parse_delayed(start, element.delay, *element.sequence);
					else
						result = concat(start, result, element.delay, [&](int node) {
							return parse(node, *element.sequence);
						});
				}
				return result;
			}

		case ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, expr_loc(expr));
					valid = false;
					return {start, false};
				}
				return apply_repetition(start, with_match.expr,
									   with_match.repetition, expr);
			}

		case ast::AssertionExprKind::FirstMatch:
			// The NFA merges overlapping attempts, leaving it no way to tell
			// which match came first in a given attempt
			unsupported(expr);
			return {start, false};

		case ast::AssertionExprKind::Unary:
			{
				const auto& unary = expr.as<ast::UnaryAssertionExpr>();
				switch (unary.op) {
				case ast::UnaryAssertionOperator::NextTime:
				case ast::UnaryAssertionOperator::SNextTime:
					{
						uint32_t delay = 1;
						if (unary.range.has_value()) {
							if (!unary.range->max.has_value() ||
								unary.range->min != unary.range->max.value()) {
								unsupported(expr);
								return {start, false};
							}
							delay = unary.range->min;
						}
						ast::SequenceRange range {delay, delay};
						if (!check_range(range, expr))
							return {start, false};
						return parse_delayed(start, range, unary.expr);
					}
				default:
					unsupported(expr);
					return {start, false};
				}
			}

		case ast::AssertionExprKind::Binary:
			{
				const auto& binary = expr.as<ast::BinaryAssertionExpr>();
				switch (binary.op) {
				case ast::BinaryAssertionOperator::Or:
					{
						int end = fsm.create_node();
						Fragment left = parse(start, binary.left);
						fsm.create_link(left.end, end);
						Fragment right = parse(start, binary.right);
						fsm.create_link(right.end, end);
						return {end, left.empty || right.empty};
					}
				case ast::BinaryAssertionOperator::Throughout:
					{
						ir::Net condition;
						if (!simple_condition(binary.left, condition)) {
							unsupported(expr);
							return {start, false};
						}
						fsm.push_throughout(condition);
						Fragment result = parse(start, binary.right);
						fsm.pop_throughout();
						return result;
					}
				case ast::BinaryAssertionOperator::And:
				case ast::BinaryAssertionOperator::Intersect:
				case ast::BinaryAssertionOperator::Within:
					// These regular-language products need determinization.
					// The finite path lowering handles bounded instances.
					unsupported(expr);
					return {start, false};
				default:
					unsupported(expr);
					return {start, false};
				}
			}

		case ast::AssertionExprKind::Clocking:
		case ast::AssertionExprKind::StrongWeak:
		case ast::AssertionExprKind::Abort:
		case ast::AssertionExprKind::Conditional:
		case ast::AssertionExprKind::Case:
		case ast::AssertionExprKind::DisableIff:
		case ast::AssertionExprKind::Invalid:
			unsupported(expr);
			return {start, false};
		}
		assert_invariant(false && "unreachable");
	}
};

// An empty match has no end tick to be accepted on; `admits_empty` reports it
static std::optional<ir::Net>
synthesize_unbounded_sequence(EvalContext& eval, const ast::AssertionExpr& sequence,
							  ir::Net trigger, ir::Net disable,
							  bool *admits_empty = nullptr) {
	if (!sva_clock_ready(eval, expr_loc(sequence)))
		return std::nullopt;

	SvaSequenceNfa fsm(eval, trigger, disable);
	SvaSequenceBuilder builder(eval, fsm);
	auto match = builder.parse(fsm.create_node(fsm.start_node), sequence);
	if (!builder.good())
		return std::nullopt;
	if (admits_empty)
		*admits_empty = match.empty;
	fsm.create_link(match.end, fsm.accept_node);
	return fsm.materialize_accept();
}

struct AssertionMatch {
	EvalContext& eval;
	ir::Value sig;
	ir::Value en;
	int start;
	bool empty;
	slang::SourceLocation loc;

	AssertionMatch(EvalContext& eval_, ir::Value sig_,
				   slang::SourceLocation loc_ = slang::SourceLocation::NoLocation):
		eval(eval_), sig(sig_), en(ir::S1), start(0), empty(false), loc(loc_) {}
	AssertionMatch(const AssertionMatch&) = default;

private:
	AssertionMatch(EvalContext& eval_, ir::Value sig_, ir::Value en_, int start_, bool empty_,
				   slang::SourceLocation loc_):
		eval(eval_), sig(sig_), en(en_), start(start_), empty(empty_), loc(loc_) {}

public:
	static AssertionMatch Empty(EvalContext& eval, slang::SourceLocation loc) {
		return { eval, ir::S1, ir::S1, 0, true, loc };
	}

	AssertionMatch& operator=(const AssertionMatch& other) {
		sig = other.sig;
		en = other.en;
		start = other.start;
		empty = other.empty;
		loc = other.loc;
		return *this;
	}

	AssertionMatch shift(int time) const {
		if (empty)
			return { eval, sig, en, start + time, true, loc };

		ir::Value shifted_sig = sig;
		ir::Value shifted_en = en;
		if (!sig.is_fully_const())
			shifted_sig = delay_sva_sample(eval, sig, time, ir::Sx, loc);
		if (!en.is_fully_const())
			shifted_en = delay_sva_sample(eval, en, time, ir::S0, loc);
		return { eval, shifted_sig, shifted_en, start + time, false, loc };
	}

	AssertionMatch operator||(const AssertionMatch& other) const {
		assert_invariant(!empty && !other.empty);

		ir::Value gated = en.is_fully_const() && en.as_bool() ? sig : ir::Value(eval.netlist.LogicAnd(en, sig));
		ir::Value other_gated = other.en.is_fully_const() && other.en.as_bool()
									? other.sig
									: ir::Value(eval.netlist.LogicAnd(other.en, other.sig));
		ir::Value result_sig = eval.netlist.LogicOr(gated, other_gated);
		ir::Value result_en = eval.netlist.LogicOr(en, other.en);
		return { eval, result_sig, result_en, std::max(other.start, start), false, loc };
	}

	AssertionMatch operator&&(const AssertionMatch& other) const {
		assert_invariant(!empty && !other.empty);

		ir::Value result_en = eval.netlist.LogicAnd(en, other.en);
		if (sig.is_fully_const() && sig.as_bool())
			return { eval, other.sig, result_en, std::max(other.start, start), false, loc };
		if (sig.is_fully_const() && !sig.as_bool())
			return { eval, ir::S0, result_en, std::max(other.start, start), false, loc };
		if (other.sig.is_fully_const() && other.sig.as_bool())
			return { eval, sig, result_en, std::max(other.start, start), false, loc };
		if (other.sig.is_fully_const() && !other.sig.as_bool())
			return { eval, ir::S0, result_en, std::max(other.start, start), false, loc };
		return { eval, eval.netlist.LogicAnd(sig, other.sig), result_en,
				 std::max(other.start, start), false, loc };
	}

	AssertionMatch operator!() const {
		assert_invariant(!empty);

		if (sig.is_fully_const()) return { eval, sig.as_bool() ? ir::S0 : ir::S1, en, start, false, loc };
		return { eval, eval.netlist.LogicNot(sig), en, start, false, loc };
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
	int time = -1;
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

	for (auto path : paths) {
		if (path.empty)
			return { path.eval, ir::S1, path.loc };
	}

	int max_start = 0;
	for (auto path : paths)
		max_start = std::max(max_start, path.start);

	std::optional<AssertionMatch> collapsed;
	for (auto path : paths) {
		if (path.start < max_start)
			path = path.shift(max_start - path.start);

		if (collapsed.has_value())
			collapsed = collapsed.value() || path;
		else
			collapsed = path;
	}
	return collapsed.value();
}

static std::vector<AssertionMatch> not_vec(EvalContext& eval, std::vector<AssertionMatch> in,
										   slang::SourceLocation loc) {
	// A sequence with no possible match is false on every attempt
	if (in.empty()) return {{eval, ir::S1, loc}};
	return { !collapse_or(in) };
}

static ir::Value gated_signal(const AssertionMatch& path) {
	assert_invariant(!path.empty);

	if (path.sig.is_fully_const())
		return path.sig.as_bool() ? path.en : ir::Value(ir::S0);
	return path.eval.netlist.LogicAnd(path.en, path.sig);
}

static bool check_finite_range(EvalContext& eval, const ast::AssertionExpr& expr,
							   uint32_t min, uint32_t max) {
	if (max < min || max > SVA_ENUMERATION_LIMIT) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		return false;
	}
	return true;
}

static std::vector<AssertionMatch> seq_vec(std::vector<AssertionMatch> a, int min, int max, std::vector<AssertionMatch> b) {
	std::vector<AssertionMatch> new_own_paths;
	for (auto path : a) {
		for (int offset = min; offset <= max; offset++) {
			for (auto inner : b) {
				if (path.empty && inner.empty) {
					// `empty ##N empty` spans N - 1 ticks
					if (offset == 1)
						new_own_paths.push_back(path);
					else if (offset > 1)
						new_own_paths.push_back(AssertionMatch(path.eval, ir::S1, path.loc).shift(offset - 2));
					continue;
				}
				if (path.empty) {
					// `empty ##N seq` is `##(N-1) seq`
					if (offset > 0)
						new_own_paths.push_back(AssertionMatch(path.eval, ir::S1, path.loc).shift(offset - 1 + inner.start) && inner);
					continue;
				}
				if (inner.empty) {
					// `seq ##N empty` is `seq ##(N-1) 1'b1`
					if (offset > 0)
						new_own_paths.push_back(path.shift(offset - 1));
					continue;
				}
				new_own_paths.push_back(path.shift(offset + inner.start) && inner);
			}
		}
	}
	return compress_paths(new_own_paths);
}

static std::vector<AssertionMatch> seq_prefix_vec(EvalContext& eval, int min, int max,
												  std::vector<AssertionMatch> b,
												  slang::SourceLocation loc) {
	std::vector<AssertionMatch> paths;
	for (int offset = min; offset <= max; offset++) {
		for (auto inner : b) {
			if (inner.empty) {
				// `##N empty` is `##(N-1) 1'b1`
				if (offset > 0)
					paths.push_back(AssertionMatch(eval, ir::S1, loc).shift(offset - 1));
				else
					paths.push_back(inner);
			} else {
				paths.push_back(AssertionMatch(eval, ir::S1, loc).shift(offset + inner.start) && inner);
			}
		}
	}
	return compress_paths(paths);
}

static std::vector<AssertionMatch> repeat_count(std::vector<AssertionMatch> paths, int count) {
	if (count == 0)
		return {};

	std::vector<AssertionMatch> result = paths;
	for (int i = 1; i < count; i++)
		result = seq_vec(result, 1, 1, paths);
	return result;
}

static bool apply_repetition(EvalContext& eval, const ast::AssertionExpr& expr,
							 std::optional<ast::SequenceRepetition> repetition,
							 std::vector<AssertionMatch>& paths) {
	if (!repetition.has_value())
		return true;

	if (repetition->kind != ast::SequenceRepetition::Consecutive || !repetition->range.max.has_value()) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		return false;
	}
	if (!check_finite_range(eval, expr, repetition->range.min, repetition->range.max.value()))
		return false;

	std::vector<AssertionMatch> repeated;
	for (uint32_t count = repetition->range.min; count <= repetition->range.max.value(); count++) {
		if (count == 0)
			repeated.push_back(AssertionMatch::Empty(eval, expr_loc(expr)));
		else {
			auto count_paths = repeat_count(paths, (int)count);
			repeated.insert(repeated.end(), count_paths.begin(), count_paths.end());
		}
	}

	paths = compress_paths(repeated);
	return true;
}

static std::vector<AssertionMatch> synthesize_disable_iff(EvalContext& eval, const ast::Expression& condition,
														 std::vector<AssertionMatch> inner,
														 slang::SourceLocation loc) {
	auto disable = AssertionMatch(eval, eval(condition), loc);
	if (inner.empty())
		inner.push_back(AssertionMatch(eval, ir::S0, loc));

	int max_start = 0;
	for (auto path : inner)
		max_start = std::max(max_start, path.start);

	std::vector<AssertionMatch> disables;
	disables.push_back(disable);
	std::vector<AssertionMatch> disable_windows;
	for (int i = 0; i <= max_start; i++) {
		std::optional<AssertionMatch> this_disables = {};
		for (int t = 0; t <= i; t++) {
			while ((int)disables.size() <= t)
				disables.push_back(disables[disables.size() - 1].shift(1));

			if (this_disables.has_value())
				this_disables = this_disables.value() || disables[t];
			else
				this_disables = disables[t];
		}
		disable_windows.push_back(this_disables.value());
	}

	std::vector<AssertionMatch> results;
	for (auto path : inner) {
		auto not_disabled = !disable_windows[path.start];
		auto success = AssertionMatch(eval, ir::S1, path.loc);
		success.en = eval.netlist.LogicAnd(gated_signal(path), not_disabled.sig);
		success.start = path.start;
		results.push_back(success);
	}

	auto base = collapse_or(inner);
	auto not_disabled_to_end = !disable_windows[max_start];
	auto check_failure = AssertionMatch(eval, ir::S0, loc);
	check_failure.en = eval.netlist.LogicAnd(base.en, not_disabled_to_end.sig);
	check_failure.start = max_start;
	results.push_back(check_failure);

	return compress_paths(results);
}

// An empty result stands for a sequence with no possible match. Constructs we
// can't lower also yield an empty result, but those have raised an error
// diagnostic so the design won't be read in.
static std::vector<AssertionMatch> synthesize_assertion_expr(EvalContext& eval, const ast::AssertionExpr& expr) {
	switch (expr.kind) {
		case slang::ast::AssertionExprKind::Invalid:
			assert_invariant(false && "unreachable");
		case slang::ast::AssertionExprKind::Simple:
			{
				const auto& simple = expr.as<ast::SimpleAssertionExpr>();
				std::vector<AssertionMatch> paths;
				if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
					const auto& instance = simple.expr.as<ast::AssertionInstanceExpression>();
					if (instance.isRecursiveProperty) {
						eval.netlist.add_diag(diag::AssertionUnsupported, simple.expr.sourceRange);
						return {};
					}
					paths = synthesize_assertion_expr(eval, instance.body);
				} else {
					paths = {{ eval, simple.isNullExpr ? ir::Value(ir::S0) : eval.sva(simple.expr), expr_loc(expr) }};
				}
				if (!apply_repetition(eval, expr, simple.repetition, paths))
					return {};
				return paths;
			}
		case slang::ast::AssertionExprKind::SequenceConcat:
			{
				const auto& sequence = expr.as<ast::SequenceConcatExpr>();
				std::vector<AssertionMatch> own_paths;
				for (size_t i = 0; i < sequence.elements.size(); i++) {
					auto inner_paths = synthesize_assertion_expr(eval, *sequence.elements[i].sequence);

					auto delay = sequence.elements[i].delay;
					if (!delay.max.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
						return {};
					}
					if (!check_finite_range(eval, expr, delay.min, delay.max.value()))
						return {};

					if (i == 0)
						own_paths = seq_prefix_vec(eval, delay.min, delay.max.value(), inner_paths, expr_loc(expr));
					else
						own_paths = seq_vec(own_paths, delay.min, delay.max.value(), inner_paths);
				}
				return own_paths;
			}
		case slang::ast::AssertionExprKind::Unary:
			{
				const auto& uop = expr.as<ast::UnaryAssertionExpr>();
				switch (uop.op) {
				case slang::ast::UnaryAssertionOperator::Not:
					return not_vec(eval, synthesize_assertion_expr(eval, uop.expr), expr_loc(expr));

				case slang::ast::UnaryAssertionOperator::Eventually:
				case slang::ast::UnaryAssertionOperator::SEventually:
				{
					if (!uop.range.has_value() || !uop.range->max.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
						return {};
					}
					if (!check_finite_range(eval, expr, uop.range->min, uop.range->max.value()))
						return {};

					std::vector<AssertionMatch> true_path = {{eval, ir::S1, expr_loc(expr)}};
					return seq_vec(true_path, uop.range->min, uop.range->max.value(),
								   synthesize_assertion_expr(eval, uop.expr));
				}

				case slang::ast::UnaryAssertionOperator::NextTime:
				case slang::ast::UnaryAssertionOperator::SNextTime:
				{
					uint32_t delay = 1;
					if (uop.range.has_value()) {
						if (!uop.range->max.has_value() || uop.range->min != uop.range->max.value()) {
							eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
							return {};
						}
						if (!check_finite_range(eval, expr, uop.range->min, uop.range->max.value()))
							return {};
						delay = uop.range->min;
					}

					std::vector<AssertionMatch> true_path = {{eval, ir::S1, expr_loc(expr)}};
					return seq_vec(true_path, delay, delay, synthesize_assertion_expr(eval, uop.expr));
				}

				case slang::ast::UnaryAssertionOperator::Always:
				case slang::ast::UnaryAssertionOperator::SAlways:
					eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
					return {};
				}
				assert_invariant(false && "unreachable");
			}
		case slang::ast::AssertionExprKind::Binary:
			{
				const auto& biop = expr.as<ast::BinaryAssertionExpr>();
				auto left = synthesize_assertion_expr(eval, biop.left);
				auto right = synthesize_assertion_expr(eval, biop.right);

				switch (biop.op) {
				case ast::BinaryAssertionOperator::And:
				{
					/*
					When te1 and te2 are sequences, then the composite sequence te1 and te2 matches if te1 and te2 match.
					The end time is the end time of either te1 or te2, whichever matches last.
					*/
					std::vector<AssertionMatch> results;
					for (auto a : left) {
						for (auto b : right) {
							// An empty match puts no constraint on the other operand
							if (a.empty)
								results.push_back(b);
							else if (b.empty)
								results.push_back(a);
							else if (a.start >= b.start)
								results.push_back(b.shift(a.start - b.start) && a);
							else
								results.push_back(a.shift(b.start - a.start) && b);
						}
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
						eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
						return {};
					}

					std::vector<AssertionMatch> results;
					for (auto path : right) {
						if (path.empty) {
							results.push_back(path);
							continue;
						}

						AssertionMatch guarded = path;
						for (int t = 0; t <= path.start; t++) {
							auto condition_at_t = condition.shift(t);
							guarded = guarded && condition_at_t;
						}
						results.push_back(guarded);
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
					return not_vec(eval, seq_vec(left, 0, 0, not_vec(eval, right, expr_loc(expr))), expr_loc(expr));
				case ast::BinaryAssertionOperator::NonOverlappedImplication:
					return not_vec(eval, seq_vec(left, 1, 1, not_vec(eval, right, expr_loc(expr))), expr_loc(expr));

				case ast::BinaryAssertionOperator::OverlappedFollowedBy:
					return seq_vec(left, 0, 0, right);
				case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
					return seq_vec(left, 1, 1, right);

				case ast::BinaryAssertionOperator::Within:
				case ast::BinaryAssertionOperator::Iff:
				case ast::BinaryAssertionOperator::Until:
				case ast::BinaryAssertionOperator::SUntil:
				case ast::BinaryAssertionOperator::UntilWith:
				case ast::BinaryAssertionOperator::SUntilWith:
				case ast::BinaryAssertionOperator::Implies:
					eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
					return {};
				}
				assert_invariant(false && "unreachable");
			}
		case slang::ast::AssertionExprKind::Clocking:
			{
				eval.netlist.add_diag(diag::UnsupportedSVAFeature, expr_loc(expr));
				return {};
			}
		case slang::ast::AssertionExprKind::DisableIff:
			{
				const auto& disableiff = expr.as<ast::DisableIffAssertionExpr>();
				return synthesize_disable_iff(eval, disableiff.condition,
											  synthesize_assertion_expr(eval, disableiff.expr), expr_loc(expr));
			}

		case slang::ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, expr_loc(expr));
					return {};
				}

				auto paths = synthesize_assertion_expr(eval, with_match.expr);
				if (!apply_repetition(eval, expr, with_match.repetition, paths))
					return {};
				return paths;
			}
		case slang::ast::AssertionExprKind::FirstMatch:
			{
				const auto& first_match = expr.as<ast::FirstMatchAssertionExpr>();
				if (!first_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, expr_loc(expr));
					return {};
				}

				auto paths = compress_paths(synthesize_assertion_expr(eval, first_match.seq));
				std::vector<AssertionMatch> results;

				// compress_paths() keeps any empty match as the last path
				if (!paths.empty() && paths.back().empty)
					return { paths.back() };

				for (auto path : paths) {
					std::vector<AssertionMatch> earlier_matches;
					for (auto earlier : paths) {
						if (earlier.start >= path.start)
							continue;
						earlier_matches.push_back(earlier.shift(path.start - earlier.start));
					}

					if (!earlier_matches.empty()) {
						auto no_earlier_match = !collapse_or(earlier_matches);
						path = path && no_earlier_match;
					}

					results.push_back(path);
				}

				return compress_paths(results);
			}
		case slang::ast::AssertionExprKind::StrongWeak:
		case slang::ast::AssertionExprKind::Abort:
		case slang::ast::AssertionExprKind::Conditional:
		case slang::ast::AssertionExprKind::Case:
			eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return {};
	}
	assert_invariant(false && "unreachable");
}

enum class PropertyObligationKind {
	Safety,
	Liveness,
	AuxiliaryFairness
};

struct PropertyObligation {
	PropertyObligationKind kind;
	ir::Value a;
	ir::Value en;
	ir::Value active;
};

struct PropertyResult {
	std::vector<PropertyObligation> obligations;
	bool valid = true;
};

struct EventualCandidate {
	ir::Net match;
	uint32_t duration;
};

static void add_liveness_obligation(PropertyResult& result, EvalContext& eval,
									ir::Net goal, ir::Net trigger,
									ir::Net disable);
static ir::Net add_pending_monitor(
		EvalContext& eval, ir::Net trigger,
		ir::Net completion, ir::Net disable,
		slang::SourceLocation loc);

static bool assertion_has_unbounded_sequence(const ast::AssertionExpr& expr) {
	switch (expr.kind) {
	case ast::AssertionExprKind::Simple:
		{
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.repetition.has_value() &&
				(simple.repetition->kind !=
					 ast::SequenceRepetition::Consecutive ||
				 !simple.repetition->range.max.has_value()))
				return true;
			if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
				const auto& instance =
					simple.expr.as<ast::AssertionInstanceExpression>();
				return !instance.isRecursiveProperty &&
					   assertion_has_unbounded_sequence(instance.body);
			}
			return false;
		}
	case ast::AssertionExprKind::SequenceConcat:
		for (const auto& element : expr.as<ast::SequenceConcatExpr>().elements) {
			if (!element.delay.max.has_value() ||
				assertion_has_unbounded_sequence(*element.sequence))
				return true;
		}
		return false;
	case ast::AssertionExprKind::SequenceWithMatch:
		{
			const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
			return (with_match.repetition.has_value() &&
					(with_match.repetition->kind !=
						 ast::SequenceRepetition::Consecutive ||
					 !with_match.repetition->range.max.has_value())) ||
				   assertion_has_unbounded_sequence(with_match.expr);
		}
	case ast::AssertionExprKind::FirstMatch:
		return assertion_has_unbounded_sequence(
			expr.as<ast::FirstMatchAssertionExpr>().seq);
	case ast::AssertionExprKind::Unary:
		{
			const auto& unary = expr.as<ast::UnaryAssertionExpr>();
			if ((unary.op == ast::UnaryAssertionOperator::Eventually ||
				 unary.op == ast::UnaryAssertionOperator::SEventually) &&
				(!unary.range.has_value() ||
				 !unary.range->max.has_value()))
				return true;
			return assertion_has_unbounded_sequence(unary.expr);
		}
	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = expr.as<ast::BinaryAssertionExpr>();
			return assertion_has_unbounded_sequence(binary.left) ||
				   assertion_has_unbounded_sequence(binary.right);
		}
	case ast::AssertionExprKind::Clocking:
		return assertion_has_unbounded_sequence(
			expr.as<ast::ClockingAssertionExpr>().expr);
	case ast::AssertionExprKind::StrongWeak:
		return assertion_has_unbounded_sequence(
			expr.as<ast::StrongWeakAssertionExpr>().expr);
	case ast::AssertionExprKind::Abort:
		return assertion_has_unbounded_sequence(
			expr.as<ast::AbortAssertionExpr>().expr);
	case ast::AssertionExprKind::Conditional:
		{
			const auto& conditional =
				expr.as<ast::ConditionalAssertionExpr>();
			return assertion_has_unbounded_sequence(conditional.ifExpr) ||
				   (conditional.elseExpr &&
					assertion_has_unbounded_sequence(*conditional.elseExpr));
		}
	case ast::AssertionExprKind::Case:
		{
			const auto& case_expr = expr.as<ast::CaseAssertionExpr>();
			for (const auto& item : case_expr.items)
				if (assertion_has_unbounded_sequence(*item.body))
					return true;
			return case_expr.defaultCase &&
				   assertion_has_unbounded_sequence(*case_expr.defaultCase);
		}
	case ast::AssertionExprKind::DisableIff:
		return assertion_has_unbounded_sequence(
			expr.as<ast::DisableIffAssertionExpr>().expr);
	case ast::AssertionExprKind::Invalid:
		return false;
	}
	assert_invariant(false && "unreachable");
}

// A suffix-eventual sequence has a finite suffix preceded by one unbounded
// delay.  Its completion event can be shared by all pending triggers, provided
// the finite suffix duration is tracked.  More general unbounded regular
// expressions use per-attempt prophecy selection in asserted properties, and
// must not accidentally take this shared-response fast path.
static bool is_suffix_eventual_sequence(const ast::AssertionExpr& expr) {
	switch (expr.kind) {
	case ast::AssertionExprKind::Simple:
		{
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
				return false;
			const auto& instance =
				simple.expr.as<ast::AssertionInstanceExpression>();
			return !instance.isRecursiveProperty &&
				   is_suffix_eventual_sequence(instance.body);
		}
	case ast::AssertionExprKind::SequenceConcat:
		{
			const auto& concat = expr.as<ast::SequenceConcatExpr>();
			if (concat.elements.empty() ||
				concat.elements[0].delay.max.has_value() ||
				assertion_has_unbounded_sequence(
					*concat.elements[0].sequence))
				return false;
			for (size_t i = 1; i < concat.elements.size(); i++) {
				if (!concat.elements[i].delay.max.has_value() ||
					assertion_has_unbounded_sequence(
						*concat.elements[i].sequence))
					return false;
			}
			return true;
		}
	case ast::AssertionExprKind::FirstMatch:
		{
			const auto& first = expr.as<ast::FirstMatchAssertionExpr>();
			return first.matchItems.empty() &&
				   is_suffix_eventual_sequence(first.seq);
		}
	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = expr.as<ast::BinaryAssertionExpr>();
			return binary.op == ast::BinaryAssertionOperator::Or &&
				   is_suffix_eventual_sequence(binary.left) &&
				   is_suffix_eventual_sequence(binary.right);
		}
	default:
		return false;
	}
}

static bool timing_from_sva_clocking(NetlistContext &netlist, const ast::TimingControl &clocking,
									 ProcessTiming &timing);

// Unwrap non-recursive property instances down to the actual top expression
static const ast::AssertionExpr &unwrap_property_instances(const ast::AssertionExpr &expr)
{
	if (expr.kind != ast::AssertionExprKind::Simple)
		return expr;

	const auto &simple = expr.as<ast::SimpleAssertionExpr>();
	if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
		return expr;

	const auto &instance = simple.expr.as<ast::AssertionInstanceExpression>();
	if (instance.isRecursiveProperty)
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

static ir::Net delayed_abortable_trigger(EvalContext& eval, ir::Net trigger, uint32_t cycles,
										 ir::Net disable, slang::SourceLocation loc) {
	ir::Net not_disable = sva_not(eval, disable);
	ir::Net active = eval.netlist.LogicAnd(trigger, not_disable);
	for (uint32_t i = 0; i < cycles; i++) {
		active = eval.netlist.LogicAnd(active, not_disable);
		active = delay_sva_sample(eval, active, 1, ir::S0, loc, "sva_obligation").as_net();
	}
	return eval.netlist.LogicAnd(active, not_disable);
}

static std::optional<std::vector<EventualCandidate>>
finite_eventual_candidates(EvalContext& eval, const ast::AssertionExpr& sequence,
						   uint32_t leading_delay = 0) {
	if (assertion_has_unbounded_sequence(sequence))
		return std::nullopt;

	auto paths = synthesize_assertion_expr(eval, sequence);

	std::vector<EventualCandidate> candidates;
	for (const auto& path : compress_paths(paths)) {
		if (path.start < 0 ||
			uint64_t(path.start) + leading_delay > SVA_ENUMERATION_LIMIT) {
			eval.netlist.add_diag(diag::AssertionUnsupported, path.loc);
			return std::nullopt;
		}
		ir::Net frame_ready = sva_not(
			eval,
			eval.netlist.ReduceBool(delay_sva_sample(
				eval, ir::S0, path.start,
				ir::S1, path.loc)));
		ir::Net match = eval.netlist.LogicAnd(
			eval.netlist.ReduceBool(path.sig),
			eval.netlist.ReduceBool(path.en));
		match = eval.netlist.LogicAnd(match, frame_ready);
		candidates.push_back(
			{match, uint32_t(path.start) + leading_delay});
	}
	return candidates;
}

static std::optional<std::vector<EventualCandidate>>
suffix_eventual_candidates(EvalContext& eval,
						   const ast::AssertionExpr& sequence,
						   uint32_t additional_delay = 0) {
	if (sequence.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = sequence.as<ast::SimpleAssertionExpr>();
		if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
			return std::nullopt;
		const auto& instance =
			simple.expr.as<ast::AssertionInstanceExpression>();
		if (instance.isRecursiveProperty)
			return std::nullopt;
		return suffix_eventual_candidates(eval, instance.body,
										  additional_delay);
	}

	if (sequence.kind == ast::AssertionExprKind::FirstMatch) {
		const auto& first = sequence.as<ast::FirstMatchAssertionExpr>();
		if (!first.matchItems.empty()) {
			eval.netlist.add_diag(diag::SVAMatchItemsUnsupported,
								  expr_loc(sequence));
			return std::nullopt;
		}
		return suffix_eventual_candidates(eval, first.seq, additional_delay);
	}

	if (sequence.kind == ast::AssertionExprKind::Binary) {
		const auto& binary = sequence.as<ast::BinaryAssertionExpr>();
		if (binary.op != ast::BinaryAssertionOperator::Or)
			return std::nullopt;
		auto left = suffix_eventual_candidates(eval, binary.left,
											 additional_delay);
		auto right = suffix_eventual_candidates(eval, binary.right,
											  additional_delay);
		if (!left.has_value() || !right.has_value())
			return std::nullopt;
		left->insert(left->end(), right->begin(), right->end());
		return left;
	}

	if (sequence.kind != ast::AssertionExprKind::SequenceConcat)
		return std::nullopt;
	const auto& concat = sequence.as<ast::SequenceConcatExpr>();
	if (concat.elements.empty() ||
		concat.elements[0].delay.max.has_value())
		return std::nullopt;
	if (uint64_t(additional_delay) + concat.elements[0].delay.min >
		SVA_ENUMERATION_LIMIT) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(sequence));
		return std::nullopt;
	}

	auto paths = synthesize_assertion_expr(
		eval, *concat.elements[0].sequence);
	for (size_t i = 1; i < concat.elements.size(); i++) {
		const auto& element = concat.elements[i];
		if (!element.delay.max.has_value() ||
			element.delay.min > SVA_ENUMERATION_LIMIT ||
			element.delay.max.value() > SVA_ENUMERATION_LIMIT ||
			assertion_has_unbounded_sequence(*element.sequence))
			return std::nullopt;
		paths = seq_vec(paths, element.delay.min, element.delay.max.value(),
						synthesize_assertion_expr(eval, *element.sequence));
	}

	uint32_t leading_delay =
		additional_delay + concat.elements[0].delay.min;
	std::vector<EventualCandidate> candidates;
	for (const auto& path : compress_paths(paths)) {
		if (path.start < 0 ||
			uint64_t(path.start) + leading_delay > SVA_ENUMERATION_LIMIT) {
			eval.netlist.add_diag(diag::AssertionUnsupported, path.loc);
			return std::nullopt;
		}
		ir::Net frame_ready = sva_not(
			eval,
			eval.netlist.ReduceBool(delay_sva_sample(
				eval, ir::S0, path.start,
				ir::S1, path.loc)));
		ir::Net match = eval.netlist.LogicAnd(
			eval.netlist.ReduceBool(path.sig),
			eval.netlist.ReduceBool(path.en));
		match = eval.netlist.LogicAnd(match, frame_ready);
		candidates.push_back(
			{match, uint32_t(path.start) + leading_delay});
	}
	return candidates;
}

static void add_response_liveness_obligation(
		PropertyResult& result, EvalContext& eval,
		const std::vector<EventualCandidate>& candidates,
		ir::Net trigger, ir::Net disable,
		slang::SourceLocation loc) {
	uint32_t maximum_duration = 0;
	ir::Net any_match = ir::S0;
	for (const auto& candidate : candidates) {
		maximum_duration = std::max(maximum_duration, candidate.duration);
		any_match = eval.netlist.LogicOr(any_match, candidate.match);
	}

	// A zero-duration goal cannot have started before its trigger, so the
	// native $live/$fair event semantics are already exact.
	if (maximum_duration == 0) {
		add_liveness_obligation(result, eval, any_match, trigger, disable);
		return;
	}

	// For a finite suffix of maximum duration N, retain pending triggers of
	// ages 1..N-1 and one saturated "old" bit for ages >= N.  A match of
	// duration D discharges exactly the triggers whose age is at least D.
	// This prevents a suffix that began before a newer trigger from
	// accidentally satisfying that newer response obligation.
	std::vector<ir::Net> recent_wires;
	std::vector<ir::Net> recent;
	recent_wires.reserve(maximum_duration - 1);
	recent.reserve(maximum_duration - 1);
	for (uint32_t age = 1; age < maximum_duration; age++) {
		ir::Net wire = add_sva_state(eval, "sva_response_recent");
		recent_wires.push_back(wire);
		recent.push_back(wire);
	}
	ir::Net old_wire = add_sva_state(eval, "sva_response_old");
	ir::Net old = old_wire;

	trigger = eval.netlist.LogicAnd(trigger, sva_not(eval, disable));
	std::vector<ir::Net> ages;
	ages.reserve(maximum_duration);
	ages.push_back(trigger);
	ages.insert(ages.end(), recent.begin(), recent.end());

	std::vector<ir::Net> clears(maximum_duration,
									  ir::S0);
	for (uint32_t age = 0; age < maximum_duration; age++) {
		for (const auto& candidate : candidates) {
			if (candidate.duration <= age)
				clears[age] =
					eval.netlist.LogicOr(clears[age], candidate.match);
		}
	}

	ir::Net pending = old;
	ir::Net completion = eval.netlist.LogicAnd(old, any_match);
	std::vector<ir::Net> remaining;
	remaining.reserve(ages.size());
	for (uint32_t age = 0; age < ages.size(); age++) {
		pending = eval.netlist.LogicOr(pending, ages[age]);
		completion = eval.netlist.LogicOr(
			completion,
			eval.netlist.LogicAnd(ages[age], clears[age]));
		ir::Net keep =
			eval.netlist.LogicAnd(ages[age], sva_not(eval, clears[age]));
		remaining.push_back(
			eval.netlist.LogicAnd(keep, sva_not(eval, disable)));
	}
	ir::Net old_next =
		eval.netlist.LogicAnd(old, sva_not(eval, any_match));
	old_next = eval.netlist.LogicAnd(old_next, sva_not(eval, disable));
	old_next = eval.netlist.LogicOr(old_next, remaining.back());

	if (!sva_clock_ready(eval, loc)) {
		result.valid = false;
		return;
	}
	for (uint32_t i = 0; i < recent_wires.size(); i++) {
		add_sva_flop(eval, remaining[i], recent_wires[i], "sva_response_recent");
	}
	add_sva_flop(eval, old_next, old_wire, "sva_response_old");

	ir::Net goal = eval.netlist.LogicOr(
		sva_not(eval, pending), completion);
	goal = eval.netlist.LogicOr(goal, disable);
	result.obligations.push_back(
		{PropertyObligationKind::Liveness, goal,
		 ir::S1, pending});
}

static std::optional<ir::Net>
antecedent_match_event(EvalContext& eval, const ast::AssertionExpr& sequence,
					   ir::Net trigger, bool nonoverlapped,
					   ir::Net disable) {
	ir::Net result = ir::S0;
	bool admits_empty = false;
	if (assertion_has_unbounded_sequence(sequence)) {
		auto match = synthesize_unbounded_sequence(
			eval, sequence, trigger, disable, &admits_empty);
		if (!match.has_value())
			return std::nullopt;
		result = match.value();
	} else {
		for (const auto& path : compress_paths(synthesize_assertion_expr(eval, sequence))) {
			if (path.empty) {
				admits_empty = true;
				continue;
			}
			if (uint32_t(path.start) > SVA_ENUMERATION_LIMIT) {
				eval.netlist.add_diag(diag::AssertionUnsupported, path.loc);
				return std::nullopt;
			}
			ir::Net path_trigger = delayed_abortable_trigger(
				eval, trigger, uint32_t(path.start), disable, path.loc);
			ir::Net path_match = eval.netlist.LogicAnd(
				eval.netlist.ReduceBool(path.sig),
				eval.netlist.ReduceBool(path.en));
			path_match = eval.netlist.LogicAnd(path_match, path_trigger);
			result = eval.netlist.LogicOr(result, path_match);
		}
	}
	// `seq |-> p` has no tick to start `p` on after an empty match, but
	// `seq |=> p` is `(seq ##1 `true) |-> p` where it matches on the first tick
	if (nonoverlapped) {
		result = delayed_abortable_trigger(
			eval, result, 1, disable, expr_loc(sequence));
		if (admits_empty)
			result = eval.netlist.LogicOr(result, trigger);
	}
	return eval.netlist.LogicAnd(result, sva_not(eval, disable));
}

static void add_liveness_obligation(PropertyResult& result, EvalContext& eval,
									ir::Net goal, ir::Net trigger,
									ir::Net disable) {
	// A disable aborts all currently pending attempts, and prevents a new
	// attempt from starting in the disabled cycle.
	goal = eval.netlist.LogicOr(goal, disable);
	trigger = eval.netlist.LogicAnd(trigger, sva_not(eval, disable));
	ir::Net active = add_pending_monitor(
		eval, trigger, goal, ir::S0,
		slang::SourceLocation::NoLocation);
	result.obligations.push_back(
		{PropertyObligationKind::Liveness, goal, trigger, active});
}

static void add_safety_obligation(PropertyResult& result, ir::Value a,
								  ir::Value en) {
	result.obligations.push_back(
		{PropertyObligationKind::Safety, a, en, ir::S0});
}

static std::optional<ir::Net>
simple_property_signal(EvalContext& eval, const ast::AssertionExpr& expr,
					   bool negate = false) {
	const ast::AssertionExpr *current = &expr;
	if (current->kind == ast::AssertionExprKind::Simple) {
		const auto& simple = current->as<ast::SimpleAssertionExpr>();
		if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
			const auto& instance =
				simple.expr.as<ast::AssertionInstanceExpression>();
			if (instance.isRecursiveProperty)
				return std::nullopt;
			return simple_property_signal(eval, instance.body, negate);
		}
		if (simple.repetition.has_value())
			return std::nullopt;
		ir::Net value = simple.isNullExpr
			? ir::S0
			: eval.netlist.ReduceBool(eval.sva(simple.expr));
		return negate ? sva_not(eval, value) : value;
	}
	if (current->kind == ast::AssertionExprKind::Unary) {
		const auto& unary = current->as<ast::UnaryAssertionExpr>();
		if (unary.op == ast::UnaryAssertionOperator::Not)
			return simple_property_signal(eval, unary.expr, !negate);
	}
	return std::nullopt;
}

static ir::Net add_pending_monitor(EvalContext& eval, ir::Net trigger,
										ir::Net completion,
										ir::Net disable,
										slang::SourceLocation loc) {
	ir::Net pending = add_sva_state(eval, "sva_pending");

	ir::Net active = eval.netlist.LogicOr(pending, trigger);
	ir::Net keep = eval.netlist.LogicAnd(active, sva_not(eval, completion));
	keep = eval.netlist.LogicAnd(keep, sva_not(eval, disable));

	if (!sva_clock_ready(eval, loc)) {
		return ir::Sx;
	}
	add_sva_flop(eval, keep, pending, "sva_pending");
	return active;
}

static ir::Net add_sticky_observation(
		EvalContext& eval, ir::Net event, ir::Net clear,
		slang::SourceLocation loc, std::string_view name) {
	ir::Net wire = add_sva_state(eval, name);
	ir::Net observed = eval.netlist.LogicOr(wire, event);
	ir::Net next =
		eval.netlist.LogicAnd(observed, sva_not(eval, clear));

	if (!sva_clock_ready(eval, loc)) {
		return ir::Sx;
	}
	add_sva_flop(eval, next, wire, name);
	return observed;
}

static ir::Net add_selected_trigger(
		PropertyResult& result, EvalContext& eval, ir::Net trigger,
		ir::Net disable, slang::SourceLocation loc) {
	trigger = eval.netlist.LogicAnd(trigger, sva_not(eval, disable));
	ir::Net seen_trigger = add_sticky_observation(
		eval, trigger, ir::S0, loc, "sva_select_seen");

	ir::Net choice = eval.netlist.add_placeholder_signal(1, "sva_select_choice").as_net();
	auto *choice_cell = eval.netlist.backend->canvas->addCell(
		eval.netlist.backend->new_id("sva_select_choice"), ID($anyseq));
	choice_cell->setParam(ID::WIDTH, 1);
	choice_cell->setPort(ID::Y, {choice});

	ir::Net chosen_wire = add_sva_state(eval, "sva_select_chosen");
	ir::Net selected = eval.netlist.LogicAnd(
		choice,
		eval.netlist.LogicAnd(trigger, sva_not(eval, chosen_wire)));
	ir::Net chosen = eval.netlist.LogicOr(chosen_wire, selected);

	if (!sva_clock_ready(eval, loc)) {
		result.valid = false;
		return ir::Sx;
	}
	add_sva_flop(eval, chosen, chosen_wire, "sva_select_chosen");

	// The fresh prophecy input selects exactly one enabled attempt.  This
	// fairness condition prevents it from avoiding every trigger.  Because
	// all possible selections remain in the formal state space, proving the
	// selected attempt proves every attempt without merging their histories.
	ir::Net selection_fair =
		eval.netlist.LogicOr(sva_not(eval, seen_trigger), chosen);
	result.obligations.push_back(
		{PropertyObligationKind::AuxiliaryFairness,
		 selection_fair, ir::S1, ir::S0});
	return selected;
}

static std::optional<ir::Net> synthesize_selected_sequence(
		EvalContext& eval, const ast::AssertionExpr& sequence,
		ir::Net trigger, ir::Net disable);

static std::optional<ir::Net> synthesize_selected_sequence(
		EvalContext& eval, const ast::AssertionExpr& sequence,
		ir::Net trigger, ir::Net disable) {
	if (sequence.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = sequence.as<ast::SimpleAssertionExpr>();
		if (!simple.repetition.has_value() &&
			simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
			const auto& instance =
				simple.expr.as<ast::AssertionInstanceExpression>();
			if (!instance.isRecursiveProperty)
				return synthesize_selected_sequence(
					eval, instance.body, trigger, disable);
		}
	}
	if (sequence.kind == ast::AssertionExprKind::FirstMatch) {
		const auto& first = sequence.as<ast::FirstMatchAssertionExpr>();
		if (!first.matchItems.empty()) {
			eval.netlist.add_diag(diag::SVAMatchItemsUnsupported,
								  expr_loc(sequence));
			return std::nullopt;
		}
		return synthesize_selected_sequence(
			eval, first.seq, trigger, disable);
	}
	if (sequence.kind == ast::AssertionExprKind::Binary) {
		const auto& binary = sequence.as<ast::BinaryAssertionExpr>();
		switch (binary.op) {
		case ast::BinaryAssertionOperator::Or:
			{
				auto left = synthesize_selected_sequence(
					eval, binary.left, trigger, disable);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				return eval.netlist.LogicOr(left.value(), right.value());
			}
		case ast::BinaryAssertionOperator::And:
			{
				auto left = synthesize_selected_sequence(
					eval, binary.left, trigger, disable);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				ir::Net left_seen = add_sticky_observation(
					eval, left.value(), disable, expr_loc(binary.left),
					"sva_and_left");
				ir::Net right_seen = add_sticky_observation(
					eval, right.value(), disable, expr_loc(binary.right),
					"sva_and_right");
				return eval.netlist.LogicAnd(left_seen, right_seen);
			}
		case ast::BinaryAssertionOperator::Intersect:
			{
				auto left = synthesize_selected_sequence(
					eval, binary.left, trigger, disable);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				return eval.netlist.LogicAnd(left.value(), right.value());
			}
		case ast::BinaryAssertionOperator::Within:
			{
				ir::Net delayed_start = add_sticky_observation(
					eval, trigger, disable, expr_loc(binary.left),
					"sva_within_start");
				auto inner = synthesize_selected_sequence(
					eval, binary.left, delayed_start, disable);
				auto outer = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!inner.has_value() || !outer.has_value())
					return std::nullopt;
				ir::Net inner_seen = add_sticky_observation(
					eval, inner.value(), disable, expr_loc(binary.left),
					"sva_within_seen");
				return eval.netlist.LogicAnd(inner_seen, outer.value());
			}
		case ast::BinaryAssertionOperator::Throughout:
			{
				auto condition =
					simple_property_signal(eval, binary.left);
				if (!condition.has_value()) {
					eval.netlist.add_diag(diag::AssertionUnsupported,
										  expr_loc(binary.left));
					return std::nullopt;
				}
				auto body = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!body.has_value())
					return std::nullopt;
				ir::Net active = add_sticky_observation(
					eval, trigger, disable, expr_loc(binary.right),
					"sva_throughout_active");
				ir::Net invalid = add_sticky_observation(
					eval,
					eval.netlist.LogicAnd(active,
						sva_not(eval, condition.value())),
					disable, expr_loc(binary.right),
					"sva_throughout_invalid");
				return eval.netlist.LogicAnd(
					body.value(),
					eval.netlist.LogicAnd(active, sva_not(eval, invalid)));
			}
		default:
			break;
		}
	}
	return synthesize_unbounded_sequence(
		eval, sequence, trigger, disable);
}

static PropertyResult lower_property(EvalContext& eval, const ast::AssertionExpr& expr,
									 ir::Net trigger = ir::S1,
									 ir::Net disable = ir::S0);

static PropertyResult lower_finite_property_from_trigger(
		EvalContext& eval, const ast::AssertionExpr& expr,
		ir::Net trigger, ir::Net disable) {
	PropertyResult result;
	auto paths = synthesize_assertion_expr(eval, expr);
	if (paths.empty()) {
		// With no possible match the property fails on every attempt
		add_safety_obligation(result, ir::S0,
							  eval.netlist.LogicAnd(trigger, sva_not(eval, disable)));
		return result;
	}
	auto match = collapse_or(paths);
	auto init_escape = delay_sva_sample(
		eval, ir::S0, match.start,
		ir::S1, match.loc);
	ir::Net frame_ready =
		sva_not(eval, eval.netlist.ReduceBool(init_escape));
	ir::Net enable = delayed_abortable_trigger(
		eval, trigger, match.start, disable, match.loc);
	enable = eval.netlist.LogicAnd(enable,
		eval.netlist.ReduceBool(match.en));
	enable = eval.netlist.LogicAnd(enable, frame_ready);
	add_safety_obligation(result, match.sig, enable);
	return result;
}

static bool property_requires_temporal_obligations(const ast::AssertionExpr& expr) {
	switch (expr.kind) {
	case ast::AssertionExprKind::Simple:
		{
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
				return false;
			const auto& instance =
				simple.expr.as<ast::AssertionInstanceExpression>();
			return !instance.isRecursiveProperty &&
				   property_requires_temporal_obligations(instance.body);
		}
	case ast::AssertionExprKind::DisableIff:
		return property_requires_temporal_obligations(
			expr.as<ast::DisableIffAssertionExpr>().expr);
	case ast::AssertionExprKind::Conditional:
		{
			const auto& conditional =
				expr.as<ast::ConditionalAssertionExpr>();
			return property_requires_temporal_obligations(conditional.ifExpr) ||
				   (conditional.elseExpr &&
					property_requires_temporal_obligations(
						*conditional.elseExpr));
		}
	case ast::AssertionExprKind::Case:
		{
			const auto& case_expr = expr.as<ast::CaseAssertionExpr>();
			for (const auto& item : case_expr.items)
				if (property_requires_temporal_obligations(*item.body))
					return true;
			return case_expr.defaultCase &&
				   property_requires_temporal_obligations(
					   *case_expr.defaultCase);
		}
	case ast::AssertionExprKind::Abort:
		return property_requires_temporal_obligations(
			expr.as<ast::AbortAssertionExpr>().expr);
	case ast::AssertionExprKind::StrongWeak:
		{
			const auto& strong_weak = expr.as<ast::StrongWeakAssertionExpr>();
			return strong_weak.strength == ast::StrongWeakAssertionExpr::Strong &&
				   assertion_has_unbounded_sequence(strong_weak.expr);
		}
	case ast::AssertionExprKind::Unary:
		{
			const auto& unary = expr.as<ast::UnaryAssertionExpr>();
			if (unary.op == ast::UnaryAssertionOperator::Eventually ||
				unary.op == ast::UnaryAssertionOperator::SEventually)
				return !unary.range.has_value() ||
					   !unary.range->max.has_value();
			if (unary.op == ast::UnaryAssertionOperator::Always ||
				unary.op == ast::UnaryAssertionOperator::SAlways)
				return true;
			if (unary.op == ast::UnaryAssertionOperator::Not)
				return property_requires_temporal_obligations(unary.expr);
			return false;
		}
	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = expr.as<ast::BinaryAssertionExpr>();
			switch (binary.op) {
			case ast::BinaryAssertionOperator::Until:
			case ast::BinaryAssertionOperator::SUntil:
			case ast::BinaryAssertionOperator::UntilWith:
			case ast::BinaryAssertionOperator::SUntilWith:
				return true;
			case ast::BinaryAssertionOperator::And:
			case ast::BinaryAssertionOperator::Or:
			case ast::BinaryAssertionOperator::Implies:
			case ast::BinaryAssertionOperator::Iff:
			case ast::BinaryAssertionOperator::OverlappedImplication:
			case ast::BinaryAssertionOperator::NonOverlappedImplication:
			case ast::BinaryAssertionOperator::OverlappedFollowedBy:
			case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
				return property_requires_temporal_obligations(binary.left) ||
					   property_requires_temporal_obligations(binary.right);
			default:
				return false;
			}
		}
	default:
		return false;
	}
}

static bool is_direct_justice_property(const ast::AssertionExpr& expr) {
	if (expr.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = expr.as<ast::SimpleAssertionExpr>();
		if (simple.repetition.has_value() ||
			simple.expr.kind != ast::ExpressionKind::AssertionInstance)
			return false;
		const auto& instance =
			simple.expr.as<ast::AssertionInstanceExpression>();
		return !instance.isRecursiveProperty &&
			   is_direct_justice_property(instance.body);
	}
	if (expr.kind != ast::AssertionExprKind::Unary)
		return false;
	const auto& unary = expr.as<ast::UnaryAssertionExpr>();
	if (unary.op == ast::UnaryAssertionOperator::Eventually ||
		unary.op == ast::UnaryAssertionOperator::SEventually)
		return !unary.range.has_value() ||
			   !unary.range->max.has_value();
	if (unary.op != ast::UnaryAssertionOperator::Not)
		return false;
	const auto *nested = unary.expr.as_if<ast::UnaryAssertionExpr>();
	return nested &&
		   (nested->op == ast::UnaryAssertionOperator::Always ||
			nested->op == ast::UnaryAssertionOperator::SAlways) &&
		   (!nested->range.has_value() ||
			!nested->range->max.has_value());
}

static PropertyResult lower_eventual(EvalContext& eval,
									 const ast::UnaryAssertionExpr& eventual,
									 ir::Net trigger, ir::Net disable) {
	PropertyResult result;
	if (eventual.range.has_value() && eventual.range->max.has_value()) {
		return lower_finite_property_from_trigger(
			eval, eventual, trigger, disable);
	}

	uint32_t minimum = eventual.range.has_value() ? eventual.range->min : 0;
	if (assertion_has_unbounded_sequence(eventual.expr) &&
		!is_suffix_eventual_sequence(eventual.expr)) {
		ir::Net selected = add_selected_trigger(
			result, eval, trigger, disable, expr_loc(eventual));
		if (!result.valid)
			return result;
		selected = delayed_abortable_trigger(
			eval, selected, minimum, disable, expr_loc(eventual));
		ir::Net future_start = add_sticky_observation(
			eval, selected, disable, expr_loc(eventual),
			"sva_eventual_start");
		auto goal = synthesize_selected_sequence(
			eval, eventual.expr, future_start, disable);
		if (!goal.has_value())
			return {{}, false};
		add_liveness_obligation(
			result, eval, goal.value(), selected, disable);
		return result;
	}
	std::optional<std::vector<EventualCandidate>> candidates;
	if (assertion_has_unbounded_sequence(eventual.expr))
		candidates = suffix_eventual_candidates(eval, eventual.expr, minimum);
	else
		candidates = finite_eventual_candidates(eval, eventual.expr, minimum);
	if (!candidates.has_value()) {
		result.valid = false;
		return result;
	}
	add_response_liveness_obligation(
		result, eval, *candidates, trigger, disable, expr_loc(eventual));
	return result;
}

static PropertyResult lower_always(EvalContext& eval,
								   const ast::UnaryAssertionExpr& always_expr,
								   ir::Net trigger,
								   ir::Net disable) {
	PropertyResult result;
	auto condition = simple_property_signal(eval, always_expr.expr);
	if (!condition.has_value()) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(always_expr));
		result.valid = false;
		return result;
	}

	uint32_t minimum = always_expr.range.has_value()
		? always_expr.range->min : 0;
	trigger = delayed_abortable_trigger(eval, trigger, minimum, disable,
										expr_loc(always_expr));

	if (always_expr.range.has_value() &&
		always_expr.range->max.has_value()) {
		uint32_t maximum = always_expr.range->max.value();
		if (maximum < minimum || maximum > SVA_ENUMERATION_LIMIT) {
			eval.netlist.add_diag(diag::AssertionUnsupported,
								  expr_loc(always_expr));
			result.valid = false;
			return result;
		}
		ir::Net enable = trigger;
		for (uint32_t offset = minimum; offset <= maximum; offset++) {
			if (offset != minimum)
				enable = delayed_abortable_trigger(
					eval, enable, 1, disable, expr_loc(always_expr));
			add_safety_obligation(result, condition.value(),
				eval.netlist.LogicAnd(enable, sva_not(eval, disable)));
		}
		return result;
	}

	ir::Net pending = add_pending_monitor(eval, trigger,
											   ir::S0,
											   disable,
											   expr_loc(always_expr));
	add_safety_obligation(result, condition.value(),
		eval.netlist.LogicAnd(pending, sva_not(eval, disable)));
	return result;
}

static PropertyResult lower_until(EvalContext& eval,
								  const ast::BinaryAssertionExpr& until_expr,
								  ir::Net trigger, ir::Net disable) {
	PropertyResult result;
	auto left = simple_property_signal(eval, until_expr.left);
	auto right = simple_property_signal(eval, until_expr.right);
	if (!left.has_value() || !right.has_value()) {
		eval.netlist.add_diag(diag::AssertionUnsupported,
							  expr_loc(until_expr));
		result.valid = false;
		return result;
	}

	bool strong = until_expr.op == ast::BinaryAssertionOperator::SUntil ||
				  until_expr.op == ast::BinaryAssertionOperator::SUntilWith;
	bool including_completion =
		until_expr.op == ast::BinaryAssertionOperator::UntilWith ||
		until_expr.op == ast::BinaryAssertionOperator::SUntilWith;

	ir::Net pending = add_pending_monitor(eval, trigger, right.value(),
											   disable, expr_loc(until_expr));
	ir::Net must_hold = including_completion
		? pending
		: eval.netlist.LogicAnd(pending, sva_not(eval, right.value()));
	must_hold = eval.netlist.LogicAnd(must_hold, sva_not(eval, disable));
	add_safety_obligation(result, left.value(), must_hold);
	if (strong)
		add_liveness_obligation(result, eval, right.value(), trigger, disable);
	return result;
}

static PropertyResult lower_property(EvalContext& eval, const ast::AssertionExpr& expr,
									 ir::Net trigger, ir::Net disable) {
	if (expr.kind == ast::AssertionExprKind::DisableIff) {
		const auto& disabled = expr.as<ast::DisableIffAssertionExpr>();
		ir::Net condition =
			eval.netlist.ReduceBool(eval(disabled.condition));
		return lower_property(eval, disabled.expr, trigger,
							  eval.netlist.LogicOr(disable, condition));
	}

	if (expr.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = expr.as<ast::SimpleAssertionExpr>();
		if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
			const auto& instance =
				simple.expr.as<ast::AssertionInstanceExpression>();
			if (instance.isRecursiveProperty) {
				eval.netlist.add_diag(diag::AssertionUnsupported,
									  simple.expr.sourceRange);
				return {{}, false};
			}
			return lower_property(eval, instance.body, trigger, disable);
		}
	}

	if (expr.kind == ast::AssertionExprKind::Unary) {
		const auto& unary = expr.as<ast::UnaryAssertionExpr>();
		if (unary.op == ast::UnaryAssertionOperator::Eventually ||
			unary.op == ast::UnaryAssertionOperator::SEventually)
			return lower_eventual(eval, unary, trigger, disable);
		if (unary.op == ast::UnaryAssertionOperator::Always ||
			unary.op == ast::UnaryAssertionOperator::SAlways)
			return lower_always(eval, unary, trigger, disable);
		if (unary.op == ast::UnaryAssertionOperator::Not) {
			const auto *nested = unary.expr.as_if<ast::UnaryAssertionExpr>();
			if (nested &&
				(nested->op == ast::UnaryAssertionOperator::Eventually ||
				 nested->op == ast::UnaryAssertionOperator::SEventually) &&
				(!nested->range.has_value() ||
				 !nested->range->max.has_value())) {
				auto condition = simple_property_signal(eval, nested->expr, true);
				if (!condition.has_value()) {
					eval.netlist.add_diag(diag::AssertionUnsupported,
										  expr_loc(expr));
					return {{}, false};
				}
				// Use the same ranged always machinery with the negated
				// operand represented as a one-cycle assertion expression.
				uint32_t minimum = nested->range.has_value()
					? nested->range->min : 0;
				ir::Net start = delayed_abortable_trigger(
					eval, trigger, minimum, disable, expr_loc(expr));
				ir::Net pending = add_pending_monitor(
					eval, start, ir::S0, disable, expr_loc(expr));
				PropertyResult result;
				add_safety_obligation(result, condition.value(),
					eval.netlist.LogicAnd(pending, sva_not(eval, disable)));
				return result;
			}
			if (nested &&
				(nested->op == ast::UnaryAssertionOperator::Always ||
				 nested->op == ast::UnaryAssertionOperator::SAlways)) {
				auto violation =
					simple_property_signal(eval, nested->expr, true);
				if (!violation.has_value()) {
					eval.netlist.add_diag(diag::AssertionUnsupported,
										  expr_loc(expr));
					return {{}, false};
				}
				uint32_t minimum = nested->range.has_value()
					? nested->range->min : 0;
				if (nested->range.has_value() &&
					nested->range->max.has_value()) {
					uint32_t maximum = nested->range->max.value();
					if (maximum < minimum ||
						maximum > SVA_ENUMERATION_LIMIT) {
						eval.netlist.add_diag(diag::AssertionUnsupported,
											  expr_loc(expr));
						return {{}, false};
					}
					ir::Net seen = ir::S0;
					for (uint32_t offset = minimum;
						 offset <= maximum; offset++) {
						ir::Net sample = delay_sva_sample(
							eval, violation.value(), maximum - offset,
							ir::S0, expr_loc(expr),
							"sva_not_always").as_net();
						seen = eval.netlist.LogicOr(seen, sample);
					}
					ir::Net enable =
						delayed_abortable_trigger(
							eval, trigger, maximum, disable,
							expr_loc(expr));
					PropertyResult result;
					add_safety_obligation(result, seen, enable);
					return result;
				}
				PropertyResult result;
				add_response_liveness_obligation(
					result, eval, {{violation.value(), minimum}},
					trigger, disable, expr_loc(expr));
				return result;
			}
		}
	}

	if (expr.kind == ast::AssertionExprKind::StrongWeak) {
		const auto& strong_weak = expr.as<ast::StrongWeakAssertionExpr>();
		if (strong_weak.strength == ast::StrongWeakAssertionExpr::Weak) {
			// Weak only changes end-of-simulation behavior.  For a finite
			// sequence, the ordinary finite monitor is exact.
			if (assertion_has_unbounded_sequence(strong_weak.expr)) {
				eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
				return {{}, false};
			}
			return lower_finite_property_from_trigger(
				eval, strong_weak.expr, trigger, disable);
		}

		if (!assertion_has_unbounded_sequence(strong_weak.expr)) {
			return lower_finite_property_from_trigger(
				eval, strong_weak.expr, trigger, disable);
		}
		if (strong_weak.expr.kind == ast::AssertionExprKind::Binary) {
			const auto& binary =
				strong_weak.expr.as<ast::BinaryAssertionExpr>();
			if (binary.op == ast::BinaryAssertionOperator::And &&
				(!assertion_has_unbounded_sequence(binary.left) ||
				 is_suffix_eventual_sequence(binary.left)) &&
				(!assertion_has_unbounded_sequence(binary.right) ||
				 is_suffix_eventual_sequence(binary.right))) {
				PropertyResult left;
				PropertyResult right;
				if (assertion_has_unbounded_sequence(binary.left)) {
					auto candidates =
						suffix_eventual_candidates(eval, binary.left);
					if (!candidates.has_value())
						return {{}, false};
					add_response_liveness_obligation(
						left, eval, *candidates, trigger, disable,
						expr_loc(binary.left));
				} else {
					left = lower_finite_property_from_trigger(
						eval, binary.left, trigger, disable);
				}
				if (assertion_has_unbounded_sequence(binary.right)) {
					auto candidates =
						suffix_eventual_candidates(eval, binary.right);
					if (!candidates.has_value())
						return {{}, false};
					add_response_liveness_obligation(
						right, eval, *candidates, trigger, disable,
						expr_loc(binary.right));
				} else {
					right = lower_finite_property_from_trigger(
						eval, binary.right, trigger, disable);
				}
				left.valid = left.valid && right.valid;
				left.obligations.insert(left.obligations.end(),
										right.obligations.begin(),
										right.obligations.end());
				return left;
			}
		}
		if (!is_suffix_eventual_sequence(strong_weak.expr)) {
			PropertyResult result;
			ir::Net selected = add_selected_trigger(
				result, eval, trigger, disable, expr_loc(expr));
			if (!result.valid)
				return result;
			auto goal = synthesize_selected_sequence(
				eval, strong_weak.expr, selected, disable);
			if (!goal.has_value())
				return {{}, false};
			add_liveness_obligation(
				result, eval, goal.value(), selected, disable);
			return result;
		}
		auto candidates =
			suffix_eventual_candidates(eval, strong_weak.expr);
		if (!candidates.has_value())
			return {{}, false};
		PropertyResult result;
		add_response_liveness_obligation(
			result, eval, *candidates, trigger, disable, expr_loc(expr));
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Abort) {
		const auto& abort = expr.as<ast::AbortAssertionExpr>();
		ir::Net condition =
			eval.netlist.ReduceBool(eval(abort.condition));
		if (abort.action == ast::AbortAssertionExpr::Accept)
			return lower_property(eval, abort.expr, trigger,
								  eval.netlist.LogicOr(disable, condition));

		PropertyResult result =
			lower_property(eval, abort.expr, trigger, disable);
		if (!result.valid)
			return result;
		bool has_liveness = false;
		std::vector<ir::Net> active_obligations;
		for (const auto& obligation : result.obligations) {
			if (obligation.kind == PropertyObligationKind::Safety) {
				eval.netlist.add_diag(diag::AssertionUnsupported,
									  expr_loc(expr));
				return {{}, false};
			}
			if (obligation.kind != PropertyObligationKind::Liveness)
				continue;
			has_liveness = true;
			active_obligations.push_back(
				eval.netlist.ReduceBool(obligation.active));
		}
		for (ir::Net active : active_obligations) {
			add_safety_obligation(
				result, sva_not(eval, condition),
				eval.netlist.LogicAnd(active, sva_not(eval, disable)));
		}
		if (!has_liveness) {
			eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return {{}, false};
		}
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Conditional) {
		const auto& conditional =
			expr.as<ast::ConditionalAssertionExpr>();
		ir::Net condition =
			eval.netlist.ReduceBool(eval(conditional.condition));
		PropertyResult result = lower_property(
			eval, conditional.ifExpr,
			eval.netlist.LogicAnd(trigger, condition), disable);
		if (conditional.elseExpr) {
			PropertyResult else_result = lower_property(
				eval, *conditional.elseExpr,
				eval.netlist.LogicAnd(trigger, sva_not(eval, condition)), disable);
			result.valid = result.valid && else_result.valid;
			result.obligations.insert(result.obligations.end(),
									  else_result.obligations.begin(),
									  else_result.obligations.end());
		}
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Case) {
		const auto& case_expr = expr.as<ast::CaseAssertionExpr>();
		ir::Value control = eval(case_expr.expr);
		ir::Net matched = ir::S0;
		PropertyResult result;
		for (const auto& item : case_expr.items) {
			ir::Net item_hit = ir::S0;
			for (const auto *item_expr : item.expressions)
				item_hit = eval.netlist.LogicOr(
					item_hit,
					inside_comparison(eval, control, *item_expr));
			item_hit = eval.netlist.LogicAnd(item_hit, sva_not(eval, matched));
			PropertyResult branch = lower_property(
				eval, *item.body,
				eval.netlist.LogicAnd(trigger, item_hit), disable);
			result.valid = result.valid && branch.valid;
			result.obligations.insert(result.obligations.end(),
									 branch.obligations.begin(),
									 branch.obligations.end());
			matched = eval.netlist.LogicOr(matched, item_hit);
		}
		if (case_expr.defaultCase) {
			PropertyResult branch = lower_property(
				eval, *case_expr.defaultCase,
				eval.netlist.LogicAnd(trigger, sva_not(eval, matched)), disable);
			result.valid = result.valid && branch.valid;
			result.obligations.insert(result.obligations.end(),
									 branch.obligations.begin(),
									 branch.obligations.end());
		}
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Binary) {
		const auto& binary = expr.as<ast::BinaryAssertionExpr>();
		switch (binary.op) {
		case ast::BinaryAssertionOperator::Implies:
			{
				auto antecedent =
					simple_property_signal(eval, binary.left);
				if (!antecedent.has_value()) {
					eval.netlist.add_diag(
						property_requires_temporal_obligations(binary.left)
							? diag::SVALivenessAcceptanceUnsupported
							: diag::AssertionUnsupported,
						expr_loc(expr));
					return {{}, false};
				}
				ir::Net consequent_trigger = eval.netlist.LogicAnd(
					trigger, antecedent.value());
				return lower_property(
					eval, binary.right, consequent_trigger, disable);
			}

		case ast::BinaryAssertionOperator::Iff:
			{
				auto left = simple_property_signal(eval, binary.left);
				auto right = simple_property_signal(eval, binary.right);
				if (!left.has_value() || !right.has_value()) {
					eval.netlist.add_diag(
						(property_requires_temporal_obligations(binary.left) ||
						 property_requires_temporal_obligations(binary.right))
							? diag::SVALivenessAcceptanceUnsupported
							: diag::AssertionUnsupported,
						expr_loc(expr));
					return {{}, false};
				}
				PropertyResult result;
				add_safety_obligation(
					result, eval.netlist.Eq(left.value(), right.value()),
					eval.netlist.LogicAnd(trigger, sva_not(eval, disable)));
				return result;
			}

		case ast::BinaryAssertionOperator::OverlappedImplication:
		case ast::BinaryAssertionOperator::NonOverlappedImplication:
			{
				bool temporal_consequent =
					property_requires_temporal_obligations(binary.right) ||
					assertion_has_unbounded_sequence(binary.right);
				if (!temporal_consequent &&
					!assertion_has_unbounded_sequence(binary.left))
					break;
				bool nonoverlapped =
					binary.op == ast::BinaryAssertionOperator::NonOverlappedImplication;
				auto antecedent = antecedent_match_event(
					eval, binary.left, trigger, nonoverlapped, disable);
				if (!antecedent.has_value())
					return {{}, false};
				ir::Net consequent_trigger = antecedent.value();
				if (!temporal_consequent)
					return lower_finite_property_from_trigger(
						eval, binary.right, consequent_trigger, disable);
				return lower_property(eval, binary.right, consequent_trigger, disable);
			}

		case ast::BinaryAssertionOperator::OverlappedFollowedBy:
		case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
			{
				bool temporal =
					property_requires_temporal_obligations(binary.right) ||
					assertion_has_unbounded_sequence(binary.left);
				if (!temporal)
					break;
				bool nonoverlapped =
					binary.op ==
					ast::BinaryAssertionOperator::NonOverlappedFollowedBy;
				PropertyResult result;
				if (assertion_has_unbounded_sequence(binary.left)) {
					if (!is_suffix_eventual_sequence(binary.left)) {
						eval.netlist.add_diag(
							diag::SVALivenessAcceptanceUnsupported,
							expr_loc(binary.left));
						return {{}, false};
					}
					auto candidates =
						suffix_eventual_candidates(eval, binary.left);
					if (!candidates.has_value())
						return {{}, false};
					add_response_liveness_obligation(
						result, eval, *candidates, trigger, disable,
						expr_loc(binary.left));
				} else {
					result = lower_finite_property_from_trigger(
						eval, binary.left, trigger, disable);
				}
				auto antecedent = antecedent_match_event(
					eval, binary.left, trigger, nonoverlapped, disable);
				if (!antecedent.has_value())
					return {{}, false};
				PropertyResult consequent = lower_property(
					eval, binary.right, antecedent.value(), disable);
				result.valid = result.valid && consequent.valid;
				result.obligations.insert(
					result.obligations.end(),
					consequent.obligations.begin(),
					consequent.obligations.end());
				return result;
			}

		case ast::BinaryAssertionOperator::Until:
		case ast::BinaryAssertionOperator::SUntil:
		case ast::BinaryAssertionOperator::UntilWith:
		case ast::BinaryAssertionOperator::SUntilWith:
			return lower_until(eval, binary, trigger, disable);

		case ast::BinaryAssertionOperator::And:
			{
				if (!property_requires_temporal_obligations(binary.left) &&
					!property_requires_temporal_obligations(binary.right) &&
					!assertion_has_unbounded_sequence(binary.left) &&
					!assertion_has_unbounded_sequence(binary.right))
					break;
				PropertyResult left =
					lower_property(eval, binary.left, trigger, disable);
				PropertyResult right =
					lower_property(eval, binary.right, trigger, disable);
				left.valid = left.valid && right.valid;
				left.obligations.insert(left.obligations.end(),
										right.obligations.begin(),
										right.obligations.end());
				return left;
			}
		case ast::BinaryAssertionOperator::Or:
			{
				if (!property_requires_temporal_obligations(binary.left) &&
					!property_requires_temporal_obligations(binary.right))
					break;
				if (!is_direct_justice_property(binary.left) ||
					!is_direct_justice_property(binary.right) ||
					trigger != ir::S1) {
					eval.netlist.add_diag(
						diag::SVALivenessAcceptanceUnsupported,
						expr_loc(expr));
					return {{}, false};
				}
				PropertyResult left =
					lower_property(eval, binary.left, trigger, disable);
				PropertyResult right =
					lower_property(eval, binary.right, trigger, disable);
				if (!left.valid || !right.valid ||
					left.obligations.size() != 1 ||
					right.obligations.size() != 1 ||
					left.obligations[0].kind !=
						PropertyObligationKind::Liveness ||
					right.obligations[0].kind !=
						PropertyObligationKind::Liveness ||
					left.obligations[0].en != right.obligations[0].en) {
					eval.netlist.add_diag(diag::AssertionUnsupported,
										  expr_loc(expr));
					return {{}, false};
				}
				PropertyResult result;
				add_liveness_obligation(
					result, eval,
					eval.netlist.LogicOr(
						eval.netlist.ReduceBool(left.obligations[0].a),
						eval.netlist.ReduceBool(right.obligations[0].a)),
					eval.netlist.ReduceBool(left.obligations[0].en),
					ir::S0);
				return result;
			}
		default:
			break;
		}
	}

	return lower_finite_property_from_trigger(
		eval, expr, trigger, disable);
}

// Process a 'concurrent assertion'
//
// Any top level clocking expressions have been stripped. Clocking is part
// of the created procedural context.
void process_sva_property(const ast::ConcurrentAssertionStatement &statement,
						  const ast::StatementBlockSymbol *block,
						  ProceduralContext &procedural, const ast::AssertionExpr &top_expr)
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

	// Obligations state what has to keep holding, which isn't a point in time
	// when the property has succeeded for a cover to observe
	if (statement.assertionKind == ast::AssertionKind::CoverProperty &&
			property_requires_temporal_obligations(*expr)) {
		netlist.add_diag(diag::SVATemporalCoverUnsupported, statement.sourceRange);
		return;
	}

	ir::Net disable = ir::S0;
	if (!ast::DisableIffAssertionExpr::isKind(unwrap_property_instances(*expr).kind) &&
			netlist.process_scope) {
		auto scope = netlist.process_scope;
		if (auto condition = scope->getCompilation().getDefaultDisable(*scope))
			disable = netlist.ReduceBool(procedural.eval(*condition));
	}

	PropertyResult result = lower_property(procedural.eval, *expr, ir::S1, disable);
	if (!result.valid)
		return;
	for (const auto& obligation : result.obligations) {
		if (obligation.kind == PropertyObligationKind::AuxiliaryFairness &&
			statement.assertionKind != ast::AssertionKind::Assert) {
			// Prophecy selection is polarity-correct for assertions: every
			// possible selected attempt must prove progress.  Using it for an
			// assumption would only retain a successful selected attempt and
			// would therefore weaken the user's environment constraint.
			netlist.add_diag(diag::SVALivenessAcceptanceUnsupported,
							  statement.sourceRange);
			return;
		}
	}
	if (statement.assertionKind == ast::AssertionKind::CoverProperty &&
			result.obligations.size() != 1) {
		netlist.add_diag(diag::SVATemporalCoverUnsupported, statement.sourceRange);
		return;
	}

	std::string base_cell_name;

	if (block && unwrap_statement(block->tryGetStatement()) == &statement && !block->name.empty()) {
		// If we are the sole statement in a block, use the block's label
		base_cell_name = netlist.id(*block);
	} else {
		base_cell_name = netlist.backend->new_id();
	}

	for (size_t index = 0; index < result.obligations.size(); index++) {
		const auto& obligation = result.obligations[index];
		std::string flavor;
		if (obligation.kind == PropertyObligationKind::AuxiliaryFairness) {
			flavor = "fair";
		} else if (obligation.kind == PropertyObligationKind::Liveness) {
			switch (statement.assertionKind) {
			case ast::AssertionKind::Assert: flavor = "live"; break;
			case ast::AssertionKind::Assume: flavor = "fair"; break;
			default:
				netlist.add_diag(diag::SVATemporalCoverUnsupported,
								 statement.sourceRange);
				return;
			}
		} else {
			switch (statement.assertionKind) {
			case ast::AssertionKind::Assert:        flavor = "assert"; break;
			case ast::AssertionKind::Assume:        flavor = "assume"; break;
			case ast::AssertionKind::CoverProperty: flavor = "cover"; break;
			default:
				netlist.add_diag(diag::AssertionUnsupported, statement.sourceRange);
				return;
			}
		}

		std::string cell_name =
			index == 0 ? base_cell_name : netlist.backend->new_id("sva_obligation");
		ir::Net a = netlist.ReduceBool(obligation.a);
		ir::Net en = netlist.ReduceBool(obligation.en);

		auto cell = netlist.backend->canvas->addCell(std::move(cell_name), ID($check));
		procedural.set_effects_trigger(cell);
		cell->setPort(ID::EN, {netlist.LogicAnd(cell->getPort(ID::EN), en)});
		cell->setParam(ID::FLAVOR, flavor);
		cell->setParam(ID::FORMAT, std::string(""));
		cell->setParam(ID::ARGS_WIDTH, 0);
		cell->setParam(ID::PRIORITY, --procedural.effects_priority);
		cell->setPort(ID::ARGS, {});
		cell->setPort(ID::A, {a});
		transfer_attrs(netlist, statement, cell);
	}
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

static void process_clocked_sva_property(NetlistContext &netlist,
										 const ast::ConcurrentAssertionStatement &statement,
										 const ast::StatementBlockSymbol *block,
										 const ast::TimingControl &clocking,
										 const ast::AssertionExpr &expr)
{
	ProcessTiming timing(ProcessTiming::EdgeTriggered);
	if (!timing_from_sva_clocking(netlist, clocking, timing))
		return;

	ProceduralContext procedure(netlist, timing);
	process_sva_property(statement, block, procedure, expr);

	RTLIL::Process *rtlil_proc = netlist.backend->canvas->addProcess(netlist.backend->new_id());
	transfer_attrs(netlist, statement, rtlil_proc);
#ifndef SLANG_MUX_LOWERING
	procedure.copy_case_tree_into(rtlil_proc->root_case);
#endif
}

void process_freestanding_sva_property(NetlistContext &netlist,
									   const ast::ConcurrentAssertionStatement &statement,
						  			   const ast::StatementBlockSymbol *block)
{
	const ast::AssertionExpr &spec = statement.propertySpec;
	const ast::Scope *scope = netlist.process_scope;

	if (auto clocking_expr = get_top_clocking_expr(spec)) {
		// Need to strip clocking
		process_clocked_sva_property(netlist, statement, block, clocking_expr->clocking,
									 clocking_expr->expr);
		return;
	} else if (scope) {
		if (auto default_clocking = scope->getCompilation().getDefaultClocking(*scope)) {
			const auto &clocking = default_clocking->as<ast::ClockingBlockSymbol>().getEvent();
			process_clocked_sva_property(netlist, statement, block, clocking, spec);
			return;
		}
	}

	// No clocking
	ProceduralContext procedure(netlist, ProcessTiming::implicit);
	process_sva_property(statement, block, procedure, spec);

	RTLIL::Process *rtlil_proc = netlist.backend->canvas->addProcess(netlist.backend->new_id());
	transfer_attrs(netlist, statement, rtlil_proc);
#ifndef SLANG_MUX_LOWERING
	procedure.copy_case_tree_into(rtlil_proc->root_case);
#endif
}

};
