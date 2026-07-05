# OpenTitan TLUL SVA smoke

| Item | Value |
| --- | --- |
| OpenTitan commit | `98a020b74d02c316fc2cc682bb212214b5c27e0e` |
| Checker | `hw/ip/tlul/rtl/tlul_assert.sv` |
| Frontend | `read_slang --no-synthesis-define` |
| Mode | SVA properties, not OpenTitan `YOSYS` macro mode |
| BMC | `yosys-smtbmc -s yices -t 8` |

| Command | Result |
| --- | --- |
| `tests/opentitan_tlul/run_tlul_sva.sh` | FAIL |
| `ASSUME_LEGAL_D2H=1 tests/opentitan_tlul/run_tlul_sva.sh` | PASS |
| `ASSUME_LEGAL_D2H_IMMEDIATE=1 tests/opentitan_tlul/run_tlul_sva.sh` | PASS |

| Property | Kind | Unconstrained | With `ASSUME_LEGAL_D2H=1` |
| --- | --- | --- | --- |
| `u_tlul_assert.p_dbw.TlDbw_A` | assert | PASS | PASS |
| `u_tlul_assert.gen_device.gen_h2d.pendingReqPerSrc_M` | assume | synthesized | synthesized |
| `u_tlul_assert.gen_device.gen_h2d.legalAParam_M` | assume | synthesized | synthesized |
| `u_tlul_assert.gen_device.gen_h2d.contigMask_M` | assume | synthesized | synthesized |
| `u_tlul_assert.gen_device.gen_d2h.respSzEqReqSz_A` | assert | FAIL | PASS |
| `u_tlul_assert.gen_device.gen_d2h.respOpcode_A` | assert | FAIL | PASS |
| `u_tlul_assert.gen_device.gen_d2h.respMustHaveReq_A` | assert | FAIL | PASS |
| `u_tlul_assert.gen_device.gen_d2h.legalDParam_A` | assert | FAIL | PASS |

| FAIL | Comment |
| --- | --- |
| `respSzEqReqSz_A` | unconstrained `d2h` can return a size different from the tracked request |
| `respOpcode_A` | unconstrained `d2h` can return `AccessAck` / `AccessAckData` inconsistent with the tracked request opcode |
| `respMustHaveReq_A` | unconstrained `d2h` can produce a response with no forwarded or pending request |
| `legalDParam_A` | unconstrained `d2h` can drive nonzero reserved `d_param` |

| Disabled before synthesis | Reason |
| --- | --- |
| `aDataKnown_A`, `aDataKnown_M` | `$isunknown` |
| `dDataKnown_A`, `dDataKnown_M` | `$isunknown` |
| `aKnown_A`, `dKnown_A`, `aReadyKnown_A`, `dReadyKnown_A` | OpenTitan knownness macros expand to `$isunknown` |
| `legalAOpcodeErr_A` | unbounded `s_eventually` |
| `sizeGTEMaskErr_A` | unbounded `s_eventually` |
| `sizeMatchesMaskErr_A` | unbounded `s_eventually` |
| `addrSizeAlignedErr_A` | unbounded `s_eventually` |
| TLUL cover properties | coverage-only; named sequences, match-item locals, `$changed`, goto repetition |

| Generated-source adaptation | Reason |
| --- | --- |
| Inline assertion sequences | no standalone named `sequence` declaration lowering |
| Numeric TLUL enum encodings | package enum literal lowering issue |
| Explicit `disable iff (disable_sva || !rst_ni)` | preserve OpenTitan default disable in generated properties |
| Tie `disable_sva` low | no UVM runtime assertion control in this smoke harness |
