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

	// A signal delayed again by any number of ticks reuses the registers made for it
	// before, so the delays of all the paths of an assertion take one line of registers
	auto &timing = eval.procedural->timing;
	auto &trigger = timing.triggers[0];
	auto &line = eval.sva_delay_lines[{sig.raw(), init, trigger.signal.raw(), trigger.edge_polarity,
									   timing.background_enable.raw()}];
	while ((int) line.size() < cycles) {
		ir::Value prev = line.empty() ? sig : line.back();
		ir::Value next = eval.netlist.add_placeholder_signal(sig.size(), name_hint);
		eval.netlist.set_initialization(next, ir::Const(init, sig.size()));
		add_sva_flop(eval, prev, next, name_hint);
		line.push_back(next);
	}
	return line[cycles - 1];
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
// Adapted for the Slang AST and sv-elab under the ISC license.
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

	// `##[delay] expr`, where `##n empty` is `##(n-1) `true` and `##0 empty` has no
	// match, so there's no empty match of the whole
	Fragment parse_delayed(int start, const ast::SequenceRange& delay,
						   const ast::AssertionExpr& expr) {
		int right_start = fsm.create_node();
		add_delay(start, delay, right_start);

		Fragment right = parse(right_start, expr);
		if (right.empty) {
			if (auto shorter = shorten(delay, 1))
				add_delay(start, *shorter, right.end);
		}
		return {right.end, false};
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
	Fragment parse_repeat(int start, const ast::SequenceRange& range, ParseUnit parse_unit) {
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
		Fragment result = parse_repeat(start, repetition.range, [&](int node) {
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
		return parse_repeat(start, repetition->range,
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
					// slang gives a bare first element a delay of zero too, which is
					// not the same as an explicit `##0` if the element can be empty
					if (index == 0 && element.delayRange.start().valid())
						result = parse_delayed(start, element.delay, *element.sequence);
					else if (index == 0)
						result = parse(start, *element.sequence);
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
						// The operand is a property, an empty match of it is no success
						int operand_start = fsm.create_node();
						add_delay(start, {delay, delay}, operand_start);
						return {parse(operand_start, unary.expr).end, false};
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
			// A clock of its own, as one inside a repeated sequence instance
			eval.netlist.add_diag(diag::UnsupportedSVAFeature, expr_loc(expr));
			valid = false;
			return {start, false};

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
	// Reads a value frozen when the attempt was queued, valid for the first tick only
	bool frozen;
	slang::SourceLocation loc;

	AssertionMatch(EvalContext& eval_, ir::Value sig_,
				   slang::SourceLocation loc_ = slang::SourceLocation::NoLocation):
		eval(eval_), sig(sig_), en(ir::S1), start(0), empty(false), frozen(false), loc(loc_) {}
	AssertionMatch(const AssertionMatch&) = default;

private:
	AssertionMatch(EvalContext& eval_, ir::Value sig_, ir::Value en_, int start_, bool empty_,
				   bool frozen_, slang::SourceLocation loc_):
		eval(eval_), sig(sig_), en(en_), start(start_), empty(empty_), frozen(frozen_), loc(loc_) {}

public:
	static AssertionMatch Empty(EvalContext& eval, slang::SourceLocation loc) {
		return { eval, ir::S1, ir::S1, 0, true, false, loc };
	}

	static AssertionMatch Operand(EvalContext& eval, const ast::Expression& expr, slang::SourceLocation loc) {
		AssertionMatch ret(eval, eval.sva(expr), loc);
		ret.frozen = eval.sva_read_frozen;
		return ret;
	}

	AssertionMatch& operator=(const AssertionMatch& other) {
		sig = other.sig;
		en = other.en;
		start = other.start;
		empty = other.empty;
		frozen = other.frozen;
		loc = other.loc;
		return *this;
	}

	AssertionMatch shift(int time) const {
		if (empty)
			return { eval, sig, en, start + time, true, frozen, loc };

		ir::Value shifted_sig = sig;
		ir::Value shifted_en = en;
		if (!sig.is_fully_const())
			shifted_sig = delay_sva_sample(eval, sig, time, ir::Sx, loc);
		if (!en.is_fully_const())
			shifted_en = delay_sva_sample(eval, en, time, ir::S0, loc);
		return { eval, shifted_sig, shifted_en, start + time, false, frozen, loc };
	}

	AssertionMatch operator||(const AssertionMatch& other) const {
		assert_invariant(!empty && !other.empty);

		ir::Value gated = en.is_fully_const() && en.as_bool() ? sig : ir::Value(eval.netlist.LogicAnd(en, sig));
		ir::Value other_gated = other.en.is_fully_const() && other.en.as_bool()
									? other.sig
									: ir::Value(eval.netlist.LogicAnd(other.en, other.sig));
		ir::Value result_sig = eval.netlist.LogicOr(gated, other_gated);
		ir::Value result_en = eval.netlist.LogicOr(en, other.en);
		return { eval, result_sig, result_en, std::max(other.start, start), false, frozen || other.frozen, loc };
	}

	AssertionMatch operator&&(const AssertionMatch& other) const {
		assert_invariant(!empty && !other.empty);

		ir::Value result_en = eval.netlist.LogicAnd(en, other.en);
		int result_start = std::max(other.start, start);
		bool result_frozen = frozen || other.frozen;
		if (sig.is_fully_const() && sig.as_bool())
			return { eval, other.sig, result_en, result_start, false, result_frozen, loc };
		if (sig.is_fully_const() && !sig.as_bool())
			return { eval, ir::S0, result_en, result_start, false, result_frozen, loc };
		if (other.sig.is_fully_const() && other.sig.as_bool())
			return { eval, sig, result_en, result_start, false, result_frozen, loc };
		if (other.sig.is_fully_const() && !other.sig.as_bool())
			return { eval, ir::S0, result_en, result_start, false, result_frozen, loc };
		return { eval, eval.netlist.LogicAnd(sig, other.sig), result_en, result_start, false,
				 result_frozen, loc };
	}

	AssertionMatch operator!() const {
		assert_invariant(!empty);

		if (sig.is_fully_const()) return { eval, sig.as_bool() ? ir::S0 : ir::S1, en, start, false, frozen, loc };
		return { eval, eval.netlist.LogicNot(sig), en, start, false, frozen, loc };
	}
};

// Automatic variables and const casts keep the value they had when the attempt
// was queued (IEEE Std 1800-2017 section 16.14.6.1), while we evaluate a path at
// the tick it's placed at, so such a path can't go past the attempt's first tick.
static void check_frozen_offset(const AssertionMatch& path, int offset) {
	if (path.frozen && offset > 0)
		path.eval.netlist.add_diag(diag::SVAFrozenValueAfterFirstTick, path.loc);
}

// The disable condition of an attempt, delayed by any number of ticks
class DisableSpans {
public:
	explicit DisableSpans(const AssertionMatch& abort): delayed({abort}) {}

	// Has the condition been true on any of the ticks from `first` to `last` ago?
	AssertionMatch span(int first, int last) {
		check_frozen_offset(delayed[0], last);

		while ((int) delayed.size() <= last)
			delayed.push_back(delayed.back().shift(1));
		auto& from_first = spans[first];
		if (from_first.empty())
			from_first.push_back(delayed[first]);
		while ((int) from_first.size() <= last - first)
			from_first.push_back(from_first.back() || delayed[first + from_first.size()]);
		return AssertionMatch(delayed[0].eval, from_first[last - first].sig, delayed[0].loc);
	}

private:
	std::vector<AssertionMatch> delayed;
	std::map<int, std::vector<AssertionMatch>> spans;
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

	// A boolean on the first tick of the operand
	AssertionMatch atom(const AssertionMatch& boolean) const {
		if (settled)
			return AssertionMatch(boolean.eval, dual ? ir::S0 : ir::S1, boolean.loc);

		// A constant can have the value of the letter to begin with
		if (abort == nullptr || (boolean.sig.is_fully_const() && boolean.sig.as_bool() != dual))
			return boolean;

		auto now = abort->span(0, 0);
		return dual ? boolean && !now : boolean || now;
	}

	// A path of the 1'b1s of a delay, `length` ticks long. Only the last one counts, as
	// the next boolean in a path sees the disable over the ticks before it as well.
	AssertionMatch true_path(EvalContext& eval, int length, slang::SourceLocation loc) const {
		AssertionMatch path = AssertionMatch(eval, ir::S1, loc).shift(length);
		if (!dual || (abort == nullptr && !settled))
			return path;
		if (settled)
			return path && AssertionMatch(eval, ir::S0, loc);
		return path && !abort->span(0, length);
	}
};

// Paths of an operand by the tick it starts on, counted from the start of the operand
// whose part it is. If the disable came ahead of that tick, the operand gets settled to
// the letter. Otherwise it's as if the operand started off the attempt, so we synthesize
// those two cases once each and pick per tick.
class OperandPaths {
public:
	using Synthesize = std::function<std::vector<AssertionMatch>(const AttemptContext&)>;

	OperandPaths(const AttemptContext& ctx_, Synthesize synthesize_):
		ctx(ctx_), synthesize(std::move(synthesize_)), from_start(synthesize(ctx)) {}

	const std::vector<AssertionMatch>& at(int offset) {
		if (offset == 0 || ctx.abort == nullptr || ctx.settled)
			return from_start;

		auto it = by_offset.find(offset);
		if (it != by_offset.end())
			return it->second;

		if (!settled_paths.has_value()) {
			AttemptContext settled_ctx = ctx;
			settled_ctx.settled = true;
			settled_paths = synthesize(settled_ctx);
			assert_invariant(settled_paths->size() == from_start.size());
		}

		std::vector<AssertionMatch> paths = from_start;
		for (size_t i = 0; i < paths.size(); i++) {
			AssertionMatch& path = paths[i];
			const AssertionMatch& other = (*settled_paths)[i];
			assert_invariant(path.empty == other.empty && path.start == other.start);
			if (path.empty || path.sig == other.sig)
				continue;

			// Reads the disable from before the operand start, on the tick the path ends
			auto before = ctx.abort->span(path.start + 1, path.start + offset);
			auto& netlist = path.eval.netlist;
			path.sig = netlist.Mux(netlist.ReduceBool(path.sig), netlist.ReduceBool(other.sig), before.sig.as_net());
		}
		return by_offset.emplace(offset, std::move(paths)).first->second;
	}

private:
	AttemptContext ctx;
	Synthesize synthesize;
	std::vector<AssertionMatch> from_start;
	std::optional<std::vector<AssertionMatch>> settled_paths;
	std::map<int, std::vector<AssertionMatch>> by_offset;
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
static std::vector<AssertionMatch> property_paths(EvalContext& eval, const AttemptContext& ctx,
												  std::vector<AssertionMatch> paths,
												  slang::SourceLocation loc) {
	std::erase_if(paths, [](const AssertionMatch& path) { return path.empty; });
	if (paths.empty())
		paths.push_back(ctx.atom(AssertionMatch(eval, ir::S0, loc)));
	return paths;
}

// Negation of paths synthesized in the context `ctx`
static std::vector<AssertionMatch> not_vec(EvalContext& eval, const AttemptContext& ctx,
										   std::vector<AssertionMatch> in, slang::SourceLocation loc) {
	return { !collapse_or(property_paths(eval, ctx, std::move(in), loc)) };
}

static ir::Value gated_signal(const AssertionMatch& path) {
	assert_invariant(!path.empty);

	if (path.sig.is_fully_const())
		return path.sig.as_bool() ? path.en : ir::Value(ir::S0);
	return path.eval.netlist.LogicAnd(path.en, path.sig);
}

// `a` being synthesized in the context `ctx`
static std::vector<AssertionMatch> seq_vec(const AttemptContext& ctx, std::vector<AssertionMatch> a,
										   int min, int max, OperandPaths& b) {
	std::vector<AssertionMatch> new_own_paths;
	for (auto path : a) {
		for (int offset = min; offset <= max; offset++) {
			// `empty ##0 seq` has no match
			if (path.empty && offset == 0)
				continue;

			for (auto inner : b.at(path.empty ? offset - 1 : path.start + offset)) {
				if (path.empty && inner.empty) {
					// `empty ##N empty` spans N - 1 ticks
					if (offset == 1)
						new_own_paths.push_back(path);
					else
						new_own_paths.push_back(ctx.true_path(path.eval, offset - 2, path.loc));
					continue;
				}
				if (path.empty) {
					// `empty ##N seq` is `##(N-1) seq`
					check_frozen_offset(inner, offset - 1);
					new_own_paths.push_back(AssertionMatch(path.eval, ir::S1, path.loc).shift(offset - 1 + inner.start) && inner);
					continue;
				}
				if (inner.empty) {
					// `seq ##N empty` is `seq ##(N-1) 1'b1`
					if (offset > 0)
						new_own_paths.push_back(path.shift(offset - 1) && ctx.true_path(path.eval, path.start + offset - 1, path.loc));
					continue;
				}
				check_frozen_offset(inner, path.start + offset);
				new_own_paths.push_back(path.shift(offset + inner.start) && inner);
			}
		}
	}
	return compress_paths(new_own_paths);
}

static std::vector<AssertionMatch> seq_prefix_vec(EvalContext& eval, const AttemptContext& ctx, int min, int max,
												  OperandPaths& b, slang::SourceLocation loc) {
	std::vector<AssertionMatch> paths;
	for (int offset = min; offset <= max; offset++) {
		for (auto inner : b.at(offset)) {
			if (inner.empty) {
				// `##N empty` is `##(N-1) 1'b1`, for N = 0 there's no match
				if (offset > 0)
					paths.push_back(ctx.true_path(eval, offset - 1, loc));
			} else {
				check_frozen_offset(inner, offset);
				paths.push_back(AssertionMatch(eval, ir::S1, loc).shift(offset + inner.start) && inner);
			}
		}
	}
	return compress_paths(paths);
}

static std::vector<AssertionMatch> apply_repetition(EvalContext& eval, const AttemptContext& ctx,
													const ast::AssertionExpr& expr,
													std::optional<ast::SequenceRepetition> repetition,
													OperandPaths& operand) {
	if (!repetition.has_value())
		return operand.at(0);

	if (repetition->kind != ast::SequenceRepetition::Consecutive || !repetition->range.max.has_value()) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		return {};
	}

	std::vector<AssertionMatch> repeated;
	if (repetition->range.min == 0)
		repeated.push_back(AssertionMatch::Empty(eval, expr_loc(expr)));

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

// A cover has no letters to extend a match by. It takes a match that ran its full course
// with no disable from the attempt start up to the end of that match.
static std::vector<AssertionMatch> cover_disable_iff(EvalContext& eval, const ast::Expression& condition,
													 std::vector<AssertionMatch> inner,
													 slang::SourceLocation loc) {
	auto disable = AssertionMatch::Operand(eval, condition, loc);

	int max_start = 0;
	for (auto path : inner)
		max_start = std::max(max_start, path.start);
	check_frozen_offset(disable, max_start);

	std::vector<AssertionMatch> disable_windows = { disable };
	auto delayed_disable = disable;
	for (int i = 1; i <= max_start; i++) {
		delayed_disable = delayed_disable.shift(1);
		disable_windows.push_back(disable_windows.back() || delayed_disable);
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

// An empty result stands for a sequence with no possible match. Constructs we
// can't lower also yield an empty result, but those have raised an error
// diagnostic so the design won't be read in.
static std::vector<AssertionMatch> synthesize_assertion_expr(EvalContext& eval, const AttemptContext& ctx,
															 const ast::AssertionExpr& expr) {
	switch (expr.kind) {
		case slang::ast::AssertionExprKind::Invalid:
			assert_invariant(false && "unreachable");
		case slang::ast::AssertionExprKind::Simple:
			{
				const auto& simple = expr.as<ast::SimpleAssertionExpr>();
				OperandPaths operand(ctx, [&](const AttemptContext& at) -> std::vector<AssertionMatch> {
					if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
						const auto& instance = simple.expr.as<ast::AssertionInstanceExpression>();
						if (instance.isRecursiveProperty) {
							eval.netlist.add_diag(diag::AssertionUnsupported, simple.expr.sourceRange);
							return {};
						}
						return synthesize_assertion_expr(eval, at, instance.body);
					}
					return { at.atom(simple.isNullExpr ? AssertionMatch(eval, ir::S0, expr_loc(expr))
													   : AssertionMatch::Operand(eval, simple.expr, expr_loc(expr))) };
				});
				return apply_repetition(eval, ctx, expr, simple.repetition, operand);
			}
		case slang::ast::AssertionExprKind::SequenceConcat:
			{
				const auto& sequence = expr.as<ast::SequenceConcatExpr>();
				std::vector<AssertionMatch> own_paths;
				for (size_t i = 0; i < sequence.elements.size(); i++) {
					const auto& element = sequence.elements[i];
					OperandPaths inner_paths(ctx, [&](const AttemptContext& at) {
						return synthesize_assertion_expr(eval, at, *element.sequence);
					});

					auto delay = element.delay;
					if (!delay.max.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
						return {};
					}

					// slang gives a bare first element a delay of zero too, which is
					// not the same as an explicit `##0` if the element can be empty
					if (i != 0)
						own_paths = seq_vec(ctx, own_paths, delay.min, delay.max.value(), inner_paths);
					else if (element.delayRange.start().valid())
						own_paths = seq_prefix_vec(eval, ctx, delay.min, delay.max.value(), inner_paths, expr_loc(expr));
					else
						own_paths = compress_paths(inner_paths.at(0));
				}
				return own_paths;
			}
		case slang::ast::AssertionExprKind::Unary:
			{
				const auto& uop = expr.as<ast::UnaryAssertionExpr>();
				switch (uop.op) {
				case slang::ast::UnaryAssertionOperator::Not:
					return not_vec(eval, ctx.negated(), synthesize_assertion_expr(eval, ctx.negated(), uop.expr), expr_loc(expr));

				case slang::ast::UnaryAssertionOperator::Eventually:
				case slang::ast::UnaryAssertionOperator::SEventually:
				{
					if (!uop.range.has_value() || !uop.range->max.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
						return {};
					}

					std::vector<AssertionMatch> true_path = {{eval, ir::S1, expr_loc(expr)}};
					OperandPaths operand(ctx, [&](const AttemptContext& at) {
						return property_paths(eval, at, synthesize_assertion_expr(eval, at, uop.expr), expr_loc(expr));
					});
					return seq_vec(ctx, true_path, uop.range->min, uop.range->max.value(), operand);
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
						delay = uop.range->min;
					}

					std::vector<AssertionMatch> true_path = {{eval, ir::S1, expr_loc(expr)}};
					OperandPaths operand(ctx, [&](const AttemptContext& at) {
						return property_paths(eval, at, synthesize_assertion_expr(eval, at, uop.expr), expr_loc(expr));
					});
					return seq_vec(ctx, true_path, delay, delay, operand);
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

				bool implication = biop.op == ast::BinaryAssertionOperator::OverlappedImplication ||
								   biop.op == ast::BinaryAssertionOperator::NonOverlappedImplication;
				bool followed_by = biop.op == ast::BinaryAssertionOperator::OverlappedFollowedBy ||
								   biop.op == ast::BinaryAssertionOperator::NonOverlappedFollowedBy;
				auto left_ctx = implication ? ctx.negated() : ctx;

				OperandPaths left_at(left_ctx, [&](const AttemptContext& at) {
					return synthesize_assertion_expr(eval, at, biop.left);
				});
				// `R |-> P` is `not (R ##0 not P)`, and `R |=> P` the same with a `##1`.
				// `R #-# P` is `not (R |-> not P)`, so `R ##0 P` with P used as a property.
				OperandPaths right_at(ctx, [&](const AttemptContext& at) {
					auto paths = synthesize_assertion_expr(eval, at, biop.right);
					if (implication)
						return not_vec(eval, at, std::move(paths), expr_loc(expr));
					if (followed_by)
						return property_paths(eval, at, std::move(paths), expr_loc(expr));
					return paths;
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
						left = property_paths(eval, ctx, std::move(left), expr_loc(expr));
						right = property_paths(eval, ctx, std::move(right), expr_loc(expr));
					}

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

						check_frozen_offset(condition, path.start);
						AssertionMatch guarded = path;
						for (int t = 0; t <= path.start; t++) {
							auto condition_at_t = collapse_or(left_at.at(path.start - t)).shift(t);
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
				case ast::BinaryAssertionOperator::NonOverlappedImplication:
				{
					int delay = biop.op == ast::BinaryAssertionOperator::NonOverlappedImplication ? 1 : 0;
					return not_vec(eval, left_ctx, seq_vec(left_ctx, left, delay, delay, right_at), expr_loc(expr));
				}
				case ast::BinaryAssertionOperator::OverlappedFollowedBy:
				case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
				{
					int delay = biop.op == ast::BinaryAssertionOperator::NonOverlappedFollowedBy ? 1 : 0;
					return seq_vec(ctx, left, delay, delay, right_at);
				}

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
			// eval_assertion() handles the one a property is allowed, at the top of it.
			// slang only rejects the others in direct instances of a property.
			eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return {};

		case slang::ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, expr_loc(expr));
					return {};
				}

				OperandPaths operand(ctx, [&](const AttemptContext& at) {
					return synthesize_assertion_expr(eval, at, with_match.expr);
				});
				return apply_repetition(eval, ctx, expr, with_match.repetition, operand);
			}
		case slang::ast::AssertionExprKind::FirstMatch:
			{
				const auto& first_match = expr.as<ast::FirstMatchAssertionExpr>();
				if (!first_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, expr_loc(expr));
					return {};
				}

				auto paths = compress_paths(synthesize_assertion_expr(eval, ctx, first_match.seq));
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

struct AssertionResult {
	ir::Value a;
	ir::Value en;
};

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
	uint64_t duration;
};

static void add_liveness_obligation(PropertyResult& result, EvalContext& eval,
									ir::Net goal, ir::Net trigger,
									ir::Net disable, slang::SourceLocation loc);
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
			// A repeated instance follows its own unbounded delay with another
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.expr.kind != ast::ExpressionKind::AssertionInstance ||
				simple.repetition.has_value())
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

	// The disable is up to the monitor. The operand is used as a property, so an
	// empty match of it meets nothing.
	AttemptContext ctx;
	auto paths = property_paths(eval, ctx, synthesize_assertion_expr(eval, ctx, sequence),
								expr_loc(sequence));

	std::vector<EventualCandidate> candidates;
	for (const auto& path : compress_paths(paths)) {
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
			{match, uint64_t(path.start) + leading_delay});
	}
	return candidates;
}

static std::optional<std::vector<EventualCandidate>>
suffix_eventual_candidates(EvalContext& eval,
						   const ast::AssertionExpr& sequence,
						   uint32_t additional_delay = 0) {
	if (sequence.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = sequence.as<ast::SimpleAssertionExpr>();
		if (simple.expr.kind != ast::ExpressionKind::AssertionInstance ||
			simple.repetition.has_value())
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

	// The disable is up to the monitor
	AttemptContext ctx;
	auto paths = synthesize_assertion_expr(
		eval, ctx, *concat.elements[0].sequence);
	for (size_t i = 1; i < concat.elements.size(); i++) {
		const auto& element = concat.elements[i];
		if (!element.delay.max.has_value() ||
			assertion_has_unbounded_sequence(*element.sequence))
			return std::nullopt;
		OperandPaths element_paths(ctx, [&](const AttemptContext& at) {
			return synthesize_assertion_expr(eval, at, *element.sequence);
		});
		paths = seq_vec(ctx, paths, element.delay.min, element.delay.max.value(),
						element_paths);
	}

	uint64_t leading_delay =
		uint64_t(additional_delay) + concat.elements[0].delay.min;
	std::vector<EventualCandidate> candidates;
	for (const auto& path : compress_paths(paths)) {
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
			{match, uint64_t(path.start) + leading_delay});
	}
	return candidates;
}

static void add_response_liveness_obligation(
		PropertyResult& result, EvalContext& eval,
		const std::vector<EventualCandidate>& candidates,
		ir::Net trigger, ir::Net disable,
		slang::SourceLocation loc) {
	uint64_t maximum_duration = 0;
	ir::Net any_match = ir::S0;
	for (const auto& candidate : candidates) {
		maximum_duration = std::max(maximum_duration, candidate.duration);
		any_match = eval.netlist.LogicOr(any_match, candidate.match);
	}

	// A zero-duration goal cannot have started before its trigger, so the
	// native $live/$fair event semantics are already exact.
	if (maximum_duration == 0) {
		add_liveness_obligation(result, eval, any_match, trigger, disable, loc);
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
	for (uint64_t age = 1; age < maximum_duration; age++) {
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
	for (uint64_t age = 0; age < maximum_duration; age++) {
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
	for (size_t age = 0; age < ages.size(); age++) {
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
	for (size_t i = 0; i < recent_wires.size(); i++) {
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

// `admits_empty` reports an empty match of the sequence
static std::optional<ir::Net>
antecedent_match_event(EvalContext& eval, const ast::AssertionExpr& sequence,
					   ir::Net trigger, bool nonoverlapped,
					   ir::Net disable, bool *admits_empty = nullptr) {
	ir::Net result = ir::S0;
	bool empty = false;
	if (assertion_has_unbounded_sequence(sequence)) {
		auto match = synthesize_unbounded_sequence(
			eval, sequence, trigger, disable, &empty);
		if (!match.has_value())
			return std::nullopt;
		result = match.value();
	} else {
		// The disable is up to the monitor
		auto paths = synthesize_assertion_expr(eval, AttemptContext(), sequence);
		for (const auto& path : compress_paths(paths)) {
			if (path.empty) {
				empty = true;
				continue;
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
	if (admits_empty)
		*admits_empty = empty;
	// `seq |-> p` has no tick to start `p` on after an empty match, but
	// `seq |=> p` is `(seq ##1 `true) |-> p` where it matches on the first tick
	if (nonoverlapped) {
		result = delayed_abortable_trigger(
			eval, result, 1, disable, expr_loc(sequence));
		if (empty)
			result = eval.netlist.LogicOr(result, trigger);
	}
	return eval.netlist.LogicAnd(result, sva_not(eval, disable));
}

static void add_liveness_obligation(PropertyResult& result, EvalContext& eval,
									ir::Net goal, ir::Net trigger,
									ir::Net disable, slang::SourceLocation loc) {
	// A disable aborts all currently pending attempts, and prevents a new
	// attempt from starting in the disabled cycle.
	goal = eval.netlist.LogicOr(goal, disable);
	trigger = eval.netlist.LogicAnd(trigger, sva_not(eval, disable));
	ir::Net active = add_pending_monitor(eval, trigger, goal, ir::S0, loc);
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
		if (simple.repetition.has_value())
			return std::nullopt;
		if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
			const auto& instance =
				simple.expr.as<ast::AssertionInstanceExpression>();
			if (instance.isRecursiveProperty)
				return std::nullopt;
			return simple_property_signal(eval, instance.body, negate);
		}
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

// Does `value` match `item` of a property case? They are compared by case equality,
// not with the wildcards of `case inside` (IEEE Std 1800-2017 sections 12.5 and
// 16.12.16). Only a constant carries an x, so it never equals a net that isn't one.
static ir::Net case_equality(EvalContext& eval, ir::Value value, ir::Value item) {
	assert_invariant(value.size() == item.size());
	ir::Value value_bits, item_bits;
	for (uint64_t i = 0; i < value.size(); i++) {
		ir::Net left = value[i], right = item[i];
		if (left.is_const() && right.is_const()) {
			if (left != right)
				return ir::S0;
			continue;
		}
		if ((left.is_const() && !left.is_def()) || (right.is_const() && !right.is_def()))
			return ir::S0;
		value_bits.append(left);
		item_bits.append(right);
	}
	return value_bits.empty() ? ir::S1 : eval.netlist.Eq(value_bits, item_bits);
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

// High on the last tick of a non-empty match of the sequence the selected attempt
// started. An empty match has no such tick, `admits_empty` reports it.
static std::optional<ir::Net> synthesize_selected_sequence(
		EvalContext& eval, const ast::AssertionExpr& sequence,
		ir::Net trigger, ir::Net disable, bool& admits_empty);

static std::optional<ir::Net> synthesize_selected_sequence(
		EvalContext& eval, const ast::AssertionExpr& sequence,
		ir::Net trigger, ir::Net disable, bool& admits_empty) {
	admits_empty = false;
	if (sequence.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = sequence.as<ast::SimpleAssertionExpr>();
		if (!simple.repetition.has_value() &&
			simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
			const auto& instance =
				simple.expr.as<ast::AssertionInstanceExpression>();
			if (!instance.isRecursiveProperty)
				return synthesize_selected_sequence(
					eval, instance.body, trigger, disable, admits_empty);
		}
	}
	if (sequence.kind == ast::AssertionExprKind::FirstMatch) {
		const auto& first = sequence.as<ast::FirstMatchAssertionExpr>();
		if (!first.matchItems.empty()) {
			eval.netlist.add_diag(diag::SVAMatchItemsUnsupported,
								  expr_loc(sequence));
			return std::nullopt;
		}
		auto match = synthesize_selected_sequence(
			eval, first.seq, trigger, disable, admits_empty);
		// An empty match comes ahead of any other, so it's the only first match
		if (match.has_value() && admits_empty)
			return ir::S0;
		return match;
	}
	if (sequence.kind == ast::AssertionExprKind::Binary) {
		const auto& binary = sequence.as<ast::BinaryAssertionExpr>();
		bool left_empty = false;
		bool right_empty = false;
		switch (binary.op) {
		case ast::BinaryAssertionOperator::Or:
			{
				auto left = synthesize_selected_sequence(
					eval, binary.left, trigger, disable, left_empty);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable, right_empty);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				admits_empty = left_empty || right_empty;
				return eval.netlist.LogicOr(left.value(), right.value());
			}
		case ast::BinaryAssertionOperator::And:
			{
				auto left = synthesize_selected_sequence(
					eval, binary.left, trigger, disable, left_empty);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable, right_empty);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				admits_empty = left_empty && right_empty;
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
					eval, binary.left, trigger, disable, left_empty);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable, right_empty);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				admits_empty = left_empty && right_empty;
				return eval.netlist.LogicAnd(left.value(), right.value());
			}
		case ast::BinaryAssertionOperator::Within:
			{
				ir::Net delayed_start = add_sticky_observation(
					eval, trigger, disable, expr_loc(binary.left),
					"sva_within_start");
				auto inner = synthesize_selected_sequence(
					eval, binary.left, delayed_start, disable, left_empty);
				auto outer = synthesize_selected_sequence(
					eval, binary.right, trigger, disable, right_empty);
				if (!inner.has_value() || !outer.has_value())
					return std::nullopt;
				admits_empty = left_empty && right_empty;
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
					eval, binary.right, trigger, disable, admits_empty);
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
		eval, sequence, trigger, disable, &admits_empty);
}

static PropertyResult lower_property(EvalContext& eval, const ast::AssertionExpr& expr,
									 ir::Net trigger = ir::S1,
									 ir::Net disable = ir::S0);

static PropertyResult lower_finite_property_from_trigger(
		EvalContext& eval, const ast::AssertionExpr& expr,
		ir::Net trigger, ir::Net disable) {
	PropertyResult result;
	// The disable is up to the monitor
	AttemptContext ctx;
	auto paths = property_paths(eval, ctx, synthesize_assertion_expr(eval, ctx, expr), expr_loc(expr));
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
			// A repeated instance is a sequence
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.expr.kind != ast::ExpressionKind::AssertionInstance ||
				simple.repetition.has_value())
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

// The matches that meet a direct justice property, for a response monitor to
// discharge its attempts by
static std::optional<std::vector<EventualCandidate>>
justice_candidates(EvalContext& eval, const ast::AssertionExpr& expr) {
	if (expr.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = expr.as<ast::SimpleAssertionExpr>();
		return justice_candidates(
			eval, simple.expr.as<ast::AssertionInstanceExpression>().body);
	}

	const auto& unary = expr.as<ast::UnaryAssertionExpr>();
	if (unary.op == ast::UnaryAssertionOperator::Not) {
		const auto& nested = unary.expr.as<ast::UnaryAssertionExpr>();
		auto violation = simple_property_signal(eval, nested.expr, true);
		if (!violation.has_value()) {
			eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return std::nullopt;
		}
		uint32_t minimum = nested.range.has_value() ? nested.range->min : 0;
		return std::vector<EventualCandidate>{{violation.value(), minimum}};
	}

	uint32_t minimum = unary.range.has_value() ? unary.range->min : 0;
	if (!assertion_has_unbounded_sequence(unary.expr))
		return finite_eventual_candidates(eval, unary.expr, minimum);
	if (!is_suffix_eventual_sequence(unary.expr)) {
		// Every attempt would need a selection of its own
		eval.netlist.add_diag(diag::SVALivenessAcceptanceUnsupported, expr_loc(expr));
		return std::nullopt;
	}
	return suffix_eventual_candidates(eval, unary.expr, minimum);
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
		// The operand is used as a property, so an empty match of it meets nothing
		bool admits_empty;
		auto goal = synthesize_selected_sequence(
			eval, eventual.expr, future_start, disable, admits_empty);
		if (!goal.has_value())
			return {{}, false};
		add_liveness_obligation(
			result, eval, goal.value(), selected, disable, expr_loc(eventual));
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
		add_liveness_obligation(result, eval, right.value(), trigger, disable,
								expr_loc(until_expr));
	return result;
}

static PropertyResult lower_property(EvalContext& eval, const ast::AssertionExpr& expr,
									 ir::Net trigger, ir::Net disable) {
	if (expr.kind == ast::AssertionExprKind::DisableIff) {
		const auto& disabled = expr.as<ast::DisableIffAssertionExpr>();
		ir::Net condition =
			eval.netlist.ReduceBool(eval.sva(disabled.condition));
		return lower_property(eval, disabled.expr, trigger,
							  eval.netlist.LogicOr(disable, condition));
	}

	// The body of a repeated instance is only one of the iterations, the repetition
	// is lowered as a sequence along with it
	if (expr.kind == ast::AssertionExprKind::Simple) {
		const auto& simple = expr.as<ast::SimpleAssertionExpr>();
		if (simple.expr.kind == ast::ExpressionKind::AssertionInstance &&
			!simple.repetition.has_value()) {
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
			// An empty match is no success of the sequence used as a property
			bool admits_empty;
			auto goal = synthesize_selected_sequence(
				eval, strong_weak.expr, selected, disable, admits_empty);
			if (!goal.has_value())
				return {{}, false};
			add_liveness_obligation(
				result, eval, goal.value(), selected, disable, expr_loc(expr));
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
		// The condition of a synchronous abort is a sampled value like the operands
		// (IEEE Std 1800-2017 section 16.12.14), that of an asynchronous one is still
		// read as the current value
		ir::Net condition = eval.netlist.ReduceBool(
			abort.isSync ? eval.sva(abort.condition) : eval(abort.condition));
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
			eval.netlist.ReduceBool(eval.sva(conditional.condition));
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
		ir::Value control = eval.sva(case_expr.expr);
		ir::Net matched = ir::S0;
		PropertyResult result;
		for (const auto& item : case_expr.items) {
			ir::Net item_hit = ir::S0;
			for (const auto *item_expr : item.expressions)
				item_hit = eval.netlist.LogicOr(
					item_hit,
					case_equality(eval, control, eval.sva(*item_expr)));
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
				bool admits_empty = false;
				auto antecedent = antecedent_match_event(
					eval, binary.left, trigger, nonoverlapped, disable, &admits_empty);
				if (!antecedent.has_value())
					return {{}, false};
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
				} else if (!(nonoverlapped && admits_empty)) {
					// An empty match of R is none for `R #-# P`, while `R #=# P` is
					// `(R ##1 `true) #-# P`, which it matches on the attempt's tick
					result = lower_finite_property_from_trigger(
						eval, binary.left, trigger, disable);
				}
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
					!is_direct_justice_property(binary.right)) {
					eval.netlist.add_diag(
						diag::SVALivenessAcceptanceUnsupported,
						expr_loc(expr));
					return {{}, false};
				}
				// One monitor takes the matches of both sides, an attempt is met by a
				// match of either that starts late enough for it
				auto candidates = justice_candidates(eval, binary.left);
				auto right = justice_candidates(eval, binary.right);
				if (!candidates.has_value() || !right.has_value())
					return {{}, false};
				candidates->insert(candidates->end(), right->begin(), right->end());
				PropertyResult result;
				add_response_liveness_obligation(
					result, eval, *candidates, trigger, disable, expr_loc(expr));
				return result;
			}
		default:
			break;
		}
	}

	return lower_finite_property_from_trigger(
		eval, expr, trigger, disable);
}

// Does lower_property() take nothing of `expr` apart, but lower all of it by its paths?
// Such a property gets lowered by eval_assertion(), which keeps to Annex F for a disable
// and queues attempts as of the procedural condition around the assertion.
static bool lowered_by_paths(const ast::AssertionExpr& expr) {
	const ast::AssertionExpr& top = unwrap_property_instances(expr);
	switch (top.kind) {
	case ast::AssertionExprKind::DisableIff:
		return lowered_by_paths(top.as<ast::DisableIffAssertionExpr>().expr);

	case ast::AssertionExprKind::Unary:
		{
			const auto& unary = top.as<ast::UnaryAssertionExpr>();
			switch (unary.op) {
			case ast::UnaryAssertionOperator::Eventually:
			case ast::UnaryAssertionOperator::SEventually:
				return unary.range.has_value() && unary.range->max.has_value();
			case ast::UnaryAssertionOperator::Always:
			case ast::UnaryAssertionOperator::SAlways:
				return false;
			case ast::UnaryAssertionOperator::Not:
				{
					const auto *nested = unary.expr.as_if<ast::UnaryAssertionExpr>();
					if (!nested)
						return true;
					if (nested->op == ast::UnaryAssertionOperator::Always ||
						nested->op == ast::UnaryAssertionOperator::SAlways)
						return false;
					return !((nested->op == ast::UnaryAssertionOperator::Eventually ||
							  nested->op == ast::UnaryAssertionOperator::SEventually) &&
							 (!nested->range.has_value() || !nested->range->max.has_value()));
				}
			default:
				return true;
			}
		}

	case ast::AssertionExprKind::StrongWeak:
	case ast::AssertionExprKind::Abort:
	case ast::AssertionExprKind::Conditional:
	case ast::AssertionExprKind::Case:
		return false;

	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = top.as<ast::BinaryAssertionExpr>();
			switch (binary.op) {
			case ast::BinaryAssertionOperator::Implies:
			case ast::BinaryAssertionOperator::Iff:
			case ast::BinaryAssertionOperator::Until:
			case ast::BinaryAssertionOperator::SUntil:
			case ast::BinaryAssertionOperator::UntilWith:
			case ast::BinaryAssertionOperator::SUntilWith:
				return false;
			case ast::BinaryAssertionOperator::OverlappedImplication:
			case ast::BinaryAssertionOperator::NonOverlappedImplication:
				return !property_requires_temporal_obligations(binary.right) &&
					   !assertion_has_unbounded_sequence(binary.right) &&
					   !assertion_has_unbounded_sequence(binary.left);
			case ast::BinaryAssertionOperator::OverlappedFollowedBy:
			case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
				return !property_requires_temporal_obligations(binary.right) &&
					   !assertion_has_unbounded_sequence(binary.left);
			case ast::BinaryAssertionOperator::And:
				return !property_requires_temporal_obligations(binary.left) &&
					   !property_requires_temporal_obligations(binary.right) &&
					   !assertion_has_unbounded_sequence(binary.left) &&
					   !assertion_has_unbounded_sequence(binary.right);
			case ast::BinaryAssertionOperator::Or:
				return !property_requires_temporal_obligations(binary.left) &&
					   !property_requires_temporal_obligations(binary.right);
			default:
				return true;
			}
		}

	default:
		return true;
	}
}

// `attempt` is the condition of the procedural code around the assertion. It's read
// when the statement is reached and decides if an attempt is queued, the attempts
// in flight run to completion regardless (IEEE Std 1800-2017 section 16.14.6).
static AssertionResult eval_assertion(EvalContext& eval, const ast::AssertionExpr& assertion,
									  ir::Net attempt, bool cover) {
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

	AttemptContext ctx;
	std::optional<DisableSpans> spans;
	if (condition && !cover) {
		auto abort = AssertionMatch::Operand(eval, *condition, disable_loc);
		abort.sig = eval.netlist.ReduceBool(abort.sig);
		if (!abort.sig.is_fully_zero())
			ctx.abort = &spans.emplace(abort);
	}

	auto paths = property_paths(eval, ctx, synthesize_assertion_expr(eval, ctx, *property), expr_loc(assertion));
	if (condition && cover)
		paths = cover_disable_iff(eval, *condition, std::move(paths), disable_loc);

	auto sig = collapse_or(paths);

	// Starts out low, so no attempt gets reported before it had the time to finish
	auto attempted = delay_sva_sample(eval, attempt, sig.start, ir::S0, sig.loc);
	return { sig.sig, eval.netlist.LogicAnd(attempted, sig.en) };
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

// The name for the cell checking `statement`
static std::string check_cell_name(NetlistContext &netlist,
								   const ast::ConcurrentAssertionStatement &statement,
								   const ast::StatementBlockSymbol *block)
{
	// If we are the sole statement in a block, use the block's label. Loops can
	// get us here more than once, a suffix tells those apart.
	if (block && unwrap_statement(block->tryGetStatement()) == &statement && !block->name.empty())
		return netlist.backend->canvas->uniquify(netlist.id(*block)).str();
	return netlist.backend->new_id();
}

// Lower a property that lower_property() takes apart, to a check for each of its
// obligations
static void lower_sva_obligations(const ast::ConcurrentAssertionStatement &statement,
								  const ast::StatementBlockSymbol *block,
								  ProceduralContext &procedural, const ast::AssertionExpr &expr,
								  ir::Net attempt)
{
	auto &netlist = procedural.netlist;

	// Obligations state what has to keep holding, which isn't a point in time
	// when the property has succeeded for a cover to observe
	if (statement.assertionKind == ast::AssertionKind::CoverProperty &&
			property_requires_temporal_obligations(expr)) {
		netlist.add_diag(diag::SVATemporalCoverUnsupported, statement.sourceRange);
		return;
	}

	ir::Net disable = ir::S0;
	if (!ast::DisableIffAssertionExpr::isKind(unwrap_property_instances(expr).kind) &&
			netlist.process_scope) {
		auto scope = netlist.process_scope;
		if (auto condition = scope->getCompilation().getDefaultDisable(*scope))
			disable = netlist.ReduceBool(procedural.eval.sva(*condition));
	}

	// The monitors start an attempt where the procedural condition holds, the attempts
	// then run to completion regardless of it
	PropertyResult result = lower_property(procedural.eval, expr, attempt, disable);
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

	std::string base_cell_name = check_cell_name(netlist, statement, block);

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

		// A justice goal is looked at on every cycle, not only on the ones the clock of
		// an `iff` qualified process ticks on
		if (obligation.kind != PropertyObligationKind::Safety)
			a = netlist.LogicAnd(a, procedural.timing.background_enable);

		auto cell = netlist.backend->canvas->addCell(std::move(cell_name), ID($check));
		procedural.timing.extract_trigger(netlist, cell, en);
		cell->setParam(ID::FLAVOR, flavor);
		cell->setParam(ID::FORMAT, std::string(""));
		cell->setParam(ID::ARGS_WIDTH, 0);
		cell->setParam(ID::PRIORITY, --procedural.effects_priority);
		cell->setPort(ID::ARGS, {});
		cell->setPort(ID::A, {a});
		transfer_attrs(netlist, statement, cell);
	}
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

	if (!lowered_by_paths(*expr)) {
		lower_sva_obligations(statement, block, procedural, *expr, attempt);
		return;
	}

	AssertionResult result = eval_assertion(procedural.eval, *expr, attempt,
											statement.assertionKind == ast::AssertionKind::CoverProperty);

	std::string flavor;
	switch (statement.assertionKind) {
	case ast::AssertionKind::Assert:        flavor = "assert"; break;
	case ast::AssertionKind::Assume:        flavor = "assume"; break;
	case ast::AssertionKind::CoverProperty: flavor = "cover"; break;
	default:                                netlist.add_diag(diag::AssertionUnsupported, statement.sourceRange); return;
	}

	std::string cell_name = check_cell_name(netlist, statement, block);
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
