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

static RTLIL::Const resize_init_const(RTLIL::Const init, int width) {
	if (init.size() == width)
		return init;
	if (init.size() == 1) {
		if (init[0] == RTLIL::State::Sx)
			return RTLIL::Const(RTLIL::State::Sx, width);
		if (init[0] == RTLIL::State::S0)
			return RTLIL::Const(RTLIL::State::S0, width);
		if (init[0] == RTLIL::State::S1)
			return RTLIL::Const(RTLIL::State::S1, width);
	}
	std::vector<RTLIL::State> bits;
	bits.reserve(width);
	for (int i = 0; i < width; i++)
		bits.push_back(i < init.size() ? init[i] : RTLIL::State::S0);
	return RTLIL::Const(bits);
}

static RTLIL::SigSpec delay_sva_sample(EvalContext& eval, RTLIL::SigSpec sig, int cycles,
									   RTLIL::Const init,
									   slang::SourceLocation loc = slang::SourceLocation::NoLocation,
									   std::string_view name_hint = "sva_delay") {
	if (cycles == 0)
		return sig;

	ProceduralContext *procedural = eval.procedural;
	if (procedural == nullptr || procedural->timing.kind != ProcessTiming::EdgeTriggered ||
			procedural->timing.triggers.size() != 1) {
		eval.netlist.add_diag(diag::SVATemporalDelayRequiresClock, loc);
		return RTLIL::SigSpec(RTLIL::Sx, sig.size());
	}

	auto &trigger = procedural->timing.triggers[0];
	for (int i = 0; i < cycles; i++) {
		auto next = eval.netlist.canvas->addWire(eval.netlist.new_id(std::string(name_hint)), sig.size());
		next->attributes = eval.netlist.staged_attributes;
		next->attributes[ID::init] = resize_init_const(init, sig.size());
		eval.netlist.add_dff(eval.netlist.new_id(std::string(name_hint)),
							 trigger.signal, sig, next, trigger.edge_polarity);
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
	std::vector<std::pair<int, RTLIL::SigBit>> edges;
	std::vector<std::pair<int, RTLIL::SigBit>> links;
};

class SvaSequenceNfa {
	EvalContext& eval;
	RTLIL::Module *module;
	RTLIL::SigBit trigger;
	RTLIL::SigBit disable = RTLIL::State::S0;
	RTLIL::SigBit throughout = RTLIL::State::S1;
	std::vector<RTLIL::SigBit> throughout_stack;
	std::vector<SvaNfaNode> nodes;
	bool materialized = false;

	RTLIL::SigBit logic_and(RTLIL::SigBit lhs, RTLIL::SigBit rhs) {
		if (lhs == RTLIL::State::S0 || rhs == RTLIL::State::S0)
			return RTLIL::State::S0;
		if (lhs == RTLIL::State::S1)
			return rhs;
		if (rhs == RTLIL::State::S1)
			return lhs;
		return module->And(eval.netlist.new_id("sva_nfa_and"), lhs, rhs);
	}

	RTLIL::SigBit logic_or(RTLIL::SigBit lhs, RTLIL::SigBit rhs) {
		if (lhs == RTLIL::State::S1 || rhs == RTLIL::State::S1)
			return RTLIL::State::S1;
		if (lhs == RTLIL::State::S0)
			return rhs;
		if (rhs == RTLIL::State::S0)
			return lhs;
		return module->Or(eval.netlist.new_id("sva_nfa_or"), lhs, rhs);
	}

	RTLIL::SigBit logic_not(RTLIL::SigBit value) {
		if (value == RTLIL::State::S0)
			return RTLIL::State::S1;
		if (value == RTLIL::State::S1)
			return RTLIL::State::S0;
		return module->Not(eval.netlist.new_id("sva_nfa_not"), value);
	}

	void make_link_order(std::vector<int>& order, int node, int minimum) const {
		if (order[node] >= minimum)
			return;
		order[node] = minimum;
		for (const auto& link : nodes[node].links)
			make_link_order(order, link.first, minimum + 1);
	}

	void add_state_ff(RTLIL::SigBit d, RTLIL::Wire *q) {
		ProceduralContext *procedural = eval.procedural;
		log_assert(procedural != nullptr);
		log_assert(procedural->timing.kind == ProcessTiming::EdgeTriggered);
		log_assert(procedural->timing.triggers.size() == 1);
		auto &clock = procedural->timing.triggers[0];
		q->attributes[ID::init] = RTLIL::State::S0;
		eval.netlist.add_dff(eval.netlist.new_id("sva_nfa_state"),
							 clock.signal, d, q, clock.edge_polarity);
	}

public:
	const int start_node;
	const int accept_node;

	SvaSequenceNfa(EvalContext& eval_, RTLIL::SigBit trigger_, RTLIL::SigBit disable_):
		eval(eval_), module(eval.netlist.canvas), trigger(trigger_), disable(disable_),
		start_node(create_node()), accept_node(create_node()) {}

	int create_node(int link_from = -1) {
		log_assert(!materialized);
		int index = int(nodes.size());
		nodes.push_back({});
		if (link_from >= 0)
			create_link(link_from, index);
		return index;
	}

	void create_edge(int from, int to, RTLIL::SigBit control = RTLIL::State::S1) {
		log_assert(!materialized);
		log_assert(0 <= from && from < int(nodes.size()));
		log_assert(0 <= to && to < int(nodes.size()));
		log_assert(to != start_node);
		control = logic_and(control, throughout);
		nodes[from].edges.emplace_back(to, control);
	}

	void create_link(int from, int to, RTLIL::SigBit control = RTLIL::State::S1) {
		log_assert(!materialized);
		log_assert(0 <= from && from < int(nodes.size()));
		log_assert(0 <= to && to < int(nodes.size()));
		log_assert(to != start_node);
		control = logic_and(control, throughout);
		nodes[from].links.emplace_back(to, control);
	}

	void push_throughout(RTLIL::SigBit condition) {
		throughout_stack.push_back(throughout);
		throughout = logic_and(throughout, condition);
	}

	void pop_throughout() {
		log_assert(!throughout_stack.empty());
		throughout = throughout_stack.back();
		throughout_stack.pop_back();
	}

	RTLIL::SigBit materialize_accept() {
		log_assert(!materialized);
		materialized = true;

		std::vector<RTLIL::Wire *> state_wires(nodes.size());
		std::vector<RTLIL::SigBit> states(nodes.size());
		std::vector<RTLIL::SigBit> next_states(nodes.size(), RTLIL::State::S0);
		RTLIL::SigBit enabled = logic_not(disable);

		for (int i = 0; i < int(nodes.size()); i++) {
			state_wires[i] = module->addWire(eval.netlist.new_id("sva_nfa_state"));
			states[i] = state_wires[i];
			if (i == start_node)
				states[i] = logic_or(states[i], trigger);
			states[i] = logic_and(states[i], enabled);
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
					RTLIL::SigBit active = logic_and(states[node], link.second);
					states[link.first] = logic_or(states[link.first], active);
				}
			}
		}

		std::vector<RTLIL::SigSpec> activations(nodes.size());
		for (int i = 0; i < int(nodes.size()); i++) {
			for (const auto& edge : nodes[i].edges)
				activations[edge.first].append(logic_and(states[i], edge.second));
		}
		for (int i = 0; i < int(nodes.size()); i++) {
			if (activations[i].size() == 1)
				next_states[i] = activations[i].as_bit();
			else if (!activations[i].empty())
				next_states[i] = module->ReduceOr(eval.netlist.new_id("sva_nfa_active"),
												 activations[i]);
		}

		for (int i = 0; i < int(nodes.size()); i++) {
			if (next_states[i] == RTLIL::State::S0)
				module->connect(state_wires[i], RTLIL::State::S0);
			else
				add_state_ff(next_states[i], state_wires[i]);
		}

		return states[accept_node];
	}
};

class SvaSequenceBuilder {
	EvalContext& eval;
	SvaSequenceNfa& fsm;
	bool valid = true;

	void unsupported(const ast::AssertionExpr& expr) {
		log_debug("SVA NFA does not support assertion AST kind %d at this position.\n",
				  int(expr.kind));
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		valid = false;
	}

	RTLIL::SigBit expression_bit(const ast::Expression& expr) {
		return eval.netlist.ReduceBool(eval.sva(expr)).as_bit();
	}

	int add_delay(int node, const ast::SequenceRange& delay,
				  const ast::AssertionExpr& owner) {
		if (delay.min > SVA_ENUMERATION_LIMIT ||
			(delay.max.has_value() &&
			 delay.max.value() > SVA_ENUMERATION_LIMIT)) {
			unsupported(owner);
			return node;
		}

		for (uint32_t i = 0; i < delay.min; i++) {
			int next = fsm.create_node();
			fsm.create_edge(node, next);
			node = next;
		}

		if (!delay.max.has_value()) {
			fsm.create_edge(node, node);
			return node;
		}

		for (uint32_t i = delay.min; i < delay.max.value(); i++) {
			int next = fsm.create_node();
			fsm.create_edge(node, next);
			fsm.create_link(node, next);
			node = next;
		}
		return node;
	}

	int parse_consecutive(int start, const ast::AssertionExpr& body,
						  const ast::SequenceRange& range,
						  const ast::AssertionExpr& owner) {
		if (range.min > SVA_ENUMERATION_LIMIT ||
			(range.max.has_value() &&
			 range.max.value() > SVA_ENUMERATION_LIMIT)) {
			unsupported(owner);
			return start;
		}

		int result = fsm.create_node();
		int node = start;
		int loop_start = -1;
		int loop_end = -1;

		if (range.min == 0)
			fsm.create_link(start, result);

		for (uint32_t i = 0; i < range.min && valid; i++) {
			if (i != 0) {
				int next = fsm.create_node();
				fsm.create_edge(node, next);
				node = next;
			}
			loop_start = node;
			node = parse(node, body);
			loop_end = node;
		}

		if (!valid)
			return result;

		if (!range.max.has_value()) {
			if (range.min == 0) {
				loop_start = start;
				loop_end = parse(loop_start, body);
			}
			fsm.create_link(loop_end, result);
			fsm.create_edge(loop_end, loop_start);
			return result;
		}

		if (range.min > 0)
			fsm.create_link(node, result);

		for (uint32_t i = range.min; i < range.max.value() && valid; i++) {
			int next = fsm.create_node();
			if (i == 0)
				fsm.create_link(node, next);
			else
				fsm.create_edge(node, next);
			node = parse(next, body);
			fsm.create_link(node, result);
		}
		return result;
	}

	bool simple_condition(const ast::AssertionExpr& expr, RTLIL::SigBit& condition) {
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
		condition = simple.isNullExpr ? RTLIL::State::S0 : expression_bit(simple.expr);
		return true;
	}

	int parse_nonconsecutive(int start, const ast::AssertionExpr& body,
							 const ast::SequenceRepetition& repetition,
							 const ast::AssertionExpr& owner) {
		RTLIL::SigBit condition;
		if (!simple_condition(body, condition)) {
			unsupported(owner);
			return start;
		}
		if (repetition.range.min > SVA_ENUMERATION_LIMIT ||
			(repetition.range.max.has_value() &&
			 repetition.range.max.value() > SVA_ENUMERATION_LIMIT)) {
			unsupported(owner);
			return start;
		}

		RTLIL::SigBit not_condition =
			eval.netlist.LogicNot(condition).as_bit();
		int node = fsm.create_node(start);

		for (uint32_t i = 0; i < repetition.range.min; i++) {
			int wait = fsm.create_node();
			fsm.create_edge(wait, wait, not_condition);
			if (i == 0)
				fsm.create_link(node, wait);
			else
				fsm.create_edge(node, wait);
			int matched = fsm.create_node();
			fsm.create_link(wait, matched, condition);
			node = matched;
		}

		if (!repetition.range.max.has_value()) {
			int wait = fsm.create_node();
			fsm.create_edge(wait, wait, not_condition);
			fsm.create_edge(node, wait);
			fsm.create_link(wait, node, condition);
		} else {
			for (uint32_t i = repetition.range.min;
				 i < repetition.range.max.value(); i++) {
				int wait = fsm.create_node();
				fsm.create_edge(wait, wait, not_condition);
				if (i == 0)
					fsm.create_link(node, wait);
				else
					fsm.create_edge(node, wait);
				int matched = fsm.create_node();
				fsm.create_link(wait, matched, condition);
				fsm.create_link(node, matched);
				node = matched;
			}
		}

		if (repetition.kind == ast::SequenceRepetition::Nonconsecutive)
			fsm.create_edge(node, node);
		return node;
	}

	int apply_repetition(int start, const ast::AssertionExpr& body,
						 const std::optional<ast::SequenceRepetition>& repetition,
						 const ast::AssertionExpr& owner) {
		if (!repetition.has_value())
			return parse(start, body);
		if (repetition->kind == ast::SequenceRepetition::Consecutive)
			return parse_consecutive(start, body, repetition->range, owner);
		return parse_nonconsecutive(start, body, *repetition, owner);
	}

	bool is_zero_consecutive(const ast::AssertionExpr& expr) const {
		if (expr.kind == ast::AssertionExprKind::Simple) {
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (!simple.repetition.has_value() &&
				simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
				const auto& instance =
					simple.expr.as<ast::AssertionInstanceExpression>();
				return !instance.isRecursiveProperty &&
					   is_zero_consecutive(instance.body);
			}
			return simple.repetition.has_value() &&
				   simple.repetition->kind ==
					   ast::SequenceRepetition::Consecutive &&
				   simple.repetition->range.min == 0;
		}
		if (expr.kind == ast::AssertionExprKind::SequenceWithMatch) {
			const auto& with_match =
				expr.as<ast::SequenceWithMatchExpr>();
			return with_match.matchItems.empty() &&
				   with_match.repetition.has_value() &&
				   with_match.repetition->kind ==
					   ast::SequenceRepetition::Consecutive &&
				   with_match.repetition->range.min == 0;
		}
		return false;
	}

	int parse_zero_consecutive(int start, const ast::AssertionExpr& expr,
							   bool allow_empty, bool add_pre_delay,
							   bool add_post_delay) {
		if (expr.kind == ast::AssertionExprKind::Simple) {
			const auto& simple = expr.as<ast::SimpleAssertionExpr>();
			if (!simple.repetition.has_value() &&
				simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
				const auto& instance =
					simple.expr.as<ast::AssertionInstanceExpression>();
				if (!instance.isRecursiveProperty)
					return parse_zero_consecutive(
						start, instance.body, allow_empty,
						add_pre_delay, add_post_delay);
			}
			log_assert(simple.repetition.has_value());
			ast::SimpleAssertionExpr body(simple.expr, std::nullopt,
										 simple.isNullExpr);
			body.syntax = expr.syntax;
			return parse_zero_consecutive_body(
				start, body, simple.repetition->range, expr,
				allow_empty, add_pre_delay, add_post_delay);
		}

		const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
		log_assert(with_match.matchItems.empty());
		log_assert(with_match.repetition.has_value());
		return parse_zero_consecutive_body(
			start, with_match.expr, with_match.repetition->range, expr,
			allow_empty, add_pre_delay, add_post_delay);
	}

	int parse_zero_consecutive_body(
			int start, const ast::AssertionExpr& body,
			const ast::SequenceRange& original_range,
			const ast::AssertionExpr& owner, bool allow_empty,
			bool add_pre_delay, bool add_post_delay) {
		int result = fsm.create_node();
		if (allow_empty)
			fsm.create_link(start, result);

		if (!original_range.max.has_value() ||
			original_range.max.value() > 0) {
			int node = start;
			if (add_pre_delay) {
				int next = fsm.create_node();
				fsm.create_edge(node, next);
				node = next;
			}

			ast::SequenceRange nonempty {
				1, original_range.max
			};
			node = parse_consecutive(node, body, nonempty, owner);

			if (add_post_delay) {
				int next = fsm.create_node();
				fsm.create_edge(node, next);
				node = next;
			}
			fsm.create_link(node, result);
		}
		return result;
	}

public:
	SvaSequenceBuilder(EvalContext& eval_, SvaSequenceNfa& fsm_):
		eval(eval_), fsm(fsm_) {}

	bool good() const {
		return valid;
	}

	int parse(int start, const ast::AssertionExpr& expr) {
		if (!valid)
			return start;

		switch (expr.kind) {
		case ast::AssertionExprKind::Simple:
			{
				const auto& simple = expr.as<ast::SimpleAssertionExpr>();
				if (simple.expr.kind == ast::ExpressionKind::AssertionInstance) {
					const auto& instance =
						simple.expr.as<ast::AssertionInstanceExpression>();
					if (instance.isRecursiveProperty) {
						unsupported(expr);
						return start;
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
					simple.isNullExpr ? RTLIL::State::S0 :
					expression_bit(simple.expr));
				return node;
			}

		case ast::AssertionExprKind::SequenceConcat:
			{
				const auto& concat = expr.as<ast::SequenceConcatExpr>();
				int node = start;
				bool reduce_delay = false;
				for (size_t index = 0;
					 index < concat.elements.size(); index++) {
					const auto& element = concat.elements[index];
					ast::SequenceRange delay = element.delay;
					if (reduce_delay) {
						log_assert(delay.min > 0);
						delay.min--;
						if (delay.max.has_value()) {
							log_assert(delay.max.value() > 0);
							delay.max = delay.max.value() - 1;
						}
					}
					reduce_delay = false;

					bool zero_repeat =
						is_zero_consecutive(*element.sequence);
					bool add_pre_delay =
						zero_repeat && delay.min > 0;
					bool add_post_delay =
						zero_repeat && !add_pre_delay && index == 0 &&
						index + 1 < concat.elements.size() &&
						concat.elements[index + 1].delay.min > 0;
					bool allow_empty =
						add_pre_delay || add_post_delay;

					if (add_pre_delay) {
						delay.min--;
						if (delay.max.has_value())
							delay.max = delay.max.value() - 1;
					}
					if (add_post_delay)
						reduce_delay = true;

					node = add_delay(node, delay, expr);
					if (zero_repeat && concat.elements.size() > 1)
						node = parse_zero_consecutive(
							node, *element.sequence, allow_empty,
							add_pre_delay, add_post_delay);
					else
						node = parse(node, *element.sequence);
				}
				return node;
			}

		case ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported,
										  expr_loc(expr));
					valid = false;
					return start;
				}
				return apply_repetition(start, with_match.expr,
									   with_match.repetition, expr);
			}

		case ast::AssertionExprKind::FirstMatch:
			{
				const auto& first = expr.as<ast::FirstMatchAssertionExpr>();
				if (!first.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported,
										  expr_loc(expr));
					valid = false;
					return start;
				}
				// First-match and ordinary sequence acceptance have the same
				// language when no match actions are present.  Priority only
				// affects which successful path executes those actions.
				return parse(start, first.seq);
			}

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
								return start;
							}
							delay = unary.range->min;
						}
						ast::SequenceRange range {delay, delay};
						return parse(add_delay(start, range, expr), unary.expr);
					}
				default:
					unsupported(expr);
					return start;
				}
			}

		case ast::AssertionExprKind::Binary:
			{
				const auto& binary = expr.as<ast::BinaryAssertionExpr>();
				switch (binary.op) {
				case ast::BinaryAssertionOperator::Or:
					{
						int result = fsm.create_node();
						fsm.create_link(parse(start, binary.left), result);
						fsm.create_link(parse(start, binary.right), result);
						return result;
					}
				case ast::BinaryAssertionOperator::Throughout:
					{
						RTLIL::SigBit condition;
						if (!simple_condition(binary.left, condition)) {
							unsupported(expr);
							return start;
						}
						fsm.push_throughout(condition);
						int result = parse(start, binary.right);
						fsm.pop_throughout();
						return result;
					}
				case ast::BinaryAssertionOperator::And:
				case ast::BinaryAssertionOperator::Intersect:
				case ast::BinaryAssertionOperator::Within:
					// These regular-language products need determinization.
					// The finite path lowering handles bounded instances.
					unsupported(expr);
					return start;
				default:
					unsupported(expr);
					return start;
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
			return start;
		}
		log_abort();
	}
};

static std::optional<RTLIL::SigBit>
synthesize_unbounded_sequence(EvalContext& eval, const ast::AssertionExpr& sequence,
							  RTLIL::SigBit trigger, RTLIL::SigBit disable,
							  bool allow_leading_delay = false) {
	SvaSequenceNfa fsm(eval, trigger, disable);
	SvaSequenceBuilder builder(eval, fsm);
	int entry = fsm.create_node(fsm.start_node);
	if (allow_leading_delay)
		fsm.create_edge(entry, entry);
	int end = builder.parse(entry, sequence);
	if (!builder.good())
		return std::nullopt;
	fsm.create_link(end, fsm.accept_node);
	return fsm.materialize_accept();
}

struct AssertionMatch {
	EvalContext& eval;
	RTLIL::SigSpec sig;
	RTLIL::SigSpec en;
	int start;
	int history;
	bool empty;
	slang::SourceLocation loc;

	AssertionMatch(EvalContext& eval_, RTLIL::SigSpec sig_,
				   slang::SourceLocation loc_ = slang::SourceLocation::NoLocation):
		eval(eval_), sig(sig_), en(true), start(0), history(0),
		empty(false), loc(loc_) {}

private:
	AssertionMatch(EvalContext& eval_, RTLIL::SigSpec sig_, RTLIL::SigSpec en_,
				   int start_, int history_, bool empty_,
				   slang::SourceLocation loc_):
		eval(eval_), sig(sig_), en(en_), start(start_), history(history_),
		empty(empty_), loc(loc_) {}

public:
	static AssertionMatch Empty(EvalContext& eval, slang::SourceLocation loc) {
		return { eval, true, true, 0, 0, true, loc };
	}

	void operator=(AssertionMatch other) {
		sig = other.sig;
		en = other.en;
		start = other.start;
		history = other.history;
		empty = other.empty;
		loc = other.loc;
	}

	AssertionMatch shift(int time) const {
		if (empty)
			return { eval, sig, en, start + time, history + time,
					 true, loc };

		RTLIL::SigSpec shifted_sig = sig;
		RTLIL::SigSpec shifted_en = en;
		if (!sig.is_fully_const())
			shifted_sig = delay_sva_sample(eval, sig, time, RTLIL::State::Sx, loc);
		if (!en.is_fully_const())
			shifted_en = delay_sva_sample(eval, en, time, RTLIL::State::S0, loc);
		return { eval, shifted_sig, shifted_en, start + time,
				 history + time, false, loc };
	}

	AssertionMatch operator||(AssertionMatch& other) const {
		log_assert(!empty && !other.empty);

		RTLIL::SigSpec gated = en.is_fully_const() && en.as_bool() ? sig : eval.netlist.LogicAnd(en, sig);
		RTLIL::SigSpec other_gated = other.en.is_fully_const() && other.en.as_bool()
										  ? other.sig
										  : eval.netlist.LogicAnd(other.en, other.sig);
		RTLIL::SigSpec result_sig = eval.netlist.LogicOr(gated, other_gated);
		RTLIL::SigSpec result_en = eval.netlist.LogicOr(en, other.en);
		return { eval, result_sig, result_en,
				 std::max(other.start, start),
				 std::max(other.history, history), false, loc };
	}

	AssertionMatch operator&&(AssertionMatch& other) const {
		log_assert(!empty && !other.empty);

		RTLIL::SigSpec result_en = eval.netlist.LogicAnd(en, other.en);
		if (sig.is_fully_const() && sig.as_bool())
			return { eval, other.sig, result_en,
					 std::max(other.start, start),
					 std::max(other.history, history), false, loc };
		if (sig.is_fully_const() && !sig.as_bool())
			return { eval, false, result_en,
					 std::max(other.start, start),
					 std::max(other.history, history), false, loc };
		if (other.sig.is_fully_const() && other.sig.as_bool())
			return { eval, sig, result_en,
					 std::max(other.start, start),
					 std::max(other.history, history), false, loc };
		if (other.sig.is_fully_const() && !other.sig.as_bool())
			return { eval, false, result_en,
					 std::max(other.start, start),
					 std::max(other.history, history), false, loc };
		return { eval, eval.netlist.LogicAnd(sig, other.sig), eval.netlist.LogicAnd(en, other.en),
				 std::max(other.start, start),
				 std::max(other.history, history), false, loc };
	}

	AssertionMatch operator!() const {
		log_assert(!empty);

		if (sig.is_fully_const())
			return { eval, !sig.as_bool(), en, start, history, false, loc };
		return { eval, eval.netlist.LogicNot(sig), en, start, history,
				 false, loc };
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
	std::sort(paths.begin(), paths.end(), (amcmp) {});

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
	log_assert(!paths.empty());

	for (auto path : paths) {
		if (path.empty)
			return { path.eval, true, path.loc };
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

static std::vector<AssertionMatch> not_vec(std::vector<AssertionMatch> in) {
	if (in.empty()) return {};
	return { !collapse_or(in) };
}

static RTLIL::SigSpec gated_signal(const AssertionMatch& path) {
	log_assert(!path.empty);

	if (path.sig.is_fully_const())
		return path.sig.as_bool() ? path.en : RTLIL::SigSpec(false);
	return path.eval.netlist.LogicAnd(path.en, path.sig);
}

static bool check_finite_range(EvalContext& eval, const ast::AssertionExpr& expr,
							   uint32_t min, uint32_t max) {
	if (max < min || max > SVA_ENUMERATION_LIMIT) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
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
					if (offset > 0)
						new_own_paths.push_back(AssertionMatch(path.eval, true, path.loc).shift(path.start + offset - 1));
					continue;
				}
				if (path.empty) {
					if (offset > 0) {
						// An empty sequence fused through ##N consumes one less
						// sample than an ordinary left operand, but its match
						// still belongs to the endpoint of this concatenation.
						// Keep those coordinates distinct so alternative paths
						// do not delay the right operand a second time.
						AssertionMatch combined =
							inner.shift(path.start + offset - 1);
						combined.start =
							path.start + offset + inner.start;
						combined.history = std::max(
							combined.history,
							path.history + offset + inner.history);
						new_own_paths.push_back(combined);
					}
					continue;
				}
				if (inner.empty) {
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
				if (offset > 0)
					paths.push_back(AssertionMatch(eval, true, loc).shift(offset - 1));
				else
					paths.push_back(inner);
			} else {
				paths.push_back(AssertionMatch(eval, true, loc).shift(offset + inner.start) && inner);
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
		eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
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

// Empty indicates error
static std::vector<AssertionMatch> synthesizeAssertionExpr(EvalContext& eval, const ast::AssertionExpr& expr) {
	switch (expr.kind) {
		case slang::ast::AssertionExprKind::Invalid:
			log_abort();
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
					paths = synthesizeAssertionExpr(eval, instance.body);
				} else {
					paths = {{ eval, simple.isNullExpr ? false : eval.sva(simple.expr), expr_loc(expr) }};
				}
				if (!apply_repetition(eval, expr, simple.repetition, paths))
					return {};
				return paths;
			}
		case slang::ast::AssertionExprKind::SequenceConcat:
			{
				const auto& sequence = expr.as<ast::SequenceConcatExpr>();
				std::vector<AssertionMatch> own_paths;
				for (int i = 0; i < sequence.elements.size(); i++) {
					auto inner_paths = synthesizeAssertionExpr(eval, *sequence.elements[i].sequence);

					auto delay = sequence.elements[i].delay;
					if (!delay.max.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
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
					return not_vec(synthesizeAssertionExpr(eval, uop.expr));

				case slang::ast::UnaryAssertionOperator::Eventually:
				case slang::ast::UnaryAssertionOperator::SEventually:
				{
					if (!uop.range.has_value() || !uop.range->max.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
						return {};
					}
					if (!check_finite_range(eval, expr, uop.range->min, uop.range->max.value()))
						return {};

					std::vector<AssertionMatch> true_path = {{eval, true, expr_loc(expr)}};
					return seq_vec(true_path, uop.range->min, uop.range->max.value(),
								   synthesizeAssertionExpr(eval, uop.expr));
				}

				case slang::ast::UnaryAssertionOperator::NextTime:
				case slang::ast::UnaryAssertionOperator::SNextTime:
				{
					uint32_t delay = 1;
					if (uop.range.has_value()) {
						if (!uop.range->max.has_value() || uop.range->min != uop.range->max.value()) {
							eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
							return {};
						}
						if (!check_finite_range(eval, expr, uop.range->min, uop.range->max.value()))
							return {};
						delay = uop.range->min;
					}

					std::vector<AssertionMatch> true_path = {{eval, true, expr_loc(expr)}};
					return seq_vec(true_path, delay, delay, synthesizeAssertionExpr(eval, uop.expr));
				}

				case slang::ast::UnaryAssertionOperator::Always:
				case slang::ast::UnaryAssertionOperator::SAlways:
					eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
					return {};
				}
			}
		case slang::ast::AssertionExprKind::Binary:
			{
				const auto& biop = expr.as<ast::BinaryAssertionExpr>();
				auto left = synthesizeAssertionExpr(eval, biop.left);
				auto right = synthesizeAssertionExpr(eval, biop.right);

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
							if (a.start >= b.start)
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
					if (left.empty() || right.empty()) return {}; // Propogate errors
					left.insert(left.end(), right.begin(), right.end());
					return left;
				}
				case ast::BinaryAssertionOperator::Throughout:
				{
					if (left.empty() || right.empty()) return {};

					auto condition = collapse_or(left);
					if (condition.start != 0) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
						return {};
					}

					std::vector<AssertionMatch> results;
					for (auto path : right) {
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
							if (a.start == b.start)
								results.push_back(a && b);
						}
					}
					return compress_paths(results);
				}
				case ast::BinaryAssertionOperator::OverlappedImplication:
					return not_vec(seq_vec(left, 0, 0, not_vec(right)));
				case ast::BinaryAssertionOperator::NonOverlappedImplication:
					return not_vec(seq_vec(left, 1, 1, not_vec(right)));
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
					eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
					return {};
				}
			}
		case slang::ast::AssertionExprKind::Clocking:
			{
				eval.netlist.add_diag(diag::UnsupportedSVAFeature, expr_loc(expr));
				return {};
			}
		case slang::ast::AssertionExprKind::DisableIff:
			{
				const auto& disableiff = expr.as<ast::DisableIffAssertionExpr>();
				auto disable = (AssertionMatch) {eval, eval(disableiff.condition), expr_loc(expr)};
				auto inner = synthesizeAssertionExpr(eval, disableiff.expr);
				if (inner.empty()) return {};

				int max_start = 0;
				for (auto path : inner)
					max_start = std::max(max_start, path.start);

				std::vector<AssertionMatch> disables;
				disables.push_back(disable);
				std::vector<AssertionMatch> disable_windows;
				for (int i = 0; i <= max_start; i++) {
					std::optional<AssertionMatch> this_disables = {};
					for (int t = 0; t <= i; t++) {
						while (disables.size() <= t)
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
					auto success = (AssertionMatch) {eval, true, path.loc};
					success.en = eval.netlist.LogicAnd(gated_signal(path), not_disabled.sig);
					success.start = path.start;
					results.push_back(success);
				}

				auto base = collapse_or(inner);
				auto not_disabled_to_end = !disable_windows[max_start];
				auto check_failure = (AssertionMatch) {eval, false, expr_loc(expr)};
				check_failure.en = eval.netlist.LogicAnd(base.en, not_disabled_to_end.sig);
				check_failure.start = max_start;
				results.push_back(check_failure);

				return compress_paths(results);
			}

		case slang::ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported,
										  expr_loc(expr));
					return {};
				}

				auto paths = synthesizeAssertionExpr(eval, with_match.expr);
				if (!apply_repetition(eval, expr, with_match.repetition, paths))
					return {};
				return paths;
			}
		case slang::ast::AssertionExprKind::FirstMatch:
			{
				const auto& first_match = expr.as<ast::FirstMatchAssertionExpr>();
				if (!first_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::SVAMatchItemsUnsupported,
										  expr_loc(expr));
					return {};
				}

				auto paths = compress_paths(synthesizeAssertionExpr(eval, first_match.seq));
				std::vector<AssertionMatch> results;

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
			eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
			return {};
	}
	log_abort(); // Unreachable
};

struct AssertionResult {
	RTLIL::SigSpec a;
	RTLIL::SigSpec en;
};

enum class PropertyObligationKind {
	Safety,
	Liveness,
	AuxiliaryFairness
};

struct PropertyObligation {
	PropertyObligationKind kind;
	RTLIL::SigSpec a;
	RTLIL::SigSpec en;
	RTLIL::SigSpec active;
};

struct PropertyResult {
	std::vector<PropertyObligation> obligations;
	bool valid = true;
};

struct EventualCandidate {
	RTLIL::SigBit match;
	uint32_t duration;
};

static void add_liveness_obligation(PropertyResult& result, EvalContext& eval,
									RTLIL::SigBit goal, RTLIL::SigBit trigger,
									RTLIL::SigBit disable);
static RTLIL::SigBit add_pending_monitor(
		EvalContext& eval, RTLIL::SigBit trigger,
		RTLIL::SigBit completion, RTLIL::SigBit disable,
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
	log_abort();
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

static const ast::ClockingAssertionExpr *get_top_clocking_expr(const ast::AssertionExpr &expr)
{
	if (ast::ClockingAssertionExpr::isKind(expr.kind))
		return &expr.as<ast::ClockingAssertionExpr>();

	if (expr.kind != ast::AssertionExprKind::Simple)
		return nullptr;

	const auto &simple = expr.as<ast::SimpleAssertionExpr>();
	if (simple.expr.kind != ast::ExpressionKind::AssertionInstance)
		return nullptr;

	const auto &instance = simple.expr.as<ast::AssertionInstanceExpression>();
	if (instance.isRecursiveProperty)
		return nullptr;

	return get_top_clocking_expr(instance.body);
}

static bool timing_matches_process(const ProcessTiming &expected, const ProcessTiming &actual)
{
	if (expected.kind != actual.kind)
		return false;
	if (expected.triggers.size() != actual.triggers.size())
		return false;

	for (size_t i = 0; i < expected.triggers.size(); i++) {
		if (expected.triggers[i].edge_polarity != actual.triggers[i].edge_polarity)
			return false;
		if (expected.triggers[i].signal != actual.triggers[i].signal)
			return false;
	}

	return true;
}

AssertionResult evalAssertion(EvalContext& eval, const ast::AssertionExpr& assertion) {
	auto paths = synthesizeAssertionExpr(eval, assertion);
	if (paths.empty()) return { false, false }; // Ran into an error

	auto sig = collapse_or(paths);

	auto init_escape = delay_sva_sample(
		eval, RTLIL::State::S0, sig.history, RTLIL::State::S1, sig.loc);
	auto frame_ready = eval.netlist.LogicNot(init_escape);
	return { sig.sig, eval.netlist.LogicAnd(sig.en, frame_ready) };
}

static RTLIL::SigBit sva_or(EvalContext& eval, RTLIL::SigBit lhs, RTLIL::SigBit rhs) {
	if (lhs == RTLIL::State::S1 || rhs == RTLIL::State::S1)
		return RTLIL::State::S1;
	if (lhs == RTLIL::State::S0)
		return rhs;
	if (rhs == RTLIL::State::S0)
		return lhs;
	return eval.netlist.LogicOr(lhs, rhs).as_bit();
}

static RTLIL::SigBit sva_and(EvalContext& eval, RTLIL::SigBit lhs, RTLIL::SigBit rhs) {
	if (lhs == RTLIL::State::S0 || rhs == RTLIL::State::S0)
		return RTLIL::State::S0;
	if (lhs == RTLIL::State::S1)
		return rhs;
	if (rhs == RTLIL::State::S1)
		return lhs;
	return eval.netlist.LogicAnd(lhs, rhs).as_bit();
}

static RTLIL::SigBit sva_not(EvalContext& eval, RTLIL::SigBit value) {
	if (value == RTLIL::State::S0)
		return RTLIL::State::S1;
	if (value == RTLIL::State::S1)
		return RTLIL::State::S0;
	return eval.netlist.LogicNot(value).as_bit();
}

static RTLIL::SigBit delayed_abortable_trigger(
		EvalContext& eval, RTLIL::SigBit trigger, uint32_t cycles,
		RTLIL::SigBit disable, slang::SourceLocation loc) {
	RTLIL::SigBit active = sva_and(eval, trigger, sva_not(eval, disable));
	for (uint32_t i = 0; i < cycles; i++) {
		active = sva_and(eval, active, sva_not(eval, disable));
		active = delay_sva_sample(eval, active, 1, RTLIL::State::S0, loc,
								  "sva_obligation").as_bit();
	}
	return sva_and(eval, active, sva_not(eval, disable));
}

static std::optional<std::vector<EventualCandidate>>
finite_eventual_candidates(EvalContext& eval, const ast::AssertionExpr& sequence,
						   uint32_t leading_delay = 0) {
	if (assertion_has_unbounded_sequence(sequence))
		return std::nullopt;

	auto paths = synthesizeAssertionExpr(eval, sequence);
	if (paths.empty())
		return std::nullopt;

	std::vector<EventualCandidate> candidates;
	for (const auto& path : compress_paths(paths)) {
		if (path.start < 0 ||
			uint64_t(path.start) + leading_delay > SVA_ENUMERATION_LIMIT) {
			eval.netlist.add_diag(diag::AssertionUnsupported, path.loc);
			return std::nullopt;
		}
		RTLIL::SigBit frame_ready = sva_not(
			eval,
			eval.netlist.ReduceBool(delay_sva_sample(
				eval, RTLIL::State::S0, path.history,
				RTLIL::State::S1, path.loc)).as_bit());
		RTLIL::SigBit match = sva_and(
			eval, eval.netlist.ReduceBool(path.sig).as_bit(),
			eval.netlist.ReduceBool(path.en).as_bit());
		match = sva_and(eval, match, frame_ready);
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

	auto paths = synthesizeAssertionExpr(
		eval, *concat.elements[0].sequence);
	if (paths.empty())
		return std::nullopt;
	for (size_t i = 1; i < concat.elements.size(); i++) {
		const auto& element = concat.elements[i];
		if (!element.delay.max.has_value() ||
			element.delay.min > SVA_ENUMERATION_LIMIT ||
			element.delay.max.value() > SVA_ENUMERATION_LIMIT ||
			assertion_has_unbounded_sequence(*element.sequence))
			return std::nullopt;
		auto next = synthesizeAssertionExpr(eval, *element.sequence);
		if (next.empty())
			return std::nullopt;
		paths = seq_vec(paths, element.delay.min,
						element.delay.max.value(), next);
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
		RTLIL::SigBit frame_ready = sva_not(
			eval,
			eval.netlist.ReduceBool(delay_sva_sample(
				eval, RTLIL::State::S0, path.history,
				RTLIL::State::S1, path.loc)).as_bit());
		RTLIL::SigBit match = sva_and(
			eval, eval.netlist.ReduceBool(path.sig).as_bit(),
			eval.netlist.ReduceBool(path.en).as_bit());
		match = sva_and(eval, match, frame_ready);
		candidates.push_back(
			{match, uint32_t(path.start) + leading_delay});
	}
	return candidates;
}

static void add_response_liveness_obligation(
		PropertyResult& result, EvalContext& eval,
		const std::vector<EventualCandidate>& candidates,
		RTLIL::SigBit trigger, RTLIL::SigBit disable,
		slang::SourceLocation loc) {
	log_assert(!candidates.empty());
	uint32_t maximum_duration = 0;
	RTLIL::SigBit any_match = RTLIL::State::S0;
	for (const auto& candidate : candidates) {
		maximum_duration = std::max(maximum_duration, candidate.duration);
		any_match = sva_or(eval, any_match, candidate.match);
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
	std::vector<RTLIL::Wire *> recent_wires;
	std::vector<RTLIL::SigBit> recent;
	recent_wires.reserve(maximum_duration - 1);
	recent.reserve(maximum_duration - 1);
	for (uint32_t age = 1; age < maximum_duration; age++) {
		auto *wire = eval.netlist.canvas->addWire(
			eval.netlist.new_id("sva_response_recent"));
		wire->attributes[ID::init] = RTLIL::State::S0;
		recent_wires.push_back(wire);
		recent.push_back(wire);
	}
	auto *old_wire = eval.netlist.canvas->addWire(
		eval.netlist.new_id("sva_response_old"));
	old_wire->attributes[ID::init] = RTLIL::State::S0;
	RTLIL::SigBit old = old_wire;

	trigger = sva_and(eval, trigger, sva_not(eval, disable));
	std::vector<RTLIL::SigBit> ages;
	ages.reserve(maximum_duration);
	ages.push_back(trigger);
	ages.insert(ages.end(), recent.begin(), recent.end());

	std::vector<RTLIL::SigBit> clears(maximum_duration,
									  RTLIL::State::S0);
	for (uint32_t age = 0; age < maximum_duration; age++) {
		for (const auto& candidate : candidates) {
			if (candidate.duration <= age)
				clears[age] =
					sva_or(eval, clears[age], candidate.match);
		}
	}

	RTLIL::SigBit pending = old;
	RTLIL::SigBit completion = sva_and(eval, old, any_match);
	std::vector<RTLIL::SigBit> remaining;
	remaining.reserve(ages.size());
	for (uint32_t age = 0; age < ages.size(); age++) {
		pending = sva_or(eval, pending, ages[age]);
		completion = sva_or(
			eval, completion,
			sva_and(eval, ages[age], clears[age]));
		RTLIL::SigBit keep =
			sva_and(eval, ages[age], sva_not(eval, clears[age]));
		remaining.push_back(
			sva_and(eval, keep, sva_not(eval, disable)));
	}
	RTLIL::SigBit old_next =
		sva_and(eval, old, sva_not(eval, any_match));
	old_next = sva_and(eval, old_next, sva_not(eval, disable));
	old_next = sva_or(eval, old_next, remaining.back());

	ProceduralContext *procedural = eval.procedural;
	if (procedural == nullptr ||
		procedural->timing.kind != ProcessTiming::EdgeTriggered ||
		procedural->timing.triggers.size() != 1) {
		eval.netlist.add_diag(diag::SVATemporalDelayRequiresClock, loc);
		result.valid = false;
		return;
	}
	auto& clock = procedural->timing.triggers[0];
	for (uint32_t i = 0; i < recent_wires.size(); i++) {
		RTLIL::SigBit next = i == 0 ? remaining[0] : remaining[i];
		eval.netlist.add_dff(
			eval.netlist.new_id("sva_response_recent"),
			clock.signal, next, recent_wires[i], clock.edge_polarity);
	}
	eval.netlist.add_dff(eval.netlist.new_id("sva_response_old"),
						 clock.signal, old_next, old_wire,
						 clock.edge_polarity);

	RTLIL::SigBit goal = sva_or(
		eval, sva_not(eval, pending), completion);
	goal = sva_or(eval, goal, disable);
	result.obligations.push_back(
		{PropertyObligationKind::Liveness, goal,
		 RTLIL::State::S1, pending});
}

static std::optional<RTLIL::SigBit>
antecedent_match_event(EvalContext& eval, const ast::AssertionExpr& sequence,
					   RTLIL::SigBit trigger, bool nonoverlapped,
					   RTLIL::SigBit disable) {
	RTLIL::SigBit result = RTLIL::State::S0;
	if (assertion_has_unbounded_sequence(sequence)) {
		auto match = synthesize_unbounded_sequence(
			eval, sequence, trigger, disable);
		if (!match.has_value())
			return std::nullopt;
		result = match.value();
	} else {
		auto paths = synthesizeAssertionExpr(eval, sequence);
		if (paths.empty())
			return RTLIL::State::S0;
		for (const auto& path : compress_paths(paths)) {
			if (path.start < 0 ||
				uint32_t(path.start) > SVA_ENUMERATION_LIMIT) {
				eval.netlist.add_diag(diag::AssertionUnsupported, path.loc);
				return std::nullopt;
			}
			RTLIL::SigBit path_trigger = delayed_abortable_trigger(
				eval, trigger, uint32_t(path.start), disable, path.loc);
			RTLIL::SigBit path_match = sva_and(
				eval, eval.netlist.ReduceBool(path.sig).as_bit(),
				eval.netlist.ReduceBool(path.en).as_bit());
			path_match = sva_and(eval, path_match, path_trigger);
			result = sva_or(eval, result, path_match);
		}
	}
	if (nonoverlapped)
		result = delayed_abortable_trigger(
			eval, result, 1, disable, expr_loc(sequence));
	return sva_and(eval, result, sva_not(eval, disable));
}

static void add_liveness_obligation(PropertyResult& result, EvalContext& eval,
									RTLIL::SigBit goal, RTLIL::SigBit trigger,
									RTLIL::SigBit disable) {
	// A disable aborts all currently pending attempts, and prevents a new
	// attempt from starting in the disabled cycle.
	goal = sva_or(eval, goal, disable);
	trigger = sva_and(eval, trigger, sva_not(eval, disable));
	RTLIL::SigBit active = add_pending_monitor(
		eval, trigger, goal, RTLIL::State::S0,
		slang::SourceLocation::NoLocation);
	result.obligations.push_back(
		{PropertyObligationKind::Liveness, goal, trigger, active});
}

static void add_safety_obligation(PropertyResult& result, RTLIL::SigSpec a,
								  RTLIL::SigSpec en) {
	result.obligations.push_back(
		{PropertyObligationKind::Safety, a, en, RTLIL::State::S0});
}

static std::optional<RTLIL::SigBit>
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
		RTLIL::SigBit value = simple.isNullExpr
			? RTLIL::State::S0
			: eval.netlist.ReduceBool(eval.sva(simple.expr)).as_bit();
		return negate ? sva_not(eval, value) : value;
	}
	if (current->kind == ast::AssertionExprKind::Unary) {
		const auto& unary = current->as<ast::UnaryAssertionExpr>();
		if (unary.op == ast::UnaryAssertionOperator::Not)
			return simple_property_signal(eval, unary.expr, !negate);
	}
	return std::nullopt;
}

static RTLIL::SigBit add_pending_monitor(EvalContext& eval, RTLIL::SigBit trigger,
										RTLIL::SigBit completion,
										RTLIL::SigBit disable,
										slang::SourceLocation loc) {
	RTLIL::Wire *pending = eval.netlist.canvas->addWire(
		eval.netlist.new_id("sva_pending"));
	pending->attributes[ID::init] = RTLIL::State::S0;

	RTLIL::SigBit active = sva_or(eval, pending, trigger);
	RTLIL::SigBit keep = sva_and(eval, active, sva_not(eval, completion));
	keep = sva_and(eval, keep, sva_not(eval, disable));

	ProceduralContext *procedural = eval.procedural;
	if (procedural == nullptr || procedural->timing.kind != ProcessTiming::EdgeTriggered ||
		procedural->timing.triggers.size() != 1) {
		eval.netlist.add_diag(diag::SVATemporalDelayRequiresClock, loc);
		return RTLIL::State::Sx;
	}
	auto &clock = procedural->timing.triggers[0];
	eval.netlist.add_dff(eval.netlist.new_id("sva_pending"),
						 clock.signal, keep, pending, clock.edge_polarity);
	return active;
}

static RTLIL::SigBit add_sticky_observation(
		EvalContext& eval, RTLIL::SigBit event, RTLIL::SigBit clear,
		slang::SourceLocation loc, std::string_view name) {
	auto *wire = eval.netlist.canvas->addWire(
		eval.netlist.new_id(std::string(name)));
	wire->attributes[ID::init] = RTLIL::State::S0;
	RTLIL::SigBit observed = sva_or(eval, wire, event);
	RTLIL::SigBit next =
		sva_and(eval, observed, sva_not(eval, clear));

	ProceduralContext *procedural = eval.procedural;
	if (procedural == nullptr ||
		procedural->timing.kind != ProcessTiming::EdgeTriggered ||
		procedural->timing.triggers.size() != 1) {
		eval.netlist.add_diag(diag::SVATemporalDelayRequiresClock, loc);
		return RTLIL::State::Sx;
	}
	auto& clock = procedural->timing.triggers[0];
	eval.netlist.add_dff(eval.netlist.new_id(std::string(name)),
						 clock.signal, next, wire, clock.edge_polarity);
	return observed;
}

static RTLIL::SigBit add_selected_trigger(
		PropertyResult& result, EvalContext& eval, RTLIL::SigBit trigger,
		RTLIL::SigBit disable, slang::SourceLocation loc) {
	trigger = sva_and(eval, trigger, sva_not(eval, disable));
	RTLIL::SigBit seen_trigger = add_sticky_observation(
		eval, trigger, RTLIL::State::S0, loc, "sva_select_seen");

	auto *choice = eval.netlist.canvas->addWire(
		eval.netlist.new_id("sva_select_choice"));
	auto *choice_cell = eval.netlist.canvas->addCell(
		eval.netlist.new_id("sva_select_choice"), ID($anyseq));
	choice_cell->setParam(ID::WIDTH, 1);
	choice_cell->setPort(ID::Y, choice);

	auto *chosen_wire = eval.netlist.canvas->addWire(
		eval.netlist.new_id("sva_select_chosen"));
	chosen_wire->attributes[ID::init] = RTLIL::State::S0;
	RTLIL::SigBit selected = sva_and(
		eval, choice,
		sva_and(eval, trigger, sva_not(eval, chosen_wire)));
	RTLIL::SigBit chosen = sva_or(eval, chosen_wire, selected);

	ProceduralContext *procedural = eval.procedural;
	if (procedural == nullptr ||
		procedural->timing.kind != ProcessTiming::EdgeTriggered ||
		procedural->timing.triggers.size() != 1) {
		eval.netlist.add_diag(diag::SVATemporalDelayRequiresClock, loc);
		result.valid = false;
		return RTLIL::State::Sx;
	}
	auto& clock = procedural->timing.triggers[0];
	eval.netlist.add_dff(eval.netlist.new_id("sva_select_chosen"),
						 clock.signal, chosen, chosen_wire,
						 clock.edge_polarity);

	// The fresh prophecy input selects exactly one enabled attempt.  This
	// fairness condition prevents it from avoiding every trigger.  Because
	// all possible selections remain in the formal state space, proving the
	// selected attempt proves every attempt without merging their histories.
	RTLIL::SigBit selection_fair =
		sva_or(eval, sva_not(eval, seen_trigger), chosen);
	result.obligations.push_back(
		{PropertyObligationKind::AuxiliaryFairness,
		 selection_fair, RTLIL::State::S1, RTLIL::State::S0});
	return selected;
}

static std::optional<RTLIL::SigBit> synthesize_selected_sequence(
		EvalContext& eval, const ast::AssertionExpr& sequence,
		RTLIL::SigBit trigger, RTLIL::SigBit disable);

static std::optional<RTLIL::SigBit> synthesize_selected_sequence(
		EvalContext& eval, const ast::AssertionExpr& sequence,
		RTLIL::SigBit trigger, RTLIL::SigBit disable) {
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
				return sva_or(eval, left.value(), right.value());
			}
		case ast::BinaryAssertionOperator::And:
			{
				auto left = synthesize_selected_sequence(
					eval, binary.left, trigger, disable);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				RTLIL::SigBit left_seen = add_sticky_observation(
					eval, left.value(), disable, expr_loc(binary.left),
					"sva_and_left");
				RTLIL::SigBit right_seen = add_sticky_observation(
					eval, right.value(), disable, expr_loc(binary.right),
					"sva_and_right");
				return sva_and(eval, left_seen, right_seen);
			}
		case ast::BinaryAssertionOperator::Intersect:
			{
				auto left = synthesize_selected_sequence(
					eval, binary.left, trigger, disable);
				auto right = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!left.has_value() || !right.has_value())
					return std::nullopt;
				return sva_and(eval, left.value(), right.value());
			}
		case ast::BinaryAssertionOperator::Within:
			{
				RTLIL::SigBit delayed_start = add_sticky_observation(
					eval, trigger, disable, expr_loc(binary.left),
					"sva_within_start");
				auto inner = synthesize_selected_sequence(
					eval, binary.left, delayed_start, disable);
				auto outer = synthesize_selected_sequence(
					eval, binary.right, trigger, disable);
				if (!inner.has_value() || !outer.has_value())
					return std::nullopt;
				RTLIL::SigBit inner_seen = add_sticky_observation(
					eval, inner.value(), disable, expr_loc(binary.left),
					"sva_within_seen");
				return sva_and(eval, inner_seen, outer.value());
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
				RTLIL::SigBit active = add_sticky_observation(
					eval, trigger, disable, expr_loc(binary.right),
					"sva_throughout_active");
				RTLIL::SigBit invalid = add_sticky_observation(
					eval,
					sva_and(eval, active,
						sva_not(eval, condition.value())),
					disable, expr_loc(binary.right),
					"sva_throughout_invalid");
				return sva_and(
					eval, body.value(),
					sva_and(eval, active, sva_not(eval, invalid)));
			}
		default:
			break;
		}
	}
	return synthesize_unbounded_sequence(
		eval, sequence, trigger, disable);
}

static PropertyResult lower_property(EvalContext& eval, const ast::AssertionExpr& expr,
									 RTLIL::SigBit trigger = RTLIL::State::S1,
									 RTLIL::SigBit disable = RTLIL::State::S0);

static PropertyResult lower_finite_property_from_trigger(
		EvalContext& eval, const ast::AssertionExpr& expr,
		RTLIL::SigBit trigger, RTLIL::SigBit disable) {
	PropertyResult result;
	auto paths = synthesizeAssertionExpr(eval, expr);
	if (paths.empty()) {
		// Preserve the established finite-sequence behavior for degenerate
		// empty matches: they produce a disabled check, which is important
		// both for vacuity and for label-based equivalence tests.
		add_safety_obligation(result, RTLIL::State::S0,
							  RTLIL::State::S0);
		return result;
	}
	auto match = collapse_or(paths);
	auto init_escape = delay_sva_sample(
		eval, RTLIL::State::S0, match.history,
		RTLIL::State::S1, match.loc);
	RTLIL::SigBit frame_ready =
		sva_not(eval, eval.netlist.ReduceBool(init_escape).as_bit());
	RTLIL::SigBit enable = delayed_abortable_trigger(
		eval, trigger, match.start, disable, match.loc);
	enable = sva_and(eval, enable,
		eval.netlist.ReduceBool(match.en).as_bit());
	enable = sva_and(eval, enable, frame_ready);
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
									 RTLIL::SigBit trigger, RTLIL::SigBit disable) {
	PropertyResult result;
	if (eventual.range.has_value() && eventual.range->max.has_value()) {
		return lower_finite_property_from_trigger(
			eval, eventual, trigger, disable);
	}

	uint32_t minimum = eventual.range.has_value() ? eventual.range->min : 0;
	if (assertion_has_unbounded_sequence(eventual.expr) &&
		!is_suffix_eventual_sequence(eventual.expr)) {
		RTLIL::SigBit selected = add_selected_trigger(
			result, eval, trigger, disable, expr_loc(eventual));
		if (!result.valid)
			return result;
		selected = delayed_abortable_trigger(
			eval, selected, minimum, disable, expr_loc(eventual));
		RTLIL::SigBit future_start = add_sticky_observation(
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
	if (!candidates.has_value() || candidates->empty()) {
		result.valid = false;
		return result;
	}
	add_response_liveness_obligation(
		result, eval, *candidates, trigger, disable, expr_loc(eventual));
	return result;
}

static PropertyResult lower_always(EvalContext& eval,
								   const ast::UnaryAssertionExpr& always_expr,
								   RTLIL::SigBit trigger,
								   RTLIL::SigBit disable) {
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
		RTLIL::SigBit enable = trigger;
		for (uint32_t offset = minimum; offset <= maximum; offset++) {
			if (offset != minimum)
				enable = delayed_abortable_trigger(
					eval, enable, 1, disable, expr_loc(always_expr));
			add_safety_obligation(result, condition.value(),
				sva_and(eval, enable, sva_not(eval, disable)));
		}
		return result;
	}

	RTLIL::SigBit pending = add_pending_monitor(eval, trigger,
											   RTLIL::State::S0,
											   disable,
											   expr_loc(always_expr));
	add_safety_obligation(result, condition.value(),
		sva_and(eval, pending, sva_not(eval, disable)));
	return result;
}

static PropertyResult lower_until(EvalContext& eval,
								  const ast::BinaryAssertionExpr& until_expr,
								  RTLIL::SigBit trigger, RTLIL::SigBit disable) {
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

	RTLIL::SigBit pending = add_pending_monitor(eval, trigger, right.value(),
											   disable, expr_loc(until_expr));
	RTLIL::SigBit must_hold = including_completion
		? pending
		: sva_and(eval, pending, sva_not(eval, right.value()));
	must_hold = sva_and(eval, must_hold, sva_not(eval, disable));
	add_safety_obligation(result, left.value(), must_hold);
	if (strong)
		add_liveness_obligation(result, eval, right.value(), trigger, disable);
	return result;
}

static PropertyResult lower_property(EvalContext& eval, const ast::AssertionExpr& expr,
									 RTLIL::SigBit trigger, RTLIL::SigBit disable) {
	if (expr.kind == ast::AssertionExprKind::DisableIff) {
		const auto& disabled = expr.as<ast::DisableIffAssertionExpr>();
		RTLIL::SigBit condition =
			eval.netlist.ReduceBool(eval(disabled.condition)).as_bit();
		return lower_property(eval, disabled.expr, trigger,
							  sva_or(eval, disable, condition));
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
				RTLIL::SigBit start = delayed_abortable_trigger(
					eval, trigger, minimum, disable, expr_loc(expr));
				RTLIL::SigBit pending = add_pending_monitor(
					eval, start, RTLIL::State::S0, disable, expr_loc(expr));
				PropertyResult result;
				add_safety_obligation(result, condition.value(),
					sva_and(eval, pending, sva_not(eval, disable)));
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
					RTLIL::SigBit seen = RTLIL::State::S0;
					for (uint32_t offset = minimum;
						 offset <= maximum; offset++) {
						RTLIL::SigBit sample = delay_sva_sample(
							eval, violation.value(), maximum - offset,
							RTLIL::State::S0, expr_loc(expr),
							"sva_not_always").as_bit();
						seen = sva_or(eval, seen, sample);
					}
					RTLIL::SigBit enable =
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
					if (!candidates.has_value() || candidates->empty())
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
					if (!candidates.has_value() || candidates->empty())
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
			RTLIL::SigBit selected = add_selected_trigger(
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
		if (!candidates.has_value() || candidates->empty())
			return {{}, false};
		PropertyResult result;
		add_response_liveness_obligation(
			result, eval, *candidates, trigger, disable, expr_loc(expr));
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Abort) {
		const auto& abort = expr.as<ast::AbortAssertionExpr>();
		RTLIL::SigBit condition =
			eval.netlist.ReduceBool(eval(abort.condition)).as_bit();
		if (abort.action == ast::AbortAssertionExpr::Accept)
			return lower_property(eval, abort.expr, trigger,
								  sva_or(eval, disable, condition));

		PropertyResult result =
			lower_property(eval, abort.expr, trigger, disable);
		if (!result.valid)
			return result;
		bool has_liveness = false;
		std::vector<RTLIL::SigBit> active_obligations;
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
				eval.netlist.ReduceBool(obligation.active).as_bit());
		}
		for (RTLIL::SigBit active : active_obligations) {
			add_safety_obligation(
				result, sva_not(eval, condition),
				sva_and(eval, active, sva_not(eval, disable)));
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
		RTLIL::SigBit condition =
			eval.netlist.ReduceBool(eval(conditional.condition)).as_bit();
		PropertyResult result = lower_property(
			eval, conditional.ifExpr,
			sva_and(eval, trigger, condition), disable);
		if (conditional.elseExpr) {
			PropertyResult else_result = lower_property(
				eval, *conditional.elseExpr,
				sva_and(eval, trigger, sva_not(eval, condition)), disable);
			result.valid = result.valid && else_result.valid;
			result.obligations.insert(result.obligations.end(),
									  else_result.obligations.begin(),
									  else_result.obligations.end());
		}
		return result;
	}

	if (expr.kind == ast::AssertionExprKind::Case) {
		const auto& case_expr = expr.as<ast::CaseAssertionExpr>();
		RTLIL::SigSpec control = eval(case_expr.expr);
		RTLIL::SigBit matched = RTLIL::State::S0;
		PropertyResult result;
		for (const auto& item : case_expr.items) {
			RTLIL::SigBit item_hit = RTLIL::State::S0;
			for (const auto *item_expr : item.expressions)
				item_hit = sva_or(
					eval, item_hit,
					inside_comparison(eval, control, *item_expr));
			item_hit = sva_and(eval, item_hit, sva_not(eval, matched));
			PropertyResult branch = lower_property(
				eval, *item.body,
				sva_and(eval, trigger, item_hit), disable);
			result.valid = result.valid && branch.valid;
			result.obligations.insert(result.obligations.end(),
									 branch.obligations.begin(),
									 branch.obligations.end());
			matched = sva_or(eval, matched, item_hit);
		}
		if (case_expr.defaultCase) {
			PropertyResult branch = lower_property(
				eval, *case_expr.defaultCase,
				sva_and(eval, trigger, sva_not(eval, matched)), disable);
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
				RTLIL::SigBit consequent_trigger = sva_and(
					eval, trigger, antecedent.value());
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
					sva_and(eval, trigger, sva_not(eval, disable)));
				return result;
			}

		case ast::BinaryAssertionOperator::OverlappedImplication:
		case ast::BinaryAssertionOperator::NonOverlappedImplication:
			{
				bool temporal_consequent =
					property_requires_temporal_obligations(binary.right);
				if (!temporal_consequent &&
					!assertion_has_unbounded_sequence(binary.left))
					break;
				bool nonoverlapped =
					binary.op == ast::BinaryAssertionOperator::NonOverlappedImplication;
				auto antecedent = antecedent_match_event(
					eval, binary.left, trigger, nonoverlapped, disable);
				if (!antecedent.has_value())
					return {{}, false};
				RTLIL::SigBit consequent_trigger = antecedent.value();
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
					if (!candidates.has_value() || candidates->empty())
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
					trigger != RTLIL::State::S1) {
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
					sva_or(eval,
						eval.netlist.ReduceBool(left.obligations[0].a).as_bit(),
						eval.netlist.ReduceBool(right.obligations[0].a).as_bit()),
					eval.netlist.ReduceBool(left.obligations[0].en).as_bit(),
					RTLIL::State::S0);
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
		if (!timing_matches_process(timing, procedural.timing)) {
			netlist.add_diag(diag::UnsupportedSVAFeature, expr_loc(*expr));
			return;
		}
		expr = &clocking_expr->expr;
	}

	PropertyResult result = lower_property(procedural.eval, *expr);
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
	if (statement.assertionKind == ast::AssertionKind::CoverProperty) {
		for (const auto& obligation : result.obligations) {
			if (obligation.kind != PropertyObligationKind::Safety) {
				netlist.add_diag(diag::SVALivenessCoverUnsupported,
								 statement.sourceRange);
				return;
			}
		}
	}

	RTLIL::IdString base_cell_name;

	if (block && unwrap_statement(block->tryGetStatement()) == &statement && !block->name.empty()) {
		// If we are the sole statement in a block, use the block's label
		base_cell_name = netlist.id(*block);
	} else {
		base_cell_name = netlist.new_id();
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
				netlist.add_diag(diag::SVALivenessCoverUnsupported,
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

		RTLIL::IdString cell_name =
			index == 0 ? base_cell_name : netlist.new_id("sva_obligation");
		RTLIL::SigSpec a = netlist.ReduceBool(obligation.a);
		RTLIL::SigSpec en = netlist.ReduceBool(obligation.en);

		auto cell = netlist.canvas->addCell(cell_name, ID($check));
		procedural.set_effects_trigger(cell);
		cell->setPort(ID::EN, netlist.LogicAnd(cell->getPort(ID::EN), en));
		cell->setParam(ID::FLAVOR, flavor);
		cell->setParam(ID::FORMAT, std::string(""));
		cell->setParam(ID::ARGS_WIDTH, 0);
		cell->setParam(ID::PRIORITY, --procedural.effects_priority);
		cell->setPort(ID::ARGS, {});
		cell->setPort(ID::A, a);
		transfer_attrs<const ast::Statement>(netlist, statement, cell);
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
	case ast::EdgeKind::NegEdge:
		timing.triggers.push_back(ProcessTiming::Sensitivity {
			.signal = netlist.eval(signal_event.expr),
			.edge_polarity = (signal_event.edge == ast::EdgeKind::PosEdge),
			.ast_node = &clocking
		});
		break;

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

	RTLIL::Process *rtlil_proc = netlist.canvas->addProcess(netlist.new_id());
	transfer_attrs<const ast::Statement>(netlist, statement, rtlil_proc);
	procedure.copy_case_tree_into(rtlil_proc->root_case);
}

void process_freestanding_sva_property(NetlistContext &netlist,
									   const ast::ConcurrentAssertionStatement &statement,
						  			   const ast::StatementBlockSymbol *block,
									   const ast::Scope *scope)
{
	const ast::AssertionExpr &spec = statement.propertySpec;

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

	RTLIL::Process *rtlil_proc = netlist.canvas->addProcess(netlist.new_id());
	transfer_attrs<const ast::Statement>(netlist, statement, rtlil_proc);
	procedure.copy_case_tree_into(rtlil_proc->root_case);
}

};
