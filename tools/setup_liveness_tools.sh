#!/usr/bin/env bash
set -euo pipefail

suite_tag=2026-07-27
archive_name=oss-cad-suite-linux-x64-20260727.tgz
archive_sha256=d5cd8fb0276b51ae6b35945e70e03f222d82a863f2e3523b33b937be0ea416f4
download_url="https://github.com/YosysHQ/oss-cad-suite-build/releases/download/${suite_tag}/${archive_name}"
# The archive members suprove needs
members=(
	oss-cad-suite/bin/suprove
	oss-cad-suite/super_prove
	oss-cad-suite/lib
	oss-cad-suite/libexec/abc.exe
	oss-cad-suite/libexec/bip
	oss-cad-suite/license
)

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tool_root=${YOSYS_SLANG_TOOL_ROOT:-"${repo_dir}/../.tools"}
download_dir="${tool_root}/downloads"
install_dir="${tool_root}/oss-cad-suite/${suite_tag}"
archive="${download_dir}/${archive_name}"
suprove="${install_dir}/oss-cad-suite/bin/suprove"
# Identifies the archive and the member list an install was extracted with,
# written once the extraction is complete
stamp="${install_dir}/.install-id"
install_id=$(printf '%s\n' "${archive_sha256}" "${members[@]}" |
	sha256sum | cut -d ' ' -f 1)

extract() {
	mkdir -p -- "${download_dir}" "${install_dir}"
	if [[ ! -f "${archive}" ]]; then
		curl --fail --location --retry 3 --output "${archive}.part" "${download_url}"
		mv -- "${archive}.part" "${archive}"
	fi
	printf '%s  %s\n' "${archive_sha256}" "${archive}" | sha256sum --check -

	# The stamp of an earlier version of this script, which held the checksum
	# alone
	rm -f -- "${stamp}" "${install_dir}/.archive-sha256"
	# Members no longer in the list go too
	rm -rf -- "${install_dir:?}/oss-cad-suite"
	tar -xzf "${archive}" -C "${install_dir}" "${members[@]}"
	printf '%s\n' "${install_id}" > "${stamp}"
}

# Proves a justice property that holds and one that fails. suprove exits with 0
# even when an engine it needs is missing, so the verdicts are what is checked.
smoke_test() {
	local dir pass fail
	dir=$(mktemp -d "${TMPDIR:-/tmp}/sv-elab-suprove.XXXXXXXX")
	# One latch, which stays low, and justice on it: never met, so it holds
	printf 'aig 1 0 1 0 0 0 0 1 0\n2\n1\n2\n' > "${dir}/pass.aig"
	# One latch, which toggles, and justice on it: met infinitely often, so a
	# lasso fails it
	printf 'aig 1 0 1 0 0 0 0 1 0\n3\n1\n2\n' > "${dir}/fail.aig"
	pass=$(cd -- "${dir}" && TMPDIR="${dir}" "${suprove}" +simple_liveness pass.aig) || true
	fail=$(cd -- "${dir}" && TMPDIR="${dir}" "${suprove}" +simple_liveness fail.aig) || true
	rm -rf -- "${dir:?}"
	[[ "${pass%%$'\n'*}" == 0 && "${fail%%$'\n'*}" == 1 ]]
}

# An install extracted from the pinned archive with these members is used as it
# is, so that one restored from a cache needs neither the archive nor the
# download. If it fails the smoke test, it is extracted again, once.
extracted=0
if [[ ! -f "${stamp}" || "$(<"${stamp}")" != "${install_id}" ]]; then
	extract
	extracted=1
fi
if ! smoke_test; then
	if ((extracted)); then
		echo 'error: the suprove just extracted fails its smoke test' >&2
		exit 1
	fi
	echo 'The installed suprove fails its smoke test, extracting it again' >&2
	extract
	if ! smoke_test; then
		echo 'error: the suprove just extracted fails its smoke test' >&2
		exit 1
	fi
fi
printf 'SUPROVE_EXECUTABLE=%s\n' "${suprove}"
