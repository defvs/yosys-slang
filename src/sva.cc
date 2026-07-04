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
#include "kernel/rtlil.h"
#include "slang/ast/expressions/AssertionExpr.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/text/SourceLocation.h"

#include "slang_frontend.h"
#include "statements.h"
#include "diag.h"

namespace slang_frontend {

// This portion was written by Louis-Emile Ploix "mndstrmr" (c) 2025; ISC licence
// Brought into Slang head by Mel Young 2026, no additional work

static slang::SourceLocation expr_loc(const ast::AssertionExpr& expr) {
	return expr.syntax ? expr.syntax->sourceRange().start() : slang::SourceLocation::NoLocation;
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
		next->attributes[ID::init] = init;
		eval.netlist.add_dff(eval.netlist.new_id(std::string(name_hint)),
							 trigger.signal, sig, next, trigger.edge_polarity);
		sig = next;
	}
	return sig;
}

struct AssertionMatch {
	EvalContext& eval;
	RTLIL::SigSpec sig;
	RTLIL::SigSpec en;
	int start;
	slang::SourceLocation loc;

	AssertionMatch(EvalContext& eval_, RTLIL::SigSpec sig_,
				   slang::SourceLocation loc_ = slang::SourceLocation::NoLocation):
		eval(eval_), sig(sig_), en(true), start(0), loc(loc_) {}

private:
	AssertionMatch(EvalContext& eval_, RTLIL::SigSpec sig_, RTLIL::SigSpec en_, int start_,
				   slang::SourceLocation loc_):
		eval(eval_), sig(sig_), en(en_), start(start_), loc(loc_) {}

public:
	void operator=(AssertionMatch other) {
		sig = other.sig;
		en = other.en;
		start = other.start;
		loc = other.loc;
	}

	AssertionMatch shift(int time) const {
		RTLIL::SigSpec shifted_sig = sig;
		RTLIL::SigSpec shifted_en = en;
		if (!sig.is_fully_const())
			shifted_sig = delay_sva_sample(eval, sig, time, RTLIL::State::Sx, loc);
		if (!en.is_fully_const())
			shifted_en = delay_sva_sample(eval, en, time, RTLIL::State::S0, loc);
		return { eval, shifted_sig, shifted_en, start + time, loc };
	}

	AssertionMatch operator||(AssertionMatch& other) const {
		RTLIL::SigSpec gated = en.is_fully_const() && en.as_bool() ? sig : eval.netlist.LogicAnd(en, sig);
		RTLIL::SigSpec other_gated = other.en.is_fully_const() && other.en.as_bool()
										  ? other.sig
										  : eval.netlist.LogicAnd(other.en, other.sig);
		RTLIL::SigSpec result_sig = eval.netlist.LogicOr(gated, other_gated);
		RTLIL::SigSpec result_en = eval.netlist.LogicOr(en, other.en);
		return { eval, result_sig, result_en, std::max(other.start, start), loc };
	}

	AssertionMatch operator&&(AssertionMatch& other) const {
		RTLIL::SigSpec result_en = eval.netlist.LogicAnd(en, other.en);
		if (sig.is_fully_const() && sig.as_bool())
			return { eval, other.sig, result_en, std::max(other.start, start), loc };
		if (sig.is_fully_const() && !sig.as_bool())
			return { eval, false, result_en, std::max(other.start, start), loc };
		if (other.sig.is_fully_const() && other.sig.as_bool())
			return { eval, sig, result_en, std::max(other.start, start), loc };
		if (other.sig.is_fully_const() && !other.sig.as_bool())
			return { eval, false, result_en, std::max(other.start, start), loc };
		return { eval, eval.netlist.LogicAnd(sig, other.sig), eval.netlist.LogicAnd(en, other.en),
				 std::max(other.start, start), loc };
	}

	AssertionMatch operator!() const {
		if (sig.is_fully_const()) return { eval, !sig.as_bool(), en, start, loc };
		return { eval, eval.netlist.LogicNot(sig), en, start, loc };
	}
};

static std::vector<AssertionMatch> compress_paths(std::vector<AssertionMatch> paths) {
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

	return grouped;
}

static AssertionMatch collapse_or(std::vector<AssertionMatch> paths) {
	log_assert(!paths.empty());

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

static std::vector<AssertionMatch> seq_vec(std::vector<AssertionMatch> a, int min, int max, std::vector<AssertionMatch> b) {
	std::vector<AssertionMatch> new_own_paths;
	for (auto path : a) {
		for (int offset = min; offset <= max; offset++) {
			for (auto inner : b)
				new_own_paths.push_back(path.shift(offset + inner.start) && inner);
		}
	}
	return compress_paths(new_own_paths);
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

	std::vector<AssertionMatch> repeated;
	for (uint32_t count = repetition->range.min; count <= repetition->range.max.value(); count++) {
		if (count == 0)
			repeated.push_back({eval, true, expr_loc(expr)});
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
				std::vector<AssertionMatch> paths = {{ eval, simple.isNullExpr ? false : eval.sva(simple.expr),
													   expr_loc(expr) }};
				if (!apply_repetition(eval, expr, simple.repetition, paths))
					return {};
				return paths;
			}
		case slang::ast::AssertionExprKind::SequenceConcat:
			{
				const auto& sequence = expr.as<ast::SequenceConcatExpr>();
				std::vector<AssertionMatch> own_paths;
				own_paths.push_back({eval, true, expr_loc(expr)});
				for (int i = 0; i < sequence.elements.size(); i++) {
					auto inner_paths = synthesizeAssertionExpr(eval, *sequence.elements[i].sequence);

					auto delay = sequence.elements[i].delay;
					if (!delay.max.has_value()) {
						eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
						return {};
					}

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

				case ast::BinaryAssertionOperator::Within:
				case ast::BinaryAssertionOperator::Iff:
				case ast::BinaryAssertionOperator::Until:
				case ast::BinaryAssertionOperator::SUntil:
				case ast::BinaryAssertionOperator::UntilWith:
				case ast::BinaryAssertionOperator::SUntilWith:
				case ast::BinaryAssertionOperator::Implies:
				case ast::BinaryAssertionOperator::OverlappedFollowedBy:
				case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
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
				std::vector<AssertionMatch> disables;
				disables.push_back(disable);
				for (int i = 0; i < inner.size(); i++) {
					std::optional<AssertionMatch> this_disables = {};
					for (int t = 0; t <= inner[i].start; t++) {
						while (disables.size() <= t)
							disables.push_back(disables[disables.size() - 1].shift(1));

						if (this_disables.has_value())
							this_disables = this_disables.value() || disables[t];
						else
							this_disables = disables[t];
					}
					if (this_disables.has_value()) {
						auto disable_window = this_disables.value();
						auto not_disabled = !disable_window;
						inner[i].en = eval.netlist.LogicAnd(inner[i].en, not_disabled.sig);
					}
				}
				return inner;
			}

		case slang::ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
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
					eval.netlist.add_diag(diag::AssertionUnsupported, expr.syntax->sourceRange().start());
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

AssertionResult evalAssertion(EvalContext& eval, const ast::AssertionExpr& assertion) {
	auto paths = synthesizeAssertionExpr(eval, assertion);
	if (paths.empty()) return { false, false }; // Ran into an error

	auto sig = collapse_or(paths);

	auto init_escape = delay_sva_sample(eval, false, sig.start, true, sig.loc);
	auto frame_ready = eval.netlist.LogicNot(init_escape);
	return { sig.sig, eval.netlist.LogicAnd(sig.en, frame_ready) };
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

	AssertionResult result = evalAssertion(procedural.eval, *expr);

	std::string flavor;
	switch (statement.assertionKind) {
	case ast::AssertionKind::Assert:        flavor = "assert"; break;
	case ast::AssertionKind::Assume:        flavor = "assume"; break;
	case ast::AssertionKind::CoverProperty: flavor = "cover"; break;
	default:                                netlist.add_diag(diag::AssertionUnsupported, statement.sourceRange); return;
	}

	RTLIL::IdString cell_name;

	if (block && unwrap_statement(block->tryGetStatement()) == &statement && !block->name.empty()) {
		// If we are the sole statement in a block, use the block's label
		cell_name = netlist.id(*block);
	} else {
		cell_name = netlist.new_id();
	}

	RTLIL::SigSpec a = netlist.ReduceBool(result.a);
	RTLIL::SigSpec en = netlist.ReduceBool(result.en);

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

void process_freestanding_sva_property(NetlistContext &netlist,
									   const ast::ConcurrentAssertionStatement &statement,
						  			   const ast::StatementBlockSymbol *block)
{
	const ast::AssertionExpr &spec = statement.propertySpec;

	if (ast::ClockingAssertionExpr::isKind(spec.kind)) {
		// Need to strip clocking
		const auto &clocking_expr = spec.as<ast::ClockingAssertionExpr>();
		const auto &clocking = clocking_expr.clocking;

		if (!ast::SignalEventControl::isKind(clocking.kind)) {
			netlist.add_diag(diag::UnsupportedSVAFeature, clocking.sourceRange);
			return;
		}

		const auto &signal_event = clocking.as<ast::SignalEventControl>();

		ProcessTiming timing(ProcessTiming::EdgeTriggered);
		switch (signal_event.edge) {
		case ast::EdgeKind::None:
			netlist.add_diag(diag::SVAClockingRequiresEdge, signal_event.sourceRange);
			return;

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
			return;
		}

		if (signal_event.iffCondition) {
			// TODO
			netlist.add_diag(diag::IffUnsupported, signal_event.iffCondition->sourceRange);
		}

		ProceduralContext procedure(netlist, timing);
		process_sva_property(statement, block, procedure, clocking_expr.expr);

		RTLIL::Process *rtlil_proc = netlist.canvas->addProcess(netlist.new_id());
		transfer_attrs<const ast::Statement>(netlist, statement, rtlil_proc);
		procedure.copy_case_tree_into(rtlil_proc->root_case);
	} else {
		// No clocking
		ProceduralContext procedure(netlist, ProcessTiming::implicit);
		process_sva_property(statement, block, procedure, spec);

		RTLIL::Process *rtlil_proc = netlist.canvas->addProcess(netlist.new_id());
		transfer_attrs<const ast::Statement>(netlist, statement, rtlil_proc);
		procedure.copy_case_tree_into(rtlil_proc->root_case);
	}
}

};
