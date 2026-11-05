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

static ir::Value delay_sva_sample(EvalContext& eval, ir::Value sig, int64_t cycles, ir::Trit init,
								  slang::SourceLocation loc = slang::SourceLocation::NoLocation,
								  std::string_view name_hint = "sva_delay") {
	if (cycles == 0)
		return sig;

	if (!sva_clock_ready(eval, loc))
		return ir::Value(ir::Sx, sig.size());

	// A signal delayed again by any number of ticks reuses the registers made for it
	// before, so a value sampled by the assertions of a module takes one line of
	// registers, however many paths read it on however many ticks (see SvaFormulas)
	auto &timing = eval.procedural->timing;
	auto &trigger = timing.triggers[0];
	auto &lines = eval.netlist.backend->sva_delay_lines;
	auto &line = lines[{sig.raw(), init, trigger.signal.raw(), trigger.edge_polarity,
						timing.background_enable.raw()}];
	while ((int64_t) line.size() < cycles) {
		ir::Value prev = line.empty() ? sig : line.back();
		ir::Value next = eval.netlist.add_placeholder_signal(sig.size(), name_hint);
		eval.netlist.set_initialization(next, ir::Const(init, sig.size()));
		add_sva_flop(eval, prev, next, name_hint);
		line.push_back(next);
	}
	return line[cycles - 1];
}

static bool assertion_has_unbounded_sequence(const ast::AssertionExpr& expr);

// ---- Begin of the portion adapted from Yosys ----
//
// SvaNfaNode, SvaSequenceNfa, SvaSequenceBuilder and synthesize_unbounded_sequence()
// are adapted from the SvaFsm class and the sequence parser of Yosys' Verific SVA
// frontend, frontends/verific/verificsva.cc, which carries this notice:
//
//   Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
//
//   Permission to use, copy, modify, and/or distribute this software for any
//   purpose with or without fee is hereby granted, provided that the above
//   copyright notice and this permission notice appear in all copies.
//
//   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
//   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
//   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
//   ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
//   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
//   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Reworked for the Slang AST and the sv-elab netlist (see git history); the changes
// are distributed under the same ISC terms.

// This regular-sequence monitor follows the NFA representation of that frontend.
// Keeping the representation here, instead of expanding unbounded ranges, is what
// makes unbounded SVA synthesis finite and exact.
//
// The graph has two kinds of arcs:
//   * links are evaluated without consuming a sampled clock tick;
//   * edges transfer active states on the next sampled clock tick.
struct SvaNfaNode {
	std::vector<std::pair<int, ir::Net>> edges;
	std::vector<std::pair<int, ir::Net>> links;
};

class SvaSequenceNfa {
	EvalContext& eval;
	ir::Net trigger;
	ir::Net disable = ir::S0;
	// Is the trigger on the first tick of the attempts only? A value frozen when the
	// attempt was queued is valid on that tick alone (see check_frozen_offset()).
	bool attempt_tick;
	ir::Net throughout = ir::S1;
	// Where the throughout condition reads a frozen value, if it does
	std::optional<slang::SourceLocation> throughout_frozen;
	std::vector<std::pair<ir::Net, std::optional<slang::SourceLocation>>> throughout_stack;
	std::vector<SvaNfaNode> nodes;
	// The nodes frozen values get read on, by the controls of their arcs
	std::vector<std::pair<int, slang::SourceLocation>> frozen_reads;
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

	SvaSequenceNfa(EvalContext& eval_, ir::Net trigger_, ir::Net disable_, bool attempt_tick_):
		eval(eval_), trigger(trigger_), disable(disable_), attempt_tick(attempt_tick_),
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
		if (throughout_frozen.has_value())
			read_frozen(from, *throughout_frozen);
		nodes[from].edges.emplace_back(to, control);
	}

	void create_link(int from, int to, ir::Net control = ir::S1) {
		assert_invariant(!materialized);
		assert_invariant(0 <= from && from < int(nodes.size()));
		assert_invariant(0 <= to && to < int(nodes.size()));
		assert_invariant(to != start_node);
		control = eval.netlist.LogicAnd(control, throughout);
		if (throughout_frozen.has_value())
			read_frozen(from, *throughout_frozen);
		nodes[from].links.emplace_back(to, control);
	}

	// The control of an arc from `node` reads a frozen value
	void read_frozen(int node, slang::SourceLocation loc) {
		frozen_reads.emplace_back(node, loc);
	}

	void push_throughout(ir::Net condition, std::optional<slang::SourceLocation> frozen) {
		throughout_stack.emplace_back(throughout, throughout_frozen);
		throughout = eval.netlist.LogicAnd(throughout, condition);
		if (!throughout_frozen.has_value())
			throughout_frozen = frozen;
	}

	void pop_throughout() {
		assert_invariant(!throughout_stack.empty());
		throughout = throughout_stack.back().first;
		throughout_frozen = throughout_stack.back().second;
		throughout_stack.pop_back();
	}

	// A node is active past the first tick of an attempt if a tick leads into it, or
	// if the trigger is past it. A frozen value read on such a node has gone stale.
	void check_frozen_reads() const {
		std::vector<bool> late(nodes.size(), !attempt_tick);
		std::vector<int> pending;
		auto mark = [&](int node) {
			if (!late[node]) {
				late[node] = true;
				pending.push_back(node);
			}
		};
		for (const auto& node : nodes)
			for (const auto& edge : node.edges)
				mark(edge.first);
		while (!pending.empty()) {
			int node = pending.back();
			pending.pop_back();
			for (const auto& link : nodes[node].links)
				mark(link.first);
			for (const auto& edge : nodes[node].edges)
				mark(edge.first);
		}
		for (const auto& [node, loc] : frozen_reads)
			if (late[node])
				eval.netlist.add_diag(diag::SVAFrozenValueAfterFirstTick, loc);
	}

	ir::Net materialize_accept() {
		assert_invariant(!materialized);
		materialized = true;
		check_frozen_reads();

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
	// High where the letters are T, see AttemptContext. That is for a monitor of a single
	// attempt, the attempts other monitors merge differ in the tick their letters change.
	ir::Net top;
	bool valid = true;

	void unsupported(const ast::AssertionExpr& expr) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		valid = false;
	}

	// A boolean, `frozen` is where it reads a value frozen with the attempt, if it does
	ir::Net expression_bit(const ast::Expression& expr,
						   std::optional<slang::SourceLocation>& frozen) {
		ir::Net value = eval.netlist.ReduceBool(eval.sva(expr));
		frozen.reset();
		if (eval.sva_read_frozen)
			frozen = expr.sourceRange.start();
		return value;
	}

	// A boolean as of the letter, T satisfies any of them
	ir::Net letter(ir::Net boolean) {
		return eval.netlist.LogicOr(boolean, top);
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

	bool simple_condition(const ast::AssertionExpr& expr, ir::Net& condition,
						  std::optional<slang::SourceLocation>& frozen) {
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
		frozen.reset();
		condition = simple.isNullExpr ? ir::S0 : expression_bit(simple.expr, frozen);
		return true;
	}

	Fragment parse_nonconsecutive(int start, const ast::AssertionExpr& body,
								  const ast::SequenceRepetition& repetition,
								  const ast::AssertionExpr& owner) {
		ir::Net condition;
		std::optional<slang::SourceLocation> frozen;
		if (!simple_condition(body, condition, frozen)) {
			unsupported(owner);
			return {start, false};
		}
		// `!b` is a boolean of its own, T satisfies it as well as `b`
		ir::Net not_condition = letter(sva_not(eval, condition));
		condition = letter(condition);
		// Both get read while waiting
		auto wait_on = [&](int node) {
			if (frozen.has_value())
				fsm.read_frozen(node, *frozen);
		};

		// `b[->n]` is `(!b[*0:$] ##1 b)[*n]`
		Fragment result = parse_repeat(start, repetition.range, [&](int node) {
			int wait = fsm.create_node(node);
			wait_on(wait);
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
			wait_on(wait);
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
	SvaSequenceBuilder(EvalContext& eval_, SvaSequenceNfa& fsm_, ir::Net top_ = ir::S0):
		eval(eval_), fsm(fsm_), top(top_) {}

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
				std::optional<slang::SourceLocation> frozen;
				int node = fsm.create_node();
				fsm.create_link(start, node, letter(
					simple.isNullExpr ? ir::S0 :
					expression_bit(simple.expr, frozen)));
				if (frozen.has_value())
					fsm.read_frozen(start, *frozen);
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
						std::optional<slang::SourceLocation> frozen;
						if (!simple_condition(binary.left, condition, frozen)) {
							unsupported(expr);
							return {start, false};
						}
						fsm.push_throughout(letter(condition), frozen);
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
			{
				// A sequence used as a property, as is the operand of `nexttime` in an
				// eventuality. A weak one is strong where it's bounded, and an empty
				// match is no success either way.
				const auto& strong_weak = expr.as<ast::StrongWeakAssertionExpr>();
				if (strong_weak.strength == ast::StrongWeakAssertionExpr::Weak &&
					assertion_has_unbounded_sequence(strong_weak.expr)) {
					eval.netlist.add_diag(diag::SVAWeakSequenceUnsupported, expr_loc(expr));
					valid = false;
					return {start, false};
				}
				return {parse(start, strong_weak.expr).end, false};
			}

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

// An empty match has no end tick to be accepted on; `admits_empty` reports it. A disable
// drops the attempts in flight, which is what the dual letter does to them, while `top`
// gives T letters to the one attempt of a monitor that has no others. `attempt_tick`
// tells if the trigger is on the first tick of the attempts only.
static std::optional<ir::Net>
synthesize_unbounded_sequence(EvalContext& eval, const ast::AssertionExpr& sequence,
							  ir::Net trigger, ir::Net disable, ir::Net top, bool attempt_tick,
							  bool *admits_empty = nullptr) {
	if (!sva_clock_ready(eval, expr_loc(sequence)))
		return std::nullopt;

	SvaSequenceNfa fsm(eval, trigger, disable, attempt_tick);
	SvaSequenceBuilder builder(eval, fsm, top);
	auto match = builder.parse(fsm.create_node(fsm.start_node), sequence);
	if (!builder.good())
		return std::nullopt;
	if (admits_empty)
		*admits_empty = match.empty;
	fsm.create_link(match.end, fsm.accept_node);
	return fsm.materialize_accept();
}

// ---- End of the portion adapted from Yosys ----

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

	explicit SvaFormulas(EvalContext &eval_): eval(eval_) {
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

	// The value of `bit` sampled on tick `tick` of the attempt
	Node value(ir::Net bit, int64_t tick) {
		if (bit.is_const())
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

	// The number of gates making up `root`, counted up to `limit`
	size_t gates(Node root, size_t limit) const {
		std::vector<Node> stack = {root};
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

	// Logic for `root` on tick `tick` of the attempt, for each of many formulas on a tick
	// of its own, as for the paths of a sequence on the ticks they end on. A formula built
	// on an earlier tick gets delayed from there, the way materialize_prefix() does with no
	// budget, so formulas built going by tick share the logic of those before.
	ir::Net materialize_by_tick(Node root, int64_t tick, slang::SourceLocation loc) {
		size_t budget = logic_budget;
		logic_budget = SIZE_MAX;
		std::optional<ir::Net> logic = materialize_prefix(root, tick, loc);
		logic_budget = budget;
		return *logic;
	}

	// Logic for `root` on tick `tick` of the attempt, a prefix on a tick of its own, or
	// none if that takes more than the budget left (see limit_prefix_logic()). Prefixes
	// on many ticks are built one on the other, and materialize() would build all those
	// before again on each tick. A formula that's been built on an earlier tick gets
	// delayed from there instead.
	std::optional<ir::Net> materialize_prefix(Node root, int64_t tick, slang::SourceLocation loc) {
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

	std::vector<Op> ops;
	Yosys::dict<std::pair<RTLIL::SigBit, int64_t>, Node> values;
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
	// Do the paths start past the first tick of the attempt, or for other attempts as
	// well? A value frozen when the attempt was queued has gone stale by then.
	bool late = false;

	AttemptContext negated() const {
		return { abort, settled, !dual, late };
	}

	// A boolean on the first tick of the operand, tick `tick` of the attempt
	AssertionMatch atom(const AssertionMatch& boolean, int64_t tick) const {
		if (settled)
			return AssertionMatch(boolean.f, dual ? SvaFormulas::False : SvaFormulas::True, boolean.loc);
		if (late)
			check_frozen_offset(boolean, 1);

		// A constant can have the value of the letter to begin with
		if (abort == nullptr || (SvaFormulas::is_const(boolean.sig) && (boolean.sig == SvaFormulas::True) != dual))
			return boolean;

		auto now = abort->span(tick, 0, 0);
		return dual ? boolean && !now : boolean || now;
	}

	// A path of the 1'b1s of a delay, `length` ticks long from tick `tick` of the attempt.
	// Only the last one counts, as the next boolean in a path sees the disable over the
	// ticks before it as well.
	AssertionMatch true_path(SvaFormulas& f, int64_t tick, int64_t length, slang::SourceLocation loc) const {
		AssertionMatch path = AssertionMatch(f, SvaFormulas::True, loc).shift(length);
		if (!dual || (abort == nullptr && !settled))
			return path;
		if (settled)
			return path && AssertionMatch(f, SvaFormulas::False, loc);
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
		paths.push_back(ctx.atom(AssertionMatch(f, SvaFormulas::False, loc), tick));
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

// Did the disable of a cover come first, on one of the first `length` ticks of the attempt
// and on none after an accept inside of it? From the first of the two on, the letters
// are the dual of T for the disable, T for the accept. Of the two on the same tick the
// disable wins, being the outer one.
static SvaFormulas::Node cover_disabled_first(SvaFormulas& f, ir::Net disable, ir::Net accept,
											 int64_t length) {
	// Going by tick, with whether an accept came on one of the ticks before
	SvaFormulas::Node disabled = SvaFormulas::False, accepted = SvaFormulas::False;
	for (int64_t tick = 0; tick <= length; tick++) {
		disabled = f.disj(disabled, f.conj(f.value(disable, tick), f.neg(accepted)));
		accepted = f.disj(accepted, f.value(accept, tick));
	}
	return disabled;
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
					return { at.atom(simple.isNullExpr ? AssertionMatch(f, SvaFormulas::False, expr_loc(expr))
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
				bool followed_by = biop.op == ast::BinaryAssertionOperator::OverlappedFollowedBy ||
								   biop.op == ast::BinaryAssertionOperator::NonOverlappedFollowedBy;
				auto left_ctx = implication ? ctx.negated() : ctx;

				OperandPaths left_at(left_ctx, base, [&](const AttemptContext& at, int64_t tick) {
					return synthesize_assertion_expr(f, at, biop.left, tick);
				});
				// `R |-> P` is `not (R ##0 not P)`, and `R |=> P` the same with a `##1`.
				// `R #-# P` is `not (R |-> not P)`, so `R ##0 P` with P used as a property.
				OperandPaths right_at(ctx, base, [&](const AttemptContext& at, int64_t tick) {
					auto paths = synthesize_assertion_expr(f, at, biop.right, tick);
					if (implication)
						return not_vec(f, at, std::move(paths), expr_loc(expr), tick);
					if (followed_by)
						return property_paths(f, at, std::move(paths), expr_loc(expr), tick);
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
					f.eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, expr_loc(expr));
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
					f.eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, expr_loc(expr));
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
// attempt so far can show it doomed. Each check rules out traces the one on the last
// tick rules out on any way they go on, and those of an attempt are folded into one
// cell with it.
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
			SvaFormulas::Node value = f.prefix_value(p);
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
	for (auto [tick, value] : checks) {
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

enum class PropertyObligationKind {
	Safety,
	Liveness,
	AuxiliaryFairness
};

struct PropertyObligation {
	PropertyObligationKind kind;
	ir::Value a;
	ir::Value en;
	// Is an attempt in flight on the tick, going by the aborts of the ticks before it
	ir::Value active;
	// For a safety check on the last tick of the paths of an attempt, adds the ticks
	// before on which the attempt can show doomed, for an assumption to rule it out on
	// (see early_rejection())
	std::function<void(AssertionResult&)> early_rejection = nullptr;
	// For a liveness obligation that answers for the attempts of all the ticks at once,
	// whether any are held over from the ticks before (see lower_sva_obligations())
	ir::Net held = ir::S0;
};

struct PropertyResult {
	std::vector<PropertyObligation> obligations;
	bool valid = true;
};

// The aborts in effect on the attempts lower_property() starts on its trigger. An abort
// that came ahead of the trigger tick has kept the attempts from getting there, so these
// bear on an attempt from its first tick on. A trigger leaves the aborts of its own tick
// to the monitors it starts, which is how a reject_on gets to come ahead of them.
struct PropertyAborts {
	// From the tick it holds on, the letters are T: a disable iff or an accept_on
	ir::Net accept = ir::S0;
	// The disable iff of a cover, from which the letters are the dual of T instead (see
	// eval_assertion())
	ir::Net cover_disable = ir::S0;

	ir::Net any(EvalContext& eval) const {
		return eval.netlist.LogicOr(accept, cover_disable);
	}
};

// An operand of a monitor, which reads it past the first tick of an attempt if `late`.
// A value frozen when the attempt was queued is only valid on that first tick (see
// check_frozen_offset()).
static ir::Value monitor_operand(EvalContext& eval, const ast::Expression& expr, bool late) {
	ir::Value value = eval.sva(expr);
	if (late && eval.sva_read_frozen)
		eval.netlist.add_diag(diag::SVAFrozenValueAfterFirstTick, expr.sourceRange);
	return value;
}

// The condition of a disable iff or an abort, which the monitors read on all the ticks of
// an attempt. The condition of an abort is a sampled value like the operands (IEEE Std
// 1800-2017 section 16.12.14). That of a disable iff is read on current values in the
// standard; sampling it is a deliberate deviation (see eval_assertion()).
static ir::Net abort_condition(EvalContext& eval, const ast::Expression& condition) {
	return eval.netlist.ReduceBool(monitor_operand(eval, condition, true));
}

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

// A sequence with an unbounded match that's read as weak in the property, if it has one.
// A sequence used as a property of an assertion or an assumption is weak unless written
// strong() (IEEE Std 1800-2017 section 16.12.2). It holds then as long as a match is still
// to come, and the monitors only tell where one came.
static const ast::AssertionExpr* find_weak_unbounded_sequence(const ast::AssertionExpr& expr) {
	const ast::AssertionExpr& top = unwrap_property_instances(expr);
	if (!is_property(top))
		return assertion_has_unbounded_sequence(top) ? &expr : nullptr;

	auto either = [](const ast::AssertionExpr* left, const ast::AssertionExpr* right) {
		return left ? left : right;
	};
	switch (top.kind) {
	case ast::AssertionExprKind::StrongWeak:
		{
			const auto& strong_weak = top.as<ast::StrongWeakAssertionExpr>();
			bool weak = strong_weak.strength == ast::StrongWeakAssertionExpr::Weak;
			return weak && assertion_has_unbounded_sequence(strong_weak.expr) ? &top : nullptr;
		}
	case ast::AssertionExprKind::Unary:
		return find_weak_unbounded_sequence(top.as<ast::UnaryAssertionExpr>().expr);
	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = top.as<ast::BinaryAssertionExpr>();
			switch (binary.op) {
			case ast::BinaryAssertionOperator::OverlappedImplication:
			case ast::BinaryAssertionOperator::NonOverlappedImplication:
			case ast::BinaryAssertionOperator::OverlappedFollowedBy:
			case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
				// The left side is a sequence, not used as a property
				return find_weak_unbounded_sequence(binary.right);
			default:
				return either(find_weak_unbounded_sequence(binary.left),
							  find_weak_unbounded_sequence(binary.right));
			}
		}
	case ast::AssertionExprKind::Abort:
		return find_weak_unbounded_sequence(top.as<ast::AbortAssertionExpr>().expr);
	case ast::AssertionExprKind::Conditional:
		{
			const auto& conditional = top.as<ast::ConditionalAssertionExpr>();
			return either(find_weak_unbounded_sequence(conditional.ifExpr),
						  conditional.elseExpr
							  ? find_weak_unbounded_sequence(*conditional.elseExpr)
							  : nullptr);
		}
	case ast::AssertionExprKind::Case:
		{
			const auto& case_expr = top.as<ast::CaseAssertionExpr>();
			for (const auto& item : case_expr.items)
				if (auto weak = find_weak_unbounded_sequence(*item.body))
					return weak;
			return case_expr.defaultCase
				? find_weak_unbounded_sequence(*case_expr.defaultCase) : nullptr;
		}
	case ast::AssertionExprKind::Clocking:
		return find_weak_unbounded_sequence(top.as<ast::ClockingAssertionExpr>().expr);
	case ast::AssertionExprKind::DisableIff:
		return find_weak_unbounded_sequence(top.as<ast::DisableIffAssertionExpr>().expr);
	default:
		return nullptr;
	}
}

// The operand of an eventuality, which is read as strong by the monitors, with a strong()
// around it taken off. An error is raised if it has a weak sequence they can't read so.
static const ast::AssertionExpr* eventual_operand(EvalContext& eval,
												   const ast::AssertionExpr& expr) {
	if (auto weak = find_weak_unbounded_sequence(expr)) {
		eval.netlist.add_diag(diag::SVAWeakSequenceUnsupported, expr_loc(*weak));
		return nullptr;
	}
	// The sequence of a weak() is bounded, it then holds where it's strong
	const ast::AssertionExpr& top = unwrap_property_instances(expr);
	if (top.kind == ast::AssertionExprKind::StrongWeak)
		return &top.as<ast::StrongWeakAssertionExpr>().expr;
	return &expr;
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

// `trigger` delayed by `cycles`, short of the attempts a disable came upon on the ticks
// in between. That of the tick it is delayed to is up to the monitor it starts.
static ir::Net delayed_abortable_trigger(EvalContext& eval, ir::Net trigger, uint32_t cycles,
										 ir::Net disable, slang::SourceLocation loc) {
	ir::Net not_disable = sva_not(eval, disable);
	ir::Net active = trigger;
	for (uint32_t i = 0; i < cycles; i++) {
		active = eval.netlist.LogicAnd(active, not_disable);
		active = delay_sva_sample(eval, active, 1, ir::S0, loc, "sva_obligation").as_net();
	}
	return active;
}

// The paths one to a tick they end on, the earliest first. That's the order to build
// their logic in, see SvaFormulas::materialize_by_tick().
static std::vector<AssertionMatch> path_ends(std::vector<AssertionMatch> paths) {
	paths = compress_paths(std::move(paths));
	std::reverse(paths.begin(), paths.end());
	return paths;
}

static std::optional<std::vector<EventualCandidate>>
finite_eventual_candidates(EvalContext& eval, const ast::AssertionExpr& sequence,
						   uint32_t leading_delay = 0) {
	if (assertion_has_unbounded_sequence(sequence))
		return std::nullopt;

	// The disable is up to the monitor. The operand is used as a property, so an
	// empty match of it meets nothing. Its matches are those of any attempt.
	SvaFormulas f(eval);
	AttemptContext ctx;
	ctx.late = true;
	auto paths = property_paths(f, ctx, synthesize_assertion_expr(f, ctx, sequence, 0),
								expr_loc(sequence), 0);

	std::vector<EventualCandidate> candidates;
	for (const auto& path : path_ends(paths)) {
		ir::Net frame_ready = sva_not(
			eval,
			eval.netlist.ReduceBool(delay_sva_sample(
				eval, ir::S0, path.start,
				ir::S1, path.loc)));
		ir::Net match = f.materialize_by_tick(path.sig, path.start, path.loc);
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

	// The disable is up to the monitor. The matches are those of any attempt.
	SvaFormulas f(eval);
	AttemptContext ctx;
	ctx.late = true;
	auto paths = synthesize_assertion_expr(
		f, ctx, *concat.elements[0].sequence, 0);
	for (size_t i = 1; i < concat.elements.size(); i++) {
		const auto& element = concat.elements[i];
		if (!element.delay.max.has_value() ||
			assertion_has_unbounded_sequence(*element.sequence))
			return std::nullopt;
		OperandPaths element_paths(ctx, 0, [&](const AttemptContext& at, int64_t tick) {
			return synthesize_assertion_expr(f, at, *element.sequence, tick);
		}, concat_after(f, *element.sequence));
		paths = seq_vec(ctx, paths, element.delay.min, element.delay.max.value(),
						element_paths);
	}

	uint64_t leading_delay =
		uint64_t(additional_delay) + concat.elements[0].delay.min;
	std::vector<EventualCandidate> candidates;
	for (const auto& path : path_ends(paths)) {
		ir::Net frame_ready = sva_not(
			eval,
			eval.netlist.ReduceBool(delay_sva_sample(
				eval, ir::S0, path.start,
				ir::S1, path.loc)));
		ir::Net match = f.materialize_by_tick(path.sig, path.start, path.loc);
		match = eval.netlist.LogicAnd(match, frame_ready);
		// With nothing after the leading delay to match, `##n empty` is `##(n-1) `true`
		// and `##0 empty` has no match, so the match ends a tick ahead of the delay
		uint64_t duration = uint64_t(path.start) + leading_delay;
		if (path.empty)
			duration = additional_delay +
					   std::max<uint64_t>(concat.elements[0].delay.min, 1) - 1;
		candidates.push_back({match, duration});
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

	ir::Net held = old;
	for (ir::Net age : recent)
		held = eval.netlist.LogicOr(held, age);
	ir::Net in_flight = eval.netlist.LogicOr(held, trigger);

	trigger = eval.netlist.LogicAnd(trigger, sva_not(eval, disable));
	std::vector<ir::Net> ages;
	ages.reserve(maximum_duration);
	ages.push_back(trigger);
	ages.insert(ages.end(), recent.begin(), recent.end());

	// A match clears the ages from its duration up, so each age takes what clears the
	// one below it and the matches of its own duration
	std::vector<ir::Net> clears(maximum_duration,
									  ir::S0);
	for (const auto& candidate : candidates) {
		if (candidate.duration < maximum_duration)
			clears[candidate.duration] =
				eval.netlist.LogicOr(clears[candidate.duration], candidate.match);
	}
	for (uint64_t age = 1; age < maximum_duration; age++)
		clears[age] = eval.netlist.LogicOr(clears[age - 1], clears[age]);

	ir::Net pending = eval.netlist.LogicOr(held, trigger);
	ir::Net completion = eval.netlist.LogicAnd(old, any_match);
	std::vector<ir::Net> remaining;
	remaining.reserve(ages.size());
	for (size_t age = 0; age < ages.size(); age++) {
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
		 ir::S1, in_flight});
	result.obligations.back().held = held;
}

// The ticks to start the right side of `sequence |-> p`, or of `sequence |=> p` if
// `nonoverlapped`. `admits_empty` reports an empty match of the sequence, `one_tick`
// whether all its matches start the right side on one and the same tick of the attempt.
// `attempt_tick` tells if the trigger is on the first tick of the attempts only.
//
// An antecedent sees the dual letter, which no boolean is met by, so a disable keeps
// the attempts it comes upon from matching. Where the right side starts on the tick
// after the match, the disable of that tick is left to it, like that of any trigger.
static std::optional<ir::Net>
antecedent_match_event(EvalContext& eval, const ast::AssertionExpr& sequence,
					   ir::Net trigger, bool attempt_tick, bool nonoverlapped,
					   ir::Net disable, bool *admits_empty = nullptr,
					   bool *one_tick = nullptr) {
	ir::Net result = ir::S0;
	bool empty = false;
	int ticks = 0;
	if (assertion_has_unbounded_sequence(sequence)) {
		ticks = 2;
		auto match = synthesize_unbounded_sequence(
			eval, sequence, trigger, disable, ir::S0, attempt_tick, &empty);
		if (!match.has_value())
			return std::nullopt;
		result = match.value();
	} else {
		SvaFormulas f(eval);
		AttemptContext ctx;
		ctx.dual = true;
		ctx.late = !attempt_tick;
		std::optional<DisableSpans> spans;
		if (disable != ir::S0)
			ctx.abort = &spans.emplace(AssertionMatch(f, f.value(disable, 0), expr_loc(sequence)), disable);
		auto paths = synthesize_assertion_expr(f, ctx, sequence, 0);
		// One path per length of match
		for (const auto& path : path_ends(paths)) {
			if (path.empty) {
				empty = true;
				continue;
			}
			if (path.sig != SvaFormulas::False)
				ticks++;
			ir::Net path_match = f.materialize_by_tick(path.sig, path.start, path.loc);
			path_match = eval.netlist.LogicAnd(path_match,
				delay_sva_sample(eval, trigger, path.start, ir::S0, path.loc).as_net());
			result = eval.netlist.LogicOr(result, path_match);
		}
		if (empty && nonoverlapped)
			ticks++;
	}
	if (admits_empty)
		*admits_empty = empty;
	if (one_tick)
		*one_tick = ticks <= 1;
	// `seq |-> p` has no tick to start `p` on after an empty match, but
	// `seq |=> p` is `(seq ##1 `true) |-> p` where it matches on the first tick
	if (nonoverlapped) {
		result = delayed_abortable_trigger(
			eval, result, 1, disable, expr_loc(sequence));
		if (empty)
			result = eval.netlist.LogicOr(result, trigger);
	}
	return result;
}

static void add_liveness_obligation(PropertyResult& result, EvalContext& eval,
									ir::Net goal, ir::Net trigger,
									ir::Net disable, slang::SourceLocation loc) {
	// T letters meet any goal, so a disable discharges the attempts pending and the one
	// starting on its tick
	goal = eval.netlist.LogicOr(goal, disable);
	ir::Net active = add_pending_monitor(eval, trigger, goal, ir::S0, loc);
	trigger = eval.netlist.LogicAnd(trigger, sva_not(eval, disable));
	result.obligations.push_back(
		{PropertyObligationKind::Liveness, goal, trigger, active});
}

static void add_safety_obligation(PropertyResult& result, ir::Value a,
								  ir::Value en) {
	result.obligations.push_back(
		{PropertyObligationKind::Safety, a, en, ir::S0});
}

// A property that is a boolean, read past the first tick of an attempt if `late` (see
// monitor_operand())
static std::optional<ir::Net>
simple_property_signal(EvalContext& eval, const ast::AssertionExpr& expr, bool late,
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
			return simple_property_signal(eval, instance.body, late, negate);
		}
		ir::Net value = simple.isNullExpr
			? ir::S0
			: eval.netlist.ReduceBool(monitor_operand(eval, simple.expr, late));
		return negate ? sva_not(eval, value) : value;
	}
	if (current->kind == ast::AssertionExprKind::Unary) {
		const auto& unary = current->as<ast::UnaryAssertionExpr>();
		if (unary.op == ast::UnaryAssertionOperator::Not)
			return simple_property_signal(eval, unary.expr, late, !negate);
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

// High from the first tick of `event` on, until a `clear`. `earlier` gets the same as of
// the ticks before, if given.
static ir::Net add_sticky_observation(
		EvalContext& eval, ir::Net event, ir::Net clear,
		slang::SourceLocation loc, std::string_view name, ir::Net *earlier = nullptr) {
	ir::Net wire = add_sva_state(eval, name);
	ir::Net observed = eval.netlist.LogicOr(wire, event);
	ir::Net next =
		eval.netlist.LogicAnd(observed, sva_not(eval, clear));

	if (!sva_clock_ready(eval, loc)) {
		if (earlier)
			*earlier = ir::Sx;
		return ir::Sx;
	}
	add_sva_flop(eval, next, wire, name);
	if (earlier)
		*earlier = wire;
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

// Does the sequence take `and`, `intersect`, `within` or `first_match`? Those compose
// matches, which the NFA of a regular sequence has no way to.
static bool has_sequence_product(const ast::AssertionExpr& expr) {
	switch (expr.kind) {
	case ast::AssertionExprKind::Simple:
		{
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
				return false;
			const auto& instance = simple.expr.as<ast::AssertionInstanceExpression>();
			return !instance.isRecursiveProperty && has_sequence_product(instance.body);
		}
	case ast::AssertionExprKind::SequenceConcat:
		for (const auto& element : expr.as<ast::SequenceConcatExpr>().elements)
			if (has_sequence_product(*element.sequence))
				return true;
		return false;
	case ast::AssertionExprKind::SequenceWithMatch:
		return has_sequence_product(expr.as<ast::SequenceWithMatchExpr>().expr);
	case ast::AssertionExprKind::FirstMatch:
		return true;
	case ast::AssertionExprKind::Unary:
		return has_sequence_product(expr.as<ast::UnaryAssertionExpr>().expr);
	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = expr.as<ast::BinaryAssertionExpr>();
			switch (binary.op) {
			case ast::BinaryAssertionOperator::And:
			case ast::BinaryAssertionOperator::Intersect:
			case ast::BinaryAssertionOperator::Within:
				return true;
			default:
				return has_sequence_product(binary.left) || has_sequence_product(binary.right);
			}
		}
	case ast::AssertionExprKind::StrongWeak:
		return has_sequence_product(expr.as<ast::StrongWeakAssertionExpr>().expr);
	default:
		return false;
	}
}

// High on the last tick of a non-empty match of the sequence the attempts on the trigger
// started. An empty match has no such tick, `admits_empty` reports it. The letters of the
// attempt are T where `top` is high, see AttemptContext. `attempt_tick` tells if the
// trigger is on the first tick of the attempts only.
//
// The NFA of a regular sequence takes the matches of any number of attempts at once.
// `and`, `intersect` and `within` compose a match of each operand from one and the same
// start, and `first_match` picks from the matches of one start, so their operands are
// monitored apart from a trigger that is high on one tick at most, as `single_start`
// says. With `earliest` the caller only takes whether there was a match by a tick, all
// it goes by is the first tick the result is high on.
static std::optional<ir::Net> synthesize_selected_sequence(
		EvalContext& eval, const ast::AssertionExpr& sequence, ir::Net trigger, ir::Net top,
		bool attempt_tick, bool single_start, bool earliest, bool& admits_empty) {
	admits_empty = false;
	if (!has_sequence_product(sequence))
		return synthesize_unbounded_sequence(
			eval, sequence, trigger, ir::S0, top, attempt_tick, &admits_empty);

	slang::SourceLocation loc = expr_loc(sequence);
	auto operand = [&](const ast::AssertionExpr& expr, bool operand_earliest, bool& empty) {
		return synthesize_selected_sequence(eval, expr, trigger, top, attempt_tick,
											single_start, operand_earliest, empty);
	};
	// The operands of the attempt that started on the trigger, apart from other attempts
	auto one_start = [&]() {
		if (single_start)
			return true;
		eval.netlist.add_diag(diag::SVASequenceProductUnsupported, loc);
		return false;
	};
	// High from the start of the attempt on
	auto started = [&]() {
		return add_sticky_observation(eval, trigger, ir::S0, loc, "sva_select_started");
	};

	switch (sequence.kind) {
	case ast::AssertionExprKind::Simple:
		{
			const auto& simple = sequence.as<ast::SimpleAssertionExpr>();
			if (simple.repetition.has_value() ||
				simple.expr.kind != ast::ExpressionKind::AssertionInstance)
				break;
			const auto& instance = simple.expr.as<ast::AssertionInstanceExpression>();
			if (instance.isRecursiveProperty)
				break;
			return operand(instance.body, earliest, admits_empty);
		}

	case ast::AssertionExprKind::StrongWeak:
		{
			// A sequence used as a property, as an operand of a property `or` in an
			// eventuality. A weak one is strong where it's bounded.
			const auto& strong_weak = sequence.as<ast::StrongWeakAssertionExpr>();
			if (strong_weak.strength == ast::StrongWeakAssertionExpr::Weak &&
				assertion_has_unbounded_sequence(strong_weak.expr)) {
				eval.netlist.add_diag(diag::SVAWeakSequenceUnsupported, loc);
				return std::nullopt;
			}
			auto match = operand(strong_weak.expr, earliest, admits_empty);
			admits_empty = false;
			return match;
		}

	case ast::AssertionExprKind::FirstMatch:
		{
			const auto& first = sequence.as<ast::FirstMatchAssertionExpr>();
			if (!first.matchItems.empty()) {
				eval.netlist.add_diag(diag::SVAMatchItemsUnsupported, loc);
				return std::nullopt;
			}
			// The first match is the first one to end, which is all that is taken of
			// the matches with `earliest`
			if (!earliest && !one_start())
				return std::nullopt;
			auto match = operand(first.seq, earliest, admits_empty);
			if (!match.has_value())
				return std::nullopt;
			// An empty match comes ahead of any other, so it's the only first match
			if (admits_empty)
				return ir::S0;
			if (earliest)
				return match;
			ir::Net earlier;
			add_sticky_observation(eval, *match, ir::S0, loc, "sva_first_match_seen", &earlier);
			return eval.netlist.LogicAnd(*match, sva_not(eval, earlier));
		}

	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = sequence.as<ast::BinaryAssertionExpr>();
			bool left_empty = false;
			bool right_empty = false;
			switch (binary.op) {
			case ast::BinaryAssertionOperator::Or:
				{
					auto left = operand(binary.left, earliest, left_empty);
					auto right = operand(binary.right, earliest, right_empty);
					if (!left.has_value() || !right.has_value())
						return std::nullopt;
					admits_empty = left_empty || right_empty;
					return eval.netlist.LogicOr(left.value(), right.value());
				}
			case ast::BinaryAssertionOperator::And:
				{
					if (!one_start())
						return std::nullopt;
					auto left = operand(binary.left, earliest, left_empty);
					auto right = operand(binary.right, earliest, right_empty);
					if (!left.has_value() || !right.has_value())
						return std::nullopt;
					admits_empty = left_empty && right_empty;
					// A match ends with that of one operand, the other one having had one
					// by then. An empty match of an operand puts no constraint on the
					// other one (IEEE Std 1800-2017 section F.3.4.2.4).
					ir::Net from_start = left_empty || right_empty ? started() : ir::S0;
					ir::Net left_seen = left_empty ? from_start : add_sticky_observation(
						eval, left.value(), ir::S0, expr_loc(binary.left), "sva_and_left");
					ir::Net right_seen = right_empty ? from_start : add_sticky_observation(
						eval, right.value(), ir::S0, expr_loc(binary.right), "sva_and_right");
					return eval.netlist.LogicOr(
						eval.netlist.LogicAnd(left.value(), right_seen),
						eval.netlist.LogicAnd(right.value(), left_seen));
				}
			case ast::BinaryAssertionOperator::Intersect:
				{
					if (!one_start())
						return std::nullopt;
					auto left = operand(binary.left, false, left_empty);
					auto right = operand(binary.right, false, right_empty);
					if (!left.has_value() || !right.has_value())
						return std::nullopt;
					admits_empty = left_empty && right_empty;
					return eval.netlist.LogicAnd(left.value(), right.value());
				}
			case ast::BinaryAssertionOperator::Within:
				{
					if (!one_start())
						return std::nullopt;
					// The inner match can start on any tick of the outer one, it takes no
					// more than one of them to end by the end of the outer one
					ir::Net inner_start = started();
					auto inner = synthesize_selected_sequence(
						eval, binary.left, inner_start, top, false, false, true, left_empty);
					auto outer = operand(binary.right, false, right_empty);
					if (!inner.has_value() || !outer.has_value())
						return std::nullopt;
					admits_empty = left_empty && right_empty;
					// An empty inner match lies within any outer one (F.3.4.2.4)
					ir::Net inner_seen = left_empty ? inner_start : add_sticky_observation(
						eval, inner.value(), ir::S0, expr_loc(binary.left), "sva_within_seen");
					return eval.netlist.LogicAnd(inner_seen, outer.value());
				}
			case ast::BinaryAssertionOperator::Throughout:
				{
					// Read on each tick of the attempt
					auto condition =
						simple_property_signal(eval, binary.left, true);
					if (!condition.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported,
											  expr_loc(binary.left));
						return std::nullopt;
					}
					// Without a product in it, the NFA takes it all from any start
					if (!one_start())
						return std::nullopt;
					auto body = operand(binary.right, earliest, admits_empty);
					if (!body.has_value())
						return std::nullopt;
					ir::Net active = started();
					ir::Net invalid = add_sticky_observation(
						eval,
						eval.netlist.LogicAnd(active, sva_not(eval,
							eval.netlist.LogicOr(condition.value(), top))),
						ir::S0, expr_loc(binary.right),
						"sva_throughout_invalid");
					return eval.netlist.LogicAnd(
						body.value(),
						eval.netlist.LogicAnd(active, sva_not(eval, invalid)));
				}
			default:
				break;
			}
			break;
		}

	default:
		break;
	}
	// A product nested in a concatenation or a repetition, which the NFA rejects
	return synthesize_unbounded_sequence(
		eval, sequence, trigger, ir::S0, top, attempt_tick, &admits_empty);
}

// `attempt_tick` tells if the trigger is on the first tick of the attempts only, where
// a value frozen when the attempt was queued is valid
static PropertyResult lower_property(EvalContext& eval, const ast::AssertionExpr& expr,
									 ir::Net trigger, bool attempt_tick,
									 const PropertyAborts& aborts);

// Can synthesize_assertion_expr() lower all of `expr` by its paths, with nothing in it that
// only lower_property() takes apart? Such a property gets lowered by eval_assertion(), which
// keeps to Annex F for a disable and queues attempts as of the procedural condition around
// the assertion. Those it can't lower either raise their errors on either route.
static bool lowered_by_paths(const ast::AssertionExpr& expr) {
	if (assertion_has_unbounded_sequence(expr))
		return false;

	switch (expr.kind) {
	case ast::AssertionExprKind::Simple:
		{
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
				return true;
			const auto& instance = simple.expr.as<ast::AssertionInstanceExpression>();
			return instance.isRecursiveProperty || lowered_by_paths(instance.body);
		}
	case ast::AssertionExprKind::SequenceConcat:
		for (const auto& element : expr.as<ast::SequenceConcatExpr>().elements)
			if (!lowered_by_paths(*element.sequence))
				return false;
		return true;
	case ast::AssertionExprKind::SequenceWithMatch:
		return lowered_by_paths(expr.as<ast::SequenceWithMatchExpr>().expr);
	case ast::AssertionExprKind::FirstMatch:
		return lowered_by_paths(expr.as<ast::FirstMatchAssertionExpr>().seq);
	case ast::AssertionExprKind::Unary:
		{
			const auto& unary = expr.as<ast::UnaryAssertionExpr>();
			return unary.op != ast::UnaryAssertionOperator::Always &&
				   unary.op != ast::UnaryAssertionOperator::SAlways &&
				   lowered_by_paths(unary.expr);
		}
	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = expr.as<ast::BinaryAssertionExpr>();
			switch (binary.op) {
			case ast::BinaryAssertionOperator::Implies:
			case ast::BinaryAssertionOperator::Iff:
			case ast::BinaryAssertionOperator::Until:
			case ast::BinaryAssertionOperator::SUntil:
			case ast::BinaryAssertionOperator::UntilWith:
			case ast::BinaryAssertionOperator::SUntilWith:
				return false;
			default:
				return lowered_by_paths(binary.left) && lowered_by_paths(binary.right);
			}
		}
	case ast::AssertionExprKind::Clocking:
		return lowered_by_paths(expr.as<ast::ClockingAssertionExpr>().expr);
	case ast::AssertionExprKind::DisableIff:
		return lowered_by_paths(expr.as<ast::DisableIffAssertionExpr>().expr);
	case ast::AssertionExprKind::StrongWeak:
		return lowered_by_paths(expr.as<ast::StrongWeakAssertionExpr>().expr);
	case ast::AssertionExprKind::Abort:
	case ast::AssertionExprKind::Conditional:
	case ast::AssertionExprKind::Case:
		return false;
	case ast::AssertionExprKind::Invalid:
		return true;
	}
	assert_invariant(false && "unreachable");
}

// The paths of `expr` get the letters of the aborts the way eval_assertion() does, as of
// the trigger tick, so a failure settled ahead of a disable stands. `admits_empty` tells
// if a sequence has an empty match, which meets no property. Like eval_assertion(), the
// check is on the last tick of an attempt, with the earlier ticks it can show doomed on
// left for an assumption to add.
static PropertyResult lower_finite_property_from_trigger(
		EvalContext& eval, const ast::AssertionExpr& expr,
		ir::Net trigger, bool attempt_tick, const PropertyAborts& aborts,
		bool *admits_empty = nullptr) {
	PropertyResult result;
	slang::SourceLocation loc = expr_loc(expr);
	// Kept for the early rejection of an assumption, which goes by the formulas
	auto f = std::make_shared<SvaFormulas>(eval);
	ir::Net abort = aborts.any(eval);
	DisableSpans spans(AssertionMatch(*f, f->value(abort, 0), loc), abort);
	if (admits_empty)
		*admits_empty = false;
	auto synthesize = [&](bool dual) {
		AttemptContext ctx;
		if (abort != ir::S0)
			ctx.abort = &spans;
		ctx.dual = dual;
		ctx.late = !attempt_tick;
		auto paths = synthesize_assertion_expr(*f, ctx, expr, 0);
		for (const auto& path : paths)
			if (admits_empty && path.empty)
				*admits_empty = true;
		return collapse_or(property_paths(*f, ctx, std::move(paths), loc, 0));
	};
	bool cover = aborts.cover_disable != ir::S0;
	AssertionMatch match = synthesize(cover && aborts.accept == ir::S0);
	if (cover && aborts.accept != ir::S0) {
		// The letters depend on which of the two came first
		AssertionMatch dual = synthesize(true);
		assert_invariant(dual.start == match.start);
		match.sig = f->mux(cover_disabled_first(*f, aborts.cover_disable, aborts.accept, match.start),
						   match.sig, dual.sig);
	}
	// Delayed with an initial low, the trigger keeps the check off until the paths
	// have all their samples
	ir::Net enable = delay_sva_sample(
		eval, trigger, match.start, ir::S0, match.loc).as_net();
	add_safety_obligation(result, f->materialize(match.sig, match.start, match.loc), enable);
	result.obligations.back().early_rejection = [f, match, trigger](AssertionResult& checks) {
		early_rejection(*f, match, trigger, checks);
	};
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

// Is the sequence a single boolean, with no ticks past the first?
static bool is_boolean_sequence(const ast::AssertionExpr& expr) {
	if (expr.kind != ast::AssertionExprKind::Simple)
		return false;
	const auto& simple = expr.as<ast::SimpleAssertionExpr>();
	if (simple.repetition.has_value())
		return false;
	if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
		return true;
	const auto& instance = simple.expr.as<ast::AssertionInstanceExpression>();
	return !instance.isRecursiveProperty && is_boolean_sequence(instance.body);
}

// Is an attempt of the property in flight past its first tick only while one of the
// obligations lower_property() makes of it is active? An antecedent spanning ticks is
// in flight ahead of them, and dual letters from a reject_on would make it match.
static bool in_flight_by_obligations(const ast::AssertionExpr& expr) {
	switch (expr.kind) {
	case ast::AssertionExprKind::Simple:
		{
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (simple.repetition.has_value() ||
				simple.expr.kind != ast::ExpressionKind::AssertionInstance)
				return false;
			const auto& instance = simple.expr.as<ast::AssertionInstanceExpression>();
			return !instance.isRecursiveProperty && in_flight_by_obligations(instance.body);
		}
	case ast::AssertionExprKind::Unary:
		{
			// An attempt of `always P` is in flight through its interval, past the
			// first tick of the attempts of P it starts
			auto op = expr.as<ast::UnaryAssertionExpr>().op;
			return op != ast::UnaryAssertionOperator::Always &&
				   op != ast::UnaryAssertionOperator::SAlways;
		}
	case ast::AssertionExprKind::StrongWeak:
		// Lowered to obligations that start on the trigger
		return true;
	case ast::AssertionExprKind::Abort:
		return in_flight_by_obligations(expr.as<ast::AbortAssertionExpr>().expr);
	case ast::AssertionExprKind::Conditional:
		{
			const auto& conditional = expr.as<ast::ConditionalAssertionExpr>();
			return in_flight_by_obligations(conditional.ifExpr) &&
				   (!conditional.elseExpr || in_flight_by_obligations(*conditional.elseExpr));
		}
	case ast::AssertionExprKind::Case:
		{
			const auto& case_expr = expr.as<ast::CaseAssertionExpr>();
			for (const auto& item : case_expr.items)
				if (!in_flight_by_obligations(*item.body))
					return false;
			return !case_expr.defaultCase || in_flight_by_obligations(*case_expr.defaultCase);
		}
	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = expr.as<ast::BinaryAssertionExpr>();
			switch (binary.op) {
			case ast::BinaryAssertionOperator::And:
			case ast::BinaryAssertionOperator::Or:
				return in_flight_by_obligations(binary.left) &&
					   in_flight_by_obligations(binary.right);
			case ast::BinaryAssertionOperator::Implies:
				return in_flight_by_obligations(binary.right);
			case ast::BinaryAssertionOperator::OverlappedImplication:
			case ast::BinaryAssertionOperator::NonOverlappedImplication:
				return is_boolean_sequence(binary.left) &&
					   in_flight_by_obligations(binary.right);
			case ast::BinaryAssertionOperator::OverlappedFollowedBy:
			case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
				// The left side has a monitor of its own from the trigger on, which
				// dual letters keep from matching
				return in_flight_by_obligations(binary.right);
			default:
				return false;
			}
		}
	default:
		return false;
	}
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
		auto violation = simple_property_signal(eval, nested.expr, true, true);
		if (!violation.has_value()) {
			eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return std::nullopt;
		}
		uint32_t minimum = nested.range.has_value() ? nested.range->min : 0;
		return std::vector<EventualCandidate>{{violation.value(), minimum}};
	}

	uint32_t minimum = unary.range.has_value() ? unary.range->min : 0;
	const ast::AssertionExpr* operand = eventual_operand(eval, unary.expr);
	if (operand == nullptr)
		return std::nullopt;
	if (!assertion_has_unbounded_sequence(*operand))
		return finite_eventual_candidates(eval, *operand, minimum);
	if (!is_suffix_eventual_sequence(*operand)) {
		// Every attempt would need a selection of its own
		eval.netlist.add_diag(diag::SVALivenessAcceptanceUnsupported, expr_loc(expr));
		return std::nullopt;
	}
	return suffix_eventual_candidates(eval, *operand, minimum);
}

static PropertyResult lower_eventual(EvalContext& eval,
									 const ast::UnaryAssertionExpr& eventual,
									 ir::Net trigger, bool attempt_tick,
									 const PropertyAborts& aborts) {
	PropertyResult result;
	if (eventual.range.has_value() && eventual.range->max.has_value()) {
		return lower_finite_property_from_trigger(
			eval, eventual, trigger, attempt_tick, aborts);
	}

	ir::Net disable = aborts.any(eval);
	uint32_t minimum = eventual.range.has_value() ? eventual.range->min : 0;
	const ast::AssertionExpr* operand = eventual_operand(eval, eventual.expr);
	if (operand == nullptr)
		return {{}, false};
	if (assertion_has_unbounded_sequence(*operand) &&
		!is_suffix_eventual_sequence(*operand)) {
		ir::Net selected = add_selected_trigger(
			result, eval, trigger, disable, expr_loc(eventual));
		if (!result.valid)
			return result;
		ir::Net start = delayed_abortable_trigger(
			eval, selected, minimum, disable, expr_loc(eventual));
		ir::Net future_start = add_sticky_observation(
			eval, start, ir::S0, expr_loc(eventual),
			"sva_eventual_start");
		// The operand is used as a property, so an empty match of it meets nothing.
		// T letters would start a match of it on any tick, so the goal is left to
		// the real ones and a disable discharges the attempt. A match can start on any
		// tick from the minimum delay on.
		bool admits_empty;
		auto goal = synthesize_selected_sequence(
			eval, *operand, future_start, ir::S0, false, false, true, admits_empty);
		if (!goal.has_value())
			return {{}, false};
		// The attempt is in flight from the tick it was selected on, all through the
		// minimum delay
		add_liveness_obligation(
			result, eval, goal.value(), selected, disable, expr_loc(eventual));
		return result;
	}
	std::optional<std::vector<EventualCandidate>> candidates;
	if (assertion_has_unbounded_sequence(*operand))
		candidates = suffix_eventual_candidates(eval, *operand, minimum);
	else
		candidates = finite_eventual_candidates(eval, *operand, minimum);
	if (!candidates.has_value()) {
		result.valid = false;
		return result;
	}
	add_response_liveness_obligation(
		result, eval, *candidates, trigger, disable, expr_loc(eventual));
	return result;
}

// An attempt of `always P` starts one of P on each tick of its interval, those of
// overlapping attempts on the same tick are one and the same
static PropertyResult lower_always(EvalContext& eval,
								   const ast::UnaryAssertionExpr& always_expr,
								   ir::Net trigger, bool attempt_tick,
								   const PropertyAborts& aborts) {
	PropertyResult result;
	ir::Net disable = aborts.any(eval);
	slang::SourceLocation loc = expr_loc(always_expr);
	// Only `always [0:0]` reads its operand on the trigger tick alone
	bool late = !attempt_tick || !always_expr.range.has_value() ||
				always_expr.range->max != uint32_t(0);
	auto condition = simple_property_signal(eval, always_expr.expr, late);

	uint32_t minimum = always_expr.range.has_value()
		? always_expr.range->min : 0;
	ir::Net start = delayed_abortable_trigger(eval, trigger, minimum, disable, loc);

	// The ticks of the interval of some attempt, short of those a disable came upon
	// ahead of them
	ir::Net window;
	if (always_expr.range.has_value() &&
		always_expr.range->max.has_value()) {
		uint32_t maximum = always_expr.range->max.value();
		ir::Net enable = start;
		window = start;
		for (uint32_t offset = minimum; offset < maximum; offset++) {
			enable = delayed_abortable_trigger(eval, enable, 1, disable, loc);
			window = eval.netlist.LogicOr(window, enable);
		}
	} else {
		window = add_pending_monitor(eval, start, ir::S0, disable, loc);
	}

	if (condition.has_value()) {
		add_safety_obligation(result, condition.value(),
			eval.netlist.LogicAnd(window, sva_not(eval, disable)));
		return result;
	}
	return lower_property(eval, always_expr.expr, window, !late, aborts);
}

static PropertyResult lower_until(EvalContext& eval,
								  const ast::BinaryAssertionExpr& until_expr,
								  ir::Net trigger, ir::Net disable) {
	PropertyResult result;
	auto left = simple_property_signal(eval, until_expr.left, true);
	auto right = simple_property_signal(eval, until_expr.right, true);
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
									 ir::Net trigger, bool attempt_tick,
									 const PropertyAborts& aborts) {
	// lower_sva_obligations() takes the one a property is allowed, at the top of it.
	// slang only rejects the others in direct instances of a property.
	if (expr.kind == ast::AssertionExprKind::DisableIff) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		return {{}, false};
	}
	ir::Net disable = aborts.any(eval);

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
			return lower_property(eval, instance.body, trigger, attempt_tick, aborts);
		}
	}

	if (expr.kind == ast::AssertionExprKind::Unary) {
		const auto& unary = expr.as<ast::UnaryAssertionExpr>();
		if (unary.op == ast::UnaryAssertionOperator::Eventually ||
			unary.op == ast::UnaryAssertionOperator::SEventually)
			return lower_eventual(eval, unary, trigger, attempt_tick, aborts);
		if (unary.op == ast::UnaryAssertionOperator::Always ||
			unary.op == ast::UnaryAssertionOperator::SAlways)
			return lower_always(eval, unary, trigger, attempt_tick, aborts);
		if (unary.op == ast::UnaryAssertionOperator::Not) {
			const auto *nested = unary.expr.as_if<ast::UnaryAssertionExpr>();
			if (nested &&
				(nested->op == ast::UnaryAssertionOperator::Eventually ||
				 nested->op == ast::UnaryAssertionOperator::SEventually) &&
				(!nested->range.has_value() ||
				 !nested->range->max.has_value())) {
				auto condition = simple_property_signal(eval, nested->expr, true, true);
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
				// Only `not always [0:0]` reads its operand on the trigger tick alone
				bool late = !attempt_tick || !nested->range.has_value() ||
							nested->range->max != uint32_t(0);
				auto violation =
					simple_property_signal(eval, nested->expr, late, true);
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
					add_safety_obligation(result, seen,
						eval.netlist.LogicAnd(enable, sva_not(eval, disable)));
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
				eval.netlist.add_diag(diag::SVAWeakSequenceUnsupported, expr_loc(expr));
				return {{}, false};
			}
			return lower_finite_property_from_trigger(
				eval, strong_weak.expr, trigger, attempt_tick, aborts);
		}

		if (!assertion_has_unbounded_sequence(strong_weak.expr)) {
			return lower_finite_property_from_trigger(
				eval, strong_weak.expr, trigger, attempt_tick, aborts);
		}
		if (strong_weak.expr.kind == ast::AssertionExprKind::Binary) {
			const auto& binary =
				strong_weak.expr.as<ast::BinaryAssertionExpr>();
			if (binary.op == ast::BinaryAssertionOperator::And &&
				(!assertion_has_unbounded_sequence(binary.left) ||
				 is_suffix_eventual_sequence(binary.left)) &&
				(!assertion_has_unbounded_sequence(binary.right) ||
				 is_suffix_eventual_sequence(binary.right))) {
				// A match of each operand from the attempt makes one of the `and`. An
				// empty match of the finite one puts no constraint on the other one
				// (IEEE Std 1800-2017 section F.3.4.2.4), which never has one, so
				// the attempt then needs a match of the other one only.
				PropertyResult left;
				PropertyResult right;
				bool empty = false;
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
						eval, binary.left, trigger, attempt_tick, aborts, &empty);
					if (empty)
						left.obligations.clear();
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
						eval, binary.right, trigger, attempt_tick, aborts, &empty);
					if (empty)
						right.obligations.clear();
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
			// A disable leaves the selected attempt to T letters, which complete a
			// match if the sequence has one left by then, else it fails. A monitor of
			// the one attempt can tell its letters apart.
			ir::Net started = add_sticky_observation(
				eval, selected, ir::S0, expr_loc(expr), "sva_select_started");
			ir::Net top = add_sticky_observation(
				eval, eval.netlist.LogicAnd(started, disable), ir::S0, expr_loc(expr),
				"sva_select_top");
			// An empty match is no success of the sequence used as a property
			bool admits_empty;
			auto goal = synthesize_selected_sequence(
				eval, strong_weak.expr, selected, top, attempt_tick, true, true, admits_empty);
			if (!goal.has_value())
				return {{}, false};
			add_liveness_obligation(
				result, eval, goal.value(), selected, ir::S0, expr_loc(expr));
			// The attempt is settled on the tick of the disable, though the match
			// comes later
			auto& obligation = result.obligations.back();
			obligation.active = eval.netlist.LogicAnd(
				eval.netlist.ReduceBool(obligation.active),
				sva_not(eval, delay_sva_sample(eval, top, 1, ir::S0, expr_loc(expr)).as_net()));
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
		// The condition of an asynchronous abort is a sampled value as well (IEEE Std
		// 1800-2017 section 16.12.14). We read it on the ticks of the clock only, which
		// is exact for a condition that changes with the clock, while a pulse between
		// two ticks goes unseen.
		ir::Net condition = abort_condition(eval, abort.condition);
		if (abort.action == ast::AbortAssertionExpr::Accept) {
			PropertyAborts inner = aborts;
			inner.accept = eval.netlist.LogicOr(aborts.accept, condition);
			return lower_property(eval, abort.expr, trigger, attempt_tick, inner);
		}

		// From the tick of a reject on, the letters are the dual of T, unless an abort
		// from outside comes on the same tick, which takes priority. A liveness goal
		// isn't met by such letters, so the attempts in flight fail. Those already
		// settled stand, as do the ones a disable or accept_on came upon before.
		PropertyResult result =
			lower_property(eval, abort.expr, trigger, attempt_tick, aborts);
		if (!result.valid)
			return result;
		bool has_liveness = false;
		// An attempt is in flight on its first tick whatever the property
		ir::Net in_flight = trigger;
		for (const auto& obligation : result.obligations) {
			if (obligation.kind == PropertyObligationKind::Safety) {
				eval.netlist.add_diag(diag::SVARejectOnUnsupported,
									  expr_loc(expr));
				return {{}, false};
			}
			if (obligation.kind != PropertyObligationKind::Liveness)
				continue;
			has_liveness = true;
			in_flight = eval.netlist.LogicOr(in_flight,
				eval.netlist.ReduceBool(obligation.active));
		}
		if (!has_liveness || !in_flight_by_obligations(abort.expr)) {
			eval.netlist.add_diag(diag::SVARejectOnUnsupported, expr_loc(expr));
			return {{}, false};
		}
		add_safety_obligation(
			result, sva_not(eval, condition),
			eval.netlist.LogicAnd(in_flight, sva_not(eval, disable)));
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Conditional) {
		const auto& conditional =
			expr.as<ast::ConditionalAssertionExpr>();
		ir::Net condition = eval.netlist.ReduceBool(
			monitor_operand(eval, conditional.condition, !attempt_tick));
		PropertyResult result = lower_property(
			eval, conditional.ifExpr,
			eval.netlist.LogicAnd(trigger, condition), attempt_tick, aborts);
		if (conditional.elseExpr) {
			PropertyResult else_result = lower_property(
				eval, *conditional.elseExpr,
				eval.netlist.LogicAnd(trigger, sva_not(eval, condition)), attempt_tick,
				aborts);
			result.valid = result.valid && else_result.valid;
			result.obligations.insert(result.obligations.end(),
									  else_result.obligations.begin(),
									  else_result.obligations.end());
		}
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Case) {
		const auto& case_expr = expr.as<ast::CaseAssertionExpr>();
		ir::Value control = monitor_operand(eval, case_expr.expr, !attempt_tick);
		ir::Net matched = ir::S0;
		PropertyResult result;
		for (const auto& item : case_expr.items) {
			ir::Net item_hit = ir::S0;
			for (const auto *item_expr : item.expressions)
				item_hit = eval.netlist.LogicOr(
					item_hit,
					case_equality(eval, control,
								  monitor_operand(eval, *item_expr, !attempt_tick)));
			item_hit = eval.netlist.LogicAnd(item_hit, sva_not(eval, matched));
			PropertyResult branch = lower_property(
				eval, *item.body,
				eval.netlist.LogicAnd(trigger, item_hit), attempt_tick, aborts);
			result.valid = result.valid && branch.valid;
			result.obligations.insert(result.obligations.end(),
									 branch.obligations.begin(),
									 branch.obligations.end());
			matched = eval.netlist.LogicOr(matched, item_hit);
		}
		if (case_expr.defaultCase) {
			PropertyResult branch = lower_property(
				eval, *case_expr.defaultCase,
				eval.netlist.LogicAnd(trigger, sva_not(eval, matched)), attempt_tick,
				aborts);
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
					simple_property_signal(eval, binary.left, !attempt_tick);
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
					eval, binary.right, consequent_trigger, attempt_tick, aborts);
			}

		case ast::BinaryAssertionOperator::Iff:
			{
				auto left = simple_property_signal(eval, binary.left, !attempt_tick);
				auto right = simple_property_signal(eval, binary.right, !attempt_tick);
				if (!left.has_value() || !right.has_value()) {
					eval.netlist.add_diag(
						(property_requires_temporal_obligations(binary.left) ||
						 property_requires_temporal_obligations(binary.right))
							? diag::SVALivenessAcceptanceUnsupported
							: diag::AssertionUnsupported,
						expr_loc(expr));
					return {{}, false};
				}
				// T letters meet it, a disable of a cover drops the attempt
				PropertyResult result;
				add_safety_obligation(
					result, eval.netlist.LogicOr(eval.netlist.Eq(left.value(), right.value()),
												 aborts.accept),
					eval.netlist.LogicAnd(trigger, sva_not(eval, aborts.cover_disable)));
				return result;
			}

		case ast::BinaryAssertionOperator::OverlappedImplication:
		case ast::BinaryAssertionOperator::NonOverlappedImplication:
			{
				if (lowered_by_paths(binary))
					break;
				bool nonoverlapped =
					binary.op == ast::BinaryAssertionOperator::NonOverlappedImplication;
				auto antecedent = antecedent_match_event(
					eval, binary.left, trigger, attempt_tick, nonoverlapped, disable);
				if (!antecedent.has_value())
					return {{}, false};
				ir::Net consequent_trigger = antecedent.value();
				// A match on the trigger tick starts the consequent on the first tick
				bool consequent_attempt_tick =
					attempt_tick && !nonoverlapped && is_boolean_sequence(binary.left);
				return lower_property(eval, binary.right, consequent_trigger,
									  consequent_attempt_tick, aborts);
			}

		case ast::BinaryAssertionOperator::OverlappedFollowedBy:
		case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
			{
				if (lowered_by_paths(binary))
					break;
				bool nonoverlapped =
					binary.op ==
					ast::BinaryAssertionOperator::NonOverlappedFollowedBy;
				// `R #-# P` is `not (R |-> not P)`, one match of R with P from it is
				// enough (IEEE Std 1800-2017 section 16.12.9), while the consequent
				// starts on every match. That is the same where all the matches of
				// an attempt start it on one tick.
				bool admits_empty = false;
				bool one_tick = false;
				std::optional<ir::Net> antecedent;
				if (!assertion_has_unbounded_sequence(binary.left))
					antecedent = antecedent_match_event(
						eval, binary.left, trigger, attempt_tick, nonoverlapped, disable,
						&admits_empty, &one_tick);
				if (!one_tick) {
					eval.netlist.add_diag(diag::SVAFollowedByUnsupported,
										  expr_loc(binary.left));
					return {{}, false};
				}
				if (!antecedent.has_value())
					return {{}, false};
				PropertyResult result;
				if (!(nonoverlapped && admits_empty)) {
					// An empty match of R is none for `R #-# P`, while `R #=# P` is
					// `(R ##1 `true) #-# P`, which it matches on the attempt's tick
					result = lower_finite_property_from_trigger(
						eval, binary.left, trigger, attempt_tick, aborts);
				}
				bool consequent_attempt_tick =
					attempt_tick && !nonoverlapped && is_boolean_sequence(binary.left);
				PropertyResult consequent = lower_property(
					eval, binary.right, antecedent.value(), consequent_attempt_tick, aborts);
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
				if (lowered_by_paths(binary))
					break;
				PropertyResult left =
					lower_property(eval, binary.left, trigger, attempt_tick, aborts);
				PropertyResult right =
					lower_property(eval, binary.right, trigger, attempt_tick, aborts);
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
		eval, expr, trigger, attempt_tick, aborts);
}

// Do the checks lower_property() makes for a cover of the property see each of its
// successes? A branch that starts nothing, as an implication with no antecedent match
// does, leaves an attempt that succeeded with nothing to report it by. The monitor of an
// unbounded antecedent can't tell a later match of the attempt, which may still fail it.
// Paths keep the vacuous successes.
static bool cover_sees_every_success(const ast::AssertionExpr& expr) {
	if (lowered_by_paths(expr))
		return true;

	const ast::AssertionExpr& top = unwrap_property_instances(expr);
	switch (top.kind) {
	case ast::AssertionExprKind::Abort:
		return cover_sees_every_success(top.as<ast::AbortAssertionExpr>().expr);

	case ast::AssertionExprKind::Conditional:
	case ast::AssertionExprKind::Case:
		return false;

	case ast::AssertionExprKind::Binary:
		{
			const auto& binary = top.as<ast::BinaryAssertionExpr>();
			switch (binary.op) {
			case ast::BinaryAssertionOperator::Implies:
			case ast::BinaryAssertionOperator::OverlappedImplication:
			case ast::BinaryAssertionOperator::NonOverlappedImplication:
				return false;
			case ast::BinaryAssertionOperator::OverlappedFollowedBy:
			case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
				return cover_sees_every_success(binary.right);
			case ast::BinaryAssertionOperator::And:
				return cover_sees_every_success(binary.left) &&
					   cover_sees_every_success(binary.right);
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

	SvaFormulas f(eval);
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
	// when the property has succeeded for a cover to observe. Nor is there a monitor
	// for the matches of a sequence with an unbounded one, which a cover reads as strong.
	if (statement.assertionKind == ast::AssertionKind::CoverProperty &&
			(property_requires_temporal_obligations(expr) ||
			 find_weak_unbounded_sequence(expr))) {
		netlist.add_diag(diag::SVATemporalCoverUnsupported, statement.sourceRange);
		return;
	}

	// The disable iff at the top of the property, or else the default one
	const ast::AssertionExpr *property = &expr;
	const ast::Expression *condition = nullptr;
	const ast::AssertionExpr &top = unwrap_property_instances(expr);
	if (ast::DisableIffAssertionExpr::isKind(top.kind)) {
		const auto &disableiff = top.as<ast::DisableIffAssertionExpr>();
		property = &disableiff.expr;
		condition = &disableiff.condition;
	} else if (netlist.process_scope) {
		auto scope = netlist.process_scope;
		condition = scope->getCompilation().getDefaultDisable(*scope);
	}

	if (statement.assertionKind == ast::AssertionKind::CoverProperty &&
			!cover_sees_every_success(*property)) {
		netlist.add_diag(diag::SVAVacuousCoverUnsupported, statement.sourceRange);
		return;
	}

	// In an assertion or an assumption a sequence used as a property is weak, which no
	// monitor takes where it has an unbounded match. A cover reads it as strong.
	if (statement.assertionKind != ast::AssertionKind::CoverProperty) {
		if (auto weak = find_weak_unbounded_sequence(*property)) {
			netlist.add_diag(diag::SVAWeakSequenceUnsupported, expr_loc(*weak));
			return;
		}
	}

	PropertyAborts aborts;
	if (condition) {
		ir::Net disable = abort_condition(procedural.eval, *condition);
		if (statement.assertionKind == ast::AssertionKind::CoverProperty)
			aborts.cover_disable = disable;
		else
			aborts.accept = disable;
	}

	// The monitors start an attempt where the procedural condition holds, the attempts
	// then run to completion regardless of it
	size_t issued = netlist.issued_diagnostics.size();
	PropertyResult result = lower_property(procedural.eval, *property, attempt, true, aborts);
	if (!result.valid) {
		// The property is left out of the netlist, which takes an error to say so
		assert_invariant(netlist.issued_diagnostics.size() > issued);
		return;
	}
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
	// A cover has one check to see a success by, the parts of one taken apart would
	// each be covered on their own
	if (statement.assertionKind == ast::AssertionKind::CoverProperty &&
			result.obligations.size() != 1) {
		netlist.add_diag(diag::SVACompoundCoverUnsupported, statement.sourceRange);
		return;
	}

	// The label goes to the progress obligation the property states, else to its first
	// one, never to the fairness of a selection. The others are named after it.
	std::string base_cell_name = check_cell_name(netlist, statement, block);
	size_t labelled = result.obligations.size();
	for (size_t index = 0; index < result.obligations.size(); index++) {
		auto kind = result.obligations[index].kind;
		if (kind == PropertyObligationKind::Liveness) {
			labelled = index;
			break;
		}
		if (kind == PropertyObligationKind::Safety && labelled == result.obligations.size())
			labelled = index;
	}
	if (labelled == result.obligations.size())
		labelled = 0;

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

		std::string cell_name = base_cell_name;
		if (index != labelled) {
			const char *suffix = obligation.kind == PropertyObligationKind::AuxiliaryFairness
				? "$select_fair" : "$obligation";
			cell_name = netlist.backend->canvas->uniquify(base_cell_name + suffix).str();
		}
		ir::Net a = netlist.ReduceBool(obligation.a);
		ir::Net en = netlist.ReduceBool(obligation.en);
		// An assumption rules out a doomed attempt as soon as it shows, as one that
		// eval_assertion() lowers does
		if (statement.assertionKind == ast::AssertionKind::Assume && obligation.early_rejection) {
			AssertionResult checks = { a, en };
			obligation.early_rejection(checks);
			a = netlist.ReduceBool(checks.a);
			en = netlist.ReduceBool(checks.en);
		}

		// A justice goal is looked at on every cycle, not only on the ones the clock of
		// an `iff` qualified process ticks on
		ir::Net ticks = procedural.timing.background_enable;
		if (obligation.kind != PropertyObligationKind::Safety)
			a = netlist.LogicAnd(a, ticks);

		auto cell = netlist.backend->canvas->addCell(std::move(cell_name), ID($check));
		procedural.timing.extract_trigger(netlist, cell, en);

		// The goal of a monitor that answers for all the attempts at once can be met on
		// the tick an older attempt completes, with a newer one still in flight. Should the
		// clock stop for good after that, nothing would raise the obligation again, and the
		// newer attempt, which no later tick can complete, would go unnoticed. So between
		// the ticks the obligation stays raised and unmet while attempts are held, which
		// for an assumption rules out the clock stopping with one pending.
		if (obligation.held != ir::S0 && ticks != ir::S1) {
			ir::Net between = netlist.LogicNot(ticks);
			a = netlist.LogicOr(a, netlist.LogicAnd(between, netlist.LogicNot(obligation.held)));
			cell->setPort(ID::EN, {netlist.LogicOr(netlist.LogicAnd(ticks, en),
												   netlist.LogicAnd(between, obligation.held))});
		}
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

	// Yosys has no check for a restrict, nor for the cover of a sequence's matches
	if (statement.assertionKind != ast::AssertionKind::Assert &&
			statement.assertionKind != ast::AssertionKind::Assume &&
			statement.assertionKind != ast::AssertionKind::CoverProperty) {
		netlist.add_diag(diag::AssertionUnsupported, statement.sourceRange);
		return;
	}

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
											statement.assertionKind == ast::AssertionKind::CoverProperty,
											statement.assertionKind == ast::AssertionKind::Assume);

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
