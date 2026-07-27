#!/usr/bin/env bash
set -euo pipefail

suite_tag=2026-07-27
archive_name=oss-cad-suite-linux-x64-20260727.tgz
archive_sha256=d5cd8fb0276b51ae6b35945e70e03f222d82a863f2e3523b33b937be0ea416f4
download_url="https://github.com/YosysHQ/oss-cad-suite-build/releases/download/${suite_tag}/${archive_name}"

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tool_root=${YOSYS_SLANG_TOOL_ROOT:-"${repo_dir}/../.tools"}
download_dir="${tool_root}/downloads"
install_dir="${tool_root}/oss-cad-suite/${suite_tag}"
archive="${download_dir}/${archive_name}"
suprove="${install_dir}/oss-cad-suite/bin/suprove"

mkdir -p -- "${download_dir}" "${install_dir}"

if [[ ! -f "${archive}" ]]; then
	curl --fail --location --retry 3 --output "${archive}.part" "${download_url}"
	mv -- "${archive}.part" "${archive}"
fi

printf '%s  %s\n' "${archive_sha256}" "${archive}" | sha256sum --check -

if [[ ! -x "${suprove}" ]]; then
	tar -xzf "${archive}" -C "${install_dir}" \
		oss-cad-suite/bin/suprove \
		oss-cad-suite/super_prove \
		oss-cad-suite/lib \
		oss-cad-suite/libexec/abc.exe \
		oss-cad-suite/libexec/bip \
		oss-cad-suite/license
fi

smoke_dir=$(mktemp -d "${TMPDIR:-/tmp}/yosys-slang-suprove.XXXXXXXX")
cleanup() {
	rm -rf -- "${smoke_dir:?}"
}
trap cleanup EXIT
(
	cd -- "${smoke_dir}"
	"${suprove}" +simple_liveness </dev/null >/dev/null
)
cleanup
trap - EXIT
printf 'SUPROVE_EXECUTABLE=%s\n' "${suprove}"
