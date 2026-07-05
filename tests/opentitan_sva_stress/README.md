# OpenTitan SVA stress

| Command | Result |
| --- | --- |
| `tests/opentitan_sva_stress/run_stress_sva.sh` | PASS |

| Target | Enabled checks | Result |
| --- | ---: | --- |
| `prim_fifo_sync` | 21 | PASS |
| `prim_alert_rxtx` | 6 | PASS |
| `prim_esc_rxtx` | 7 | PASS |
| `clkmgr` | 1 | PASS |
| `sha3pad` | 1 | PASS |

| Source | Disabled / rewritten | Reason |
| --- | --- | --- |
| `prim_alert_rxtx_assert_fpv.sv` | `PingEn_M`, `AlertPingIgnored_A` | `throughout`, goto repetition |
| `prim_alert_rxtx_assert_fpv.sv` | `FullHandshake_S`, `PingHs_A`, `AlertHs_A`, `AlertTestHs_A` | named sequence composition, `$changed` |
| `prim_alert_rxtx_assert_fpv.sv` | `AlertReqAck_A`, `AlertCheck1_A`, `FsmLiveness*_A` | unbounded `strong(##[1:$] ...)` |
| `prim_esc_rxtx_assert_fpv.sv` | `EscDeassert_A`, `FsmLiveness*_A` | unbounded `strong`, repetition |
| `prim_alert_rxtx_assert_fpv.sv`, `prim_esc_rxtx_assert_fpv.sv`, `clkmgr_*_sva_if.sv` | `$rose`, `$fell`, `$stable`, `$changed` checks | sampled-value functions reach yosys-slang as unsupported system tasks |
| `prim_esc_rxtx_assert_fpv.sv` | `SingleSigIntDetected*_A` | `$onehot` reaches yosys-slang as unsupported system task |
| `prim_alert_rxtx_assert_fpv.sv` | `*ErrorsAreOH_M` rewritten from `$onehot0({a,b})` to `!(a && b)` | preserve two-bit assumption parity |
| `sha3pad_assert_if.sv` | `ProcessToRun_A`, `RunThenComplete_M` | unbounded `strong(##[N:$] ...)` |
| `prim_alert_rxtx_assert_fpv.sv` | `init_pending`, sender `Idle` references rewritten to constants | hierarchical enum literals caused missing-wire frontend errors |
