#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
	echo "usage: $0 PLUGIN SUPROVE" >&2
	exit 2
fi

plugin=$1
suprove=$2
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "${test_dir}/../.." && pwd)
ot="${root}/tests/third_party/opentitan"
prim_rtl="${ot}/hw/ip/prim/rtl"
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/yosys-slang-ot-arbiter.XXXXXXXX")
cleanup() {
	rm -rf -- "${work_dir:?}"
}
trap cleanup EXIT

cp -- "${test_dir}/arbiter_live.sby" "${work_dir}/arbiter_live.sby"
cp -- "${test_dir}/arbiter_safety.sby" "${work_dir}/arbiter_safety.sby"
cp -- "${test_dir}/arbiter_reach.sby" "${work_dir}/arbiter_reach.sby"

build_variant() {
	local define=$1
	local output=$2
	yosys -q -m "${plugin}" -p "
		read_slang --single-unit ${define} \
			-I${prim_rtl} \
			${prim_rtl}/prim_util_pkg.sv \
			${prim_rtl}/prim_leading_one_ppc.sv \
			${prim_rtl}/prim_arbiter_ppc.sv \
			${test_dir}/arbiter_harness.sv;
		prep -top opentitan_arbiter_liveness_harness;
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${work_dir}/${output}
	"
}

build_variant "" arbiter_pass.il
build_variant "-DMUTATE_LIVENESS" arbiter_liveness_fail.il
build_variant "-DMUTATE_SAFETY" arbiter_safety_fail.il

(
	cd -- "${work_dir}"
	sby --suprove "${suprove}" --prefix "${work_dir}/live" \
		-f arbiter_live.sby
	sby --prefix "${work_dir}/safety" -f arbiter_safety.sby
	sby --prefix "${work_dir}/reach" -f arbiter_reach.sby
)
