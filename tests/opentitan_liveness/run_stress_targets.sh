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
suprove_driver="${root}/tools/suprove_all_justice.py"
stress_dir="${root}/tests/opentitan_sva_stress"
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/yosys-slang-ot-live.XXXXXXXX")
cleanup() {
	rm -rf -- "${work_dir:?}"
}
trap cleanup EXIT

for target in alert esc sha3pad tlul
do
	BUILD_ROOT="${work_dir}/stress" \
	PLUGIN="${plugin}" \
	"${stress_dir}/${target}/run.sh"

done

pattern_dir="${work_dir}/patterns"
mkdir -p -- "${pattern_dir}"
cp -- "${test_dir}/target_patterns_live.sby" "${pattern_dir}/target_patterns_live.sby"
cp -- "${test_dir}/target_patterns_reach.sby" "${pattern_dir}/target_patterns_reach.sby"

for top in \
	alert_pattern_pass alert_pattern_fail \
	esc_pattern_pass esc_pattern_fail \
	sha3_pattern_pass sha3_pattern_fail \
	tlul_pattern_pass tlul_pattern_fail
do
	yosys -q -m "${plugin}" -p "
		read_slang ${test_dir}/target_patterns.sv;
		prep -top ${top};
		async2sync;
		formalff -clk2ff;
		chformal -lower;
		opt_clean;
		write_rtlil ${pattern_dir}/${top}.il
	"
done

(
	cd -- "${pattern_dir}"
	export YOSYS_SLANG_REAL_SUPROVE="${suprove}"
	sby --suprove "${suprove_driver}" --prefix "${pattern_dir}/live" \
		-f target_patterns_live.sby
	sby --prefix "${pattern_dir}/reach" -f target_patterns_reach.sby
)
