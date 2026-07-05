# OpenTitan SVA stress

| Command | Result |
| --- | --- |
| `tests/opentitan_sva_stress/run_stress_sva.sh` | PASS |

| Target | Enabled checks | Result |
| --- | ---: | --- |
| `prim_fifo_sync` | 21 | PASS |
| `prim_alert_rxtx` | 7 | PASS |
| `prim_esc_rxtx` | 10 | PASS |
| `clkmgr` | 5 | PASS |
| `sha3pad` | 1 | PASS |

| Source | Disabled / rewritten | Reason |
| --- | --- | --- |
| `prim_alert_rxtx_assert_fpv.sv` | `PingEn_M`, `AlertPingIgnored_A` | `throughout`, goto repetition |
| `prim_alert_rxtx_assert_fpv.sv` | `FullHandshake_S`, `PingHs_A`, `AlertHs_A`, `AlertTestHs_A` | named sequence composition/reference |
| `prim_alert_rxtx_assert_fpv.sv` | `AlertReqAck_A`, `AlertCheck1_A`, `FsmLiveness*_A` | unbounded `strong(##[1:$] ...)` |
| `prim_esc_rxtx_assert_fpv.sv` | `EscDeassert_A`, `FsmLiveness*_A` | unbounded `strong`, repetition |
| `sha3pad_assert_if.sv` | `ProcessToRun_A`, `RunThenComplete_M` | unbounded `strong(##[N:$] ...)` |
| `prim_alert_rxtx_assert_fpv.sv` | `init_pending`, sender `Idle` references rewritten to constants | hierarchical enum literals caused missing-wire frontend errors |
| `clkmgr_cg_en_sva_if.sv` | `scanmode == prim_mubi_pkg::MuBi4True` rewritten to `scanmode == 4'h6` | hierarchical package enum literal caused missing-wire frontend errors |
