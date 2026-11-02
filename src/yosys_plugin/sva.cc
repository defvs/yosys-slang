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

static constexpr uint32_t SVA_ENUMERATION_LIMIT = 1024;

// Ranges within the limit above still multiply when nested or put in a row, so these
// limits go by an assertion as a whole: the first by the ticks it spans, the second by
// the delay registers and logic cells it lowers to. Those take about 1.4 KiB each,
// and `a ##[0:1024] b` gets through a third of the limit.
static constexpr int SVA_LENGTH_LIMIT = 1 << 16;
static constexpr uint64_t SVA_COST_LIMIT = 3000000;

// Lowering can have a long way to go past the point we know it's in vain, so we
// leave it altogether on hitting a limit
struct SvaLimitExceeded {};

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

	void add_cost(int cost) const {
		eval.sva_lowering_cost += cost;
		if (eval.sva_lowering_cost > SVA_COST_LIMIT)
			throw SvaLimitExceeded {};
	}

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
		if (start + time > SVA_LENGTH_LIMIT)
			throw SvaLimitExceeded {};
		add_cost(time);

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
		add_cost(1);

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
		add_cost(1);

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
		add_cost(1);

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

	int max_start = 0;
	for (auto path : paths) {
		assert_invariant(!path.empty);
		max_start = std::max(max_start, path.start);
	}

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

static bool check_finite_range(EvalContext& eval, const ast::AssertionExpr& expr,
							   uint32_t min, uint32_t max) {
	if (max < min || max > SVA_ENUMERATION_LIMIT) {
		eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
		return false;
	}
	return true;
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

static std::vector<AssertionMatch> repeat_count(const AttemptContext& ctx, OperandPaths& operand, int count) {
	if (count == 0)
		return {};

	std::vector<AssertionMatch> result = operand.at(0);
	for (int i = 1; i < count; i++)
		result = seq_vec(ctx, result, 1, 1, operand);
	return result;
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
	if (!check_finite_range(eval, expr, repetition->range.min, repetition->range.max.value()))
		return {};

	std::vector<AssertionMatch> repeated;
	for (uint32_t count = repetition->range.min; count <= repetition->range.max.value(); count++) {
		if (count == 0)
			repeated.push_back(AssertionMatch::Empty(eval, expr_loc(expr)));
		else {
			auto count_paths = repeat_count(ctx, operand, (int)count);
			repeated.insert(repeated.end(), count_paths.begin(), count_paths.end());
		}
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
					if (!check_finite_range(eval, expr, delay.min, delay.max.value()))
						return {};

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
					if (!check_finite_range(eval, expr, uop.range->min, uop.range->max.value()))
						return {};

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
						if (!check_finite_range(eval, expr, uop.range->min, uop.range->max.value()))
							return {};
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
				auto left_ctx = implication ? ctx.negated() : ctx;

				OperandPaths left_at(left_ctx, [&](const AttemptContext& at) {
					return synthesize_assertion_expr(eval, at, biop.left);
				});
				// `R |-> P` is `not (R ##0 not P)`, and `R |=> P` the same with a `##1`
				OperandPaths right_at(ctx, [&](const AttemptContext& at) {
					auto paths = synthesize_assertion_expr(eval, at, biop.right);
					return implication ? not_vec(eval, at, std::move(paths), expr_loc(expr)) : paths;
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
			// eval_assertion() handles the one a property is allowed, at the top of it.
			// slang only rejects the others in direct instances of a property.
			eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
			return {};

		case slang::ast::AssertionExprKind::SequenceWithMatch:
			{
				const auto& with_match = expr.as<ast::SequenceWithMatchExpr>();
				if (!with_match.matchItems.empty()) {
					eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
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
					eval.netlist.add_diag(diag::AssertionUnsupported, expr_loc(expr));
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

// `attempt` is the condition of the procedural code around the assertion. It's read
// when the statement is reached and decides if an attempt is queued, the attempts
// in flight run to completion regardless (IEEE Std 1800-2017 section 16.14.6).
static AssertionResult eval_assertion(EvalContext& eval, const ast::AssertionExpr& assertion,
									  ir::Net attempt, bool cover) {
	eval.sva_lowering_cost = 0;

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

	AssertionResult result;
	try {
		result = eval_assertion(procedural.eval, *expr, attempt,
								statement.assertionKind == ast::AssertionKind::CoverProperty);
	} catch (const SvaLimitExceeded &) {
		netlist.add_diag(diag::AssertionUnsupported, expr_loc(*expr));
		return;
	}

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
