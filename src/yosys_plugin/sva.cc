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

static ir::Value delay_sva_sample(EvalContext& eval, ir::Value sig, int cycles, ir::Trit init,
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

	auto &trigger = procedural->timing.triggers[0];
	for (int i = 0; i < cycles; i++) {
		ir::Value next = eval.netlist.add_placeholder_signal(sig.size(), name_hint);
		eval.netlist.set_initialization(next, ir::Const(init, sig.size()));
		eval.netlist.add_dffe(eval.netlist.backend->new_id(std::string(name_hint)),
							  trigger.signal, procedural->timing.background_enable, sig, next,
							  trigger.edge_polarity, true);
		sig = next;
	}
	return sig;
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
	auto disable = AssertionMatch(eval, eval.sva(condition), loc);
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

				case ast::BinaryAssertionOperator::Within:
				case ast::BinaryAssertionOperator::Iff:
				case ast::BinaryAssertionOperator::Until:
				case ast::BinaryAssertionOperator::SUntil:
				case ast::BinaryAssertionOperator::UntilWith:
				case ast::BinaryAssertionOperator::SUntilWith:
				case ast::BinaryAssertionOperator::Implies:
				case ast::BinaryAssertionOperator::OverlappedFollowedBy:
				case ast::BinaryAssertionOperator::NonOverlappedFollowedBy:
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
					eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
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
					eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
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

struct AssertionResult {
	ir::Value a;
	ir::Value en;
};

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

static AssertionResult eval_assertion(EvalContext& eval, const ast::AssertionExpr& assertion) {
	auto paths = synthesize_assertion_expr(eval, assertion);

	const ast::AssertionExpr &top = unwrap_property_instances(assertion);
	if (!ast::DisableIffAssertionExpr::isKind(top.kind) && eval.netlist.process_scope) {
		auto scope = eval.netlist.process_scope;
		if (auto condition = scope->getCompilation().getDefaultDisable(*scope))
			paths = synthesize_disable_iff(eval, *condition, std::move(paths), expr_loc(assertion));
	}

	// With no possible match the property fails on every attempt
	if (paths.empty()) return { ir::S0, ir::S1 };

	auto sig = collapse_or(paths);

	auto init_escape = delay_sva_sample(eval, ir::S0, sig.start, ir::S1, sig.loc);
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

	AssertionResult result = eval_assertion(procedural.eval, *expr);

	std::string flavor;
	switch (statement.assertionKind) {
	case ast::AssertionKind::Assert:        flavor = "assert"; break;
	case ast::AssertionKind::Assume:        flavor = "assume"; break;
	case ast::AssertionKind::CoverProperty: flavor = "cover"; break;
	default:                                netlist.add_diag(diag::AssertionUnsupported, statement.sourceRange); return;
	}

	std::string cell_name;

	if (block && unwrap_statement(block->tryGetStatement()) == &statement && !block->name.empty()) {
		// If we are the sole statement in a block, use the block's label
		cell_name = netlist.id(*block);
	} else {
		cell_name = netlist.backend->new_id();
	}

	ir::Net a = netlist.ReduceBool(result.a);
	ir::Net en = netlist.ReduceBool(result.en);

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
