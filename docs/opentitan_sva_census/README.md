# OpenTitan SVA census

This directory records a source-traceable compatibility census of every formal
configuration in the pinned OpenTitan submodule. The campaign tests whether the
real property sources elaborate and lower through yosys-slang into Yosys formal
cells. It does not claim that each property has been mathematically proved.

## Reproduce

Build the plugin first:

```sh
cmake -S . -B build
cmake --build build -j2
```

The campaign runner needs FuseSoC plus the Python packages used by OpenTitan's
CSR assertion generator. One isolated setup is:

```sh
python3 -m venv --system-site-packages /workspace/.tools/opentitan-census-venv
/workspace/.tools/opentitan-census-venv/bin/pip install \
  hjson==3.1.0 mako==1.3.10 semantic_version==2.10.0 \
  mistletoe==0.9.0 tabulate pycryptodome peakrdl-systemrdl
```

Run all 125 configurations:

```sh
/workspace/.tools/opentitan-census-venv/bin/python \
  tools/opentitan_sva_census.py \
  --build-root build/opentitan_sva_census \
  --output docs/opentitan_sva_census \
  --jobs 4
```

Per-target EDAM, generated Yosys scripts, JSON netlists, and complete logs stay
under the build root. `results.json` is the machine-readable aggregate;
`targets.csv` is the target matrix; `unsupported.csv` lists every unsupported
diagnostic with source attribution.

The script explicitly enables `FPV_ON`, disables the implicit `SYNTHESIS`
define, ignores simulation-only initial blocks, elaborates bind modules as
additional tops, and uses OpenTitan's FuseSoC `formal` file lists. It records
commercial-flow `stopats`, `after_load`, tasks, and SecCm blackbox declarations,
but does not claim to reproduce those proof abstractions.

## Result meanings

- `PASS`: all properties that elaborated in the target lowered to formal cells.
- `PARTIAL`: lowering completed, but listed unsupported properties were omitted.
- `LOWERING_FAIL`: a non-SVA frontend, RTL, bind, or configuration error blocked
  target elaboration.
- `SETUP_FAIL`: FuseSoC could not produce an EDAM file.

The runner returns nonzero for setup or harness failures but still writes a
complete report. Unsupported properties are never silently converted into
passing checks: census mode downgrades their diagnostics and drops only the
affected obligation.
