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
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/yosys-slang-live.XXXXXXXX")
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
)
