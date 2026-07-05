# OpenTitan SVA Stress

This directory contains opt-in stress tests for yosys-slang SVA lowering using
OpenTitan verification collateral. The tests use the SVA properties directly,
with small source filters for unsupported constructs; they do not use
OpenTitan's `YOSYS` macro mode.

The default `run_all.sh` path is expected to pass. TLUL uses
`ASSUME_LEGAL_D2H=1` by default so the harness checks SVA lowering rather than
failing on an intentionally unconstrained response channel.

## Coverage

| Target | Enabled checks | Expected result |
| --- | ---: | --- |
| `prim_fifo_sync` | 21 | PASS |
| `prim_alert_rxtx` | 7 | PASS |
| `prim_esc_rxtx` | 10 | PASS |
| `clkmgr` | 5 | PASS |
| `sha3pad` | 1 | PASS |
| `tlul` | generated from `tlul_assert.sv` | PASS with legal D2H assumption |

Unsupported or rewritten OpenTitan constructs:

| Source | Disabled / rewritten | Reason |
| --- | --- | --- |
| `prim_alert_rxtx_assert_fpv.sv` | `PingEn_M`, `AlertPingIgnored_A` | `throughout`, goto repetition |
| `prim_alert_rxtx_assert_fpv.sv` | `FullHandshake_S`, `PingHs_A`, `AlertHs_A`, `AlertTestHs_A` | named sequence composition/reference |
| `prim_alert_rxtx_assert_fpv.sv` | `AlertReqAck_A`, `AlertCheck1_A`, `FsmLiveness*_A` | unbounded `strong(##[1:$] ...)` |
| `prim_esc_rxtx_assert_fpv.sv` | `EscDeassert_A`, `FsmLiveness*_A` | unbounded `strong`, repetition |
| `sha3pad_assert_if.sv` | `ProcessToRun_A`, `RunThenComplete_M` | unbounded `strong(##[N:$] ...)` |
| `tlul_assert.sv` | knownness assertions | `$isunknown` |
| `tlul_assert.sv` | `legalAOpcodeErr_A`, `sizeGTEMaskErr_A`, `sizeMatchesMaskErr_A`, `addrSizeAlignedErr_A` | unbounded `s_eventually` |
| `tlul_assert.sv` | cover properties | coverage-only; some cover sequences use match-item locals and goto repetition |
| OpenTitan enum references | rewritten to constants where needed | hierarchical enum literals caused missing-wire frontend errors (FIXME) |

## Main Suite Integration

Opt-in:

```sh
cmake -S . -B build -DENABLE_OPENTITAN_SVA_STRESS=ON
```

Run only the OpenTitan stress test through CTest:

```sh
ctest --test-dir build -L opentitan --output-on-failure
```
