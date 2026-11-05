#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 PLUGIN SUPROVE" >&2
	exit 2
fi

plugin=$1
suprove=$2
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
suprove_driver=$(cd -- "${test_dir}/../../tools" && pwd)/suprove_all_justice.py
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/sv-elab-live.XXXXXXXX")
cleanup() {
	rm -rf -- "${work_dir:?}"
}
trap cleanup EXIT

cp -- "${test_dir}/response.sby" "${work_dir}/response.sby"
cp -- "${test_dir}/raw_live.sby" "${work_dir}/raw_live.sby"
cp -- "${test_dir}/raw_live_pass.il" "${work_dir}/raw_live_pass.il"
cp -- "${test_dir}/raw_live_fail.il" "${work_dir}/raw_live_fail.il"
cp -- "${test_dir}/semantics.sby" "${work_dir}/semantics.sby"
cp -- "${test_dir}/reachability.sby" "${work_dir}/reachability.sby"
cp -- "${test_dir}/until_safety.sby" "${work_dir}/until_safety.sby"
cp -- "${test_dir}/regular.sby" "${work_dir}/regular.sby"
cp -- "${test_dir}/regular_reachability.sby" \
	"${work_dir}/regular_reachability.sby"
cp -- "${test_dir}/reject_safety.sby" "${work_dir}/reject_safety.sby"
cp -- "${test_dir}/procedural.sby" "${work_dir}/procedural.sby"
cp -- "${test_dir}/clock_stop.sby" "${work_dir}/clock_stop.sby"
cp -- "${test_dir}/sequence.sby" "${work_dir}/sequence.sby"
cp -- "${test_dir}/sequence_safety.sby" "${work_dir}/sequence_safety.sby"
cp -- "${test_dir}/abort.sby" "${work_dir}/abort.sby"
cp -- "${test_dir}/abort_safety.sby" "${work_dir}/abort_safety.sby"
cp -- "${test_dir}/constants.sby" "${work_dir}/constants.sby"
cp -- "${test_dir}/composition.sby" "${work_dir}/composition.sby"
cp -- "${test_dir}/composition_safety.sby" \
	"${work_dir}/composition_safety.sby"
cp -- "${test_dir}/justice.sby" "${work_dir}/justice.sby"
cp -- "${test_dir}/operators.sby" "${work_dir}/operators.sby"

yosys -q -m "${plugin}" -p "
	read_slang ${test_dir}/response_proof.sv;
	prep -top response_proof;
	async2sync;
	formalff -clk2ff;
	chformal -lower;
	opt_clean;
	write_rtlil ${work_dir}/response_pass.il
"

for top in \
	disable_abort_pass \
	disable_abort_fail \
	disable_or_pass \
	disable_or_fail \
	minimum_delay_pass \
	minimum_delay_fail \
	fairness_pass \
	fairness_fail \
	suffix_overlap_pass \
	suffix_pretrigger_fail \
	overlapping_requests_pass \
	reject_abort_pass \
	reject_abort_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/semantics_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	regular_sequence_pass \
	regular_sequence_fail \
	regular_intersect_pass \
	regular_intersect_fail \
	regular_goto_pass \
	regular_goto_fail \
	regular_within_pass \
	regular_within_fail \
	regular_and_pass \
	regular_and_fail \
	regular_or_pass \
	regular_or_fail \
	regular_throughout_pass \
	regular_throughout_fail \
	regular_nonconsecutive_pass \
	regular_nonconsecutive_fail \
	regular_consecutive_pass \
	regular_consecutive_fail \
	regular_empty_fusion_pass \
	regular_empty_fusion_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/regular_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in until_pass until_fail until_with_pass until_with_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/until_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	guard_response_pass \
	guard_response_fail \
	guard_selected_pass \
	guard_selected_fail \
	guard_or_pass \
	guard_or_fail \
	guard_or_mixed_pass \
	guard_or_mixed_fail \
	guard_assume_pass \
	guard_assume_fail \
	sampled_if_pass \
	sampled_if_fail \
	comb_default_pass \
	comb_default_fail \
	iff_zero_pass \
	iff_zero_fail \
	iff_minimum_pass \
	iff_minimum_fail \
	iff_response_pass \
	iff_response_fail \
	iff_fair_pass \
	iff_fair_fail \
	guard_always_pass \
	guard_always_fail \
	guard_ranged_always_pass \
	guard_ranged_always_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/procedural_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	stop_minimum_fail \
	stop_minimum_pass \
	stop_minimum_single_fail \
	stop_minimum_late_fail \
	stop_minimum_ticking_fail \
	stop_sequence_fail \
	stop_sequence_pass \
	stop_or_fail \
	stop_or_pass \
	stop_not_always_fail \
	stop_not_always_pass \
	stop_strong_fail \
	stop_strong_pass \
	stop_top_fail \
	stop_zero_fail \
	stop_zero_pass \
	pause_pass \
	stop_assume_pass \
	stop_assume_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/clock_stop_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	named_strong_pass \
	named_strong_fail \
	named_eventually_pass \
	named_eventually_fail \
	zero_prefix_pass \
	zero_prefix_fail \
	nexttime_empty_pass \
	nexttime_empty_fail \
	empty_eventually_pass \
	empty_eventually_fail \
	empty_followed_pass \
	empty_followed_fail \
	empty_overlapped_fail \
	suffix_empty_pass \
	suffix_empty_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/sequence_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	selected_settled_fail \
	selected_disabled_pass \
	selected_accept_pass \
	selected_reject_fail \
	selected_minimum_fail \
	selected_minimum_pass \
	reject_vacuous_fail \
	reject_vacuous_pass \
	reject_accept_fail \
	accept_reject_pass \
	reject_next_fail \
	reject_next_pass \
	antecedent_settled_fail \
	antecedent_disabled_pass \
	followed_settled_fail \
	followed_disabled_pass \
	not_always_pass \
	not_always_fail \
	strong_settled_fail \
	strong_disabled_pass
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/abort_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	constants_enum_pass \
	constants_enum_fail \
	constants_defparam_pass \
	constants_defparam_fail \
	constants_hierarchical_pass \
	constants_hierarchical_fail \
	constants_genvar_pass \
	constants_genvar_fail \
	constants_function_pass \
	constants_function_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/constants_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	strong_assumed_pass \
	strong_assumed_fail \
	strong_next_pass \
	strong_next_fail \
	weak_bounded_fail \
	strong_bounded_pass \
	and_intersect_pass \
	and_intersect_fail \
	and_within_pass \
	and_within_fail \
	first_match_intersect_pass \
	first_match_intersect_fail \
	first_match_within_pass \
	first_match_within_fail \
	and_empty_pass \
	and_empty_fail \
	within_empty_pass \
	within_empty_fail \
	within_throughout_pass \
	within_throughout_fail \
	first_match_attempts_fail \
	intersect_attempts_pass \
	eventual_throughout_pass \
	eventual_throughout_fail \
	and_empty_finite_pass \
	and_empty_finite_fail \
	and_finite_fail \
	followed_one_end_pass \
	followed_one_end_fail \
	followed_no_match_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/composition_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	justice_fair_pass \
	justice_first_fail \
	justice_second_fail \
	justice_selected_pass \
	justice_selected_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/justice_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

for top in \
	until_progress_fail \
	until_with_progress_fail \
	implies_live_pass \
	implies_live_fail \
	if_live_pass \
	if_live_fail \
	case_live_pass \
	case_live_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/operators_proof.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${top}.il
	"
done

yosys -q -m "${plugin}" -p "
	read_slang -DMUTATE_LIVENESS ${test_dir}/response_proof.sv;
	prep -top response_proof;
	async2sync;
	formalff -clk2ff;
	chformal -lower;
	opt_clean;
	write_rtlil ${work_dir}/response_fail.il
"

(
	cd -- "${work_dir}"
	export YOSYS_SLANG_REAL_SUPROVE="${suprove}"
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/raw_live" \
		-f raw_live.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/response" \
		-f response.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/semantics" \
		-f semantics.sby
	sby --prefix "${work_dir}/reachability" -f reachability.sby
	sby --prefix "${work_dir}/until_safety" -f until_safety.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/regular" \
		-f regular.sby
	sby --prefix "${work_dir}/regular_reachability" \
		-f regular_reachability.sby
	sby --prefix "${work_dir}/reject_safety" -f reject_safety.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/procedural" \
		-f procedural.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/clock_stop" \
		-f clock_stop.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/sequence" \
		-f sequence.sby
	sby --prefix "${work_dir}/sequence_safety" -f sequence_safety.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/abort" \
		-f abort.sby
	sby --prefix "${work_dir}/abort_safety" -f abort_safety.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/constants" \
		-f constants.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/composition" \
		-f composition.sby
	sby --prefix "${work_dir}/composition_safety" \
		-f composition_safety.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/justice" \
		-f justice.sby
	sby --suprove "${suprove_driver}" --prefix "${work_dir}/operators" \
		-f operators.sby
)
