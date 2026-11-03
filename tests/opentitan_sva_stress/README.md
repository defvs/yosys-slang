# OpenTitan SVA Stress

This directory contains opt-in stress tests for sv-elab SVA lowering using
OpenTitan verification collateral. The tests use the SVA properties directly,
with small source filters for unsupported constructs; they do not use
OpenTitan's `YOSYS` macro mode.

The default `run_all.sh` path is expected to pass. TLUL uses
`ASSUME_LEGAL_D2H=1` by default so the harness checks SVA lowering rather than
failing on an intentionally unconstrained response channel.

A property dropped in lowering would pass BMC unnoticed, so each target also
fails unless the model it hands to BMC has the number of checks below. Those
count the assertions, assumptions and covers, including the ones of the harness.

## Coverage

| Target | Enabled checks | Expected result |
| --- | ---: | --- |
| `prim_fifo_sync` | 21 | PASS |
| `prim_alert_rxtx` | 10 | PASS |
| `prim_esc_rxtx` | 10 | PASS |
| `clkmgr` | 5 | PASS |
| `sha3pad` | 1 | PASS |
| `tlul` | 12 | PASS with legal D2H assumption |

Unsupported or rewritten OpenTitan constructs:

| Source | Disabled / rewritten | Reason |
| --- | --- | --- |
| `prim_alert_rxtx_assert_fpv.sv` | `PingEn_M`, `AlertPingIgnored_A` | `throughout`, goto repetition |
| `prim_alert_rxtx_assert_fpv.sv` | `AlertReqAck_A`, `AlertCheck1_A`, `FsmLiveness*_A` | unbounded `strong(##[1:$] ...)` |
| `prim_esc_rxtx_assert_fpv.sv` | `EscDeassert_A`, `FsmLiveness*_A` | unbounded `strong`, repetition |
| `sha3pad_assert_if.sv` | `ProcessToRun_A`, `RunThenComplete_M` | unbounded `strong(##[N:$] ...)` |
| `tlul_assert.sv` | knownness assertions | `$isunknown` |
| `tlul_assert.sv` | `legalAOpcodeErr_A`, `sizeGTEMaskErr_A`, `sizeMatchesMaskErr_A`, `addrSizeAlignedErr_A` | unbounded `s_eventually` |
| `tlul_assert.sv` | cover properties | coverage-only; some cover sequences use match-item locals and goto repetition |
| `tlul_assert.sv` | `noOutstandingReqsAtEndOfSim_A` (`ASSERT_FINAL`) | checks the end of simulation in a final procedure, which has no counterpart in a netlist; sv-elab rejects it |
| `prim_alert_rxtx_assert_fpv.sv` | hierarchical enum literals in the `inside` sets of `init_pending` and `AlertPingOk_A`, rewritten to their encoding | the frontend's `inside` needs integral items that slang evaluates as constants, and slang does not evaluate hierarchical references ("Feature unimplemented ... (bool) const_result", also in plain RTL and on upstream) |

## Main Suite Integration

The OpenTitan tree is large and only these tests need it, so it is not a
submodule. `common/fetch_opentitan.sh` checks out the pinned revision into
`tests/third_party/opentitan` on first use.

Opt-in:

```sh
cmake -S . -B build -DENABLE_OPENTITAN_SVA_STRESS=ON
```

Run only the OpenTitan stress test through CTest:

```sh
ctest --test-dir build -L opentitan --output-on-failure
```
