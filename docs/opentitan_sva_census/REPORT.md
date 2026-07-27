# OpenTitan SVA compatibility census

Generated: `2026-07-27T18:49:54.853628+00:00`

OpenTitan revision: `98a020b74d02c316fc2cc682bb212214b5c27e0e`

yosys-slang revision: `ec4b91276c1450baee3f1ebca664d395078bf10f`

## Scope and interpretation

All **125** formal configurations in the pinned OpenTitan snapshot were attempted using their FuseSoC `formal` target. `FPV_ON` was enabled and bind modules were elaborated as extra tops.

A PASS means every elaborated property lowered into Yosys formal cells. It does **not** mean the properties were mathematically proved. PARTIAL means lowering completed but one or more explicitly reported properties were omitted because their SVA form is unsupported.

Commercial-flow Tcl (`stopats`, `after_load`, task filtering) is recorded but not claimed to be reproduced by this frontend compatibility census. The SecCm primitive blackbox list is also recorded but deliberately not applied: this census checks source/SVA lowering, while those blackboxes are proof abstractions that would prevent open elaboration of the hierarchical countermeasure checks.

## Summary

| Result | Targets |
| --- | ---: |
| PASS | 59 |
| PARTIAL | 48 |
| LOWERING_FAIL | 17 |
| LOWERING_TIMEOUT | 0 |
| SETUP_FAIL | 1 |
| SETUP_TIMEOUT | 0 |

De-duplicated source-level property candidates in attempted EDAM file lists: **5967**.

This source count includes transitive file-list candidates and is not presented as an elaborated-instance denominator. Emitted cells and diagnostics below are elaboration results.

Emitted formal cells: **19050** `$check`, **0** legacy `$live`, **0** `$fair`.

Obligations by `$check` flavor: **17212** assert, **1255** assume, **362** cover, **219** live, **2** fair.

Unsupported-SVA diagnostics: **1144**.

Unique unsupported source forms: **57**.

## Conclusions

- **107/125** configurations (85.6%) completed elaboration and lowering; 59 were clean and 48 were partial.
- The completed configurations emitted **219** liveness obligations. This is direct evidence that OpenTitan liveness properties now reach Yosys, but it is not a proof result.
- **57** unique unsupported source forms produced **1144** configuration/instance diagnostics. Repetition is expected because common TL-UL, Ibex, and OTBN sources elaborate in many targets and generated instances.
- The dominant unsupported classes are four-state knownness (`$isunknown`), match-item/local-variable sequences, and temporal forms combining `throughout`, goto repetition, or unbounded ranges that the current liveness lowering cannot represent.
- **18** configurations remain blocked before a complete property inventory. Their exact root diagnostics are listed below; they must not be counted as property passes or failures.
- **22** stale config core identifiers were mapped to an unambiguous same-component core in the pinned source tree. Every such resolution is retained in `results.json`.

## Blocked configuration roots

| First blocking diagnostic | Configurations |
| --- | ---: |
| assignment pattern target type cannot be deduced in this context | 2 |
| unknown module 'prim_alert_rxtx_tb' | 2 |
| unknown module 'prim_alert_rxtx_async_tb' | 2 |
| unknown module 'i_nopass1' | 2 |
| multiple asynchronous loads unsupported | 2 |
| use of undeclared identifier 'ScrmblKeyNotAllZero_A0' | 2 |
| 'pinmux_tb' is not a valid top-level module | 2 |
| could not resolve hierarchical path name 'u_reg_top_prim' | 1 |
| no member named 'tdo_idx' in 'target_cfg_t' | 1 |
| no member named 'tms_idx' in 'target_cfg_t' | 1 |
| 'lowrisc:dv:top_darjeeling_flash_ctrl_sva' or any of its dependencies requires 'top_darjeeling_flash_ctrl_sva', but this core was not found | 1 |

## Unsupported SVA source forms

| Source | Property | Diagnostic | Affected configurations |
| --- | --- | --- | ---: |
| `hw/ip/edn/rtl/edn.sv:125` | `EdnDataStable_A` | unsupported assertion statement | 2 |
| `hw/ip/keymgr/rtl/keymgr_ctrl.sv:895` | `SecCmCFILinear_A` | SVA match items and local-variable actions are not synthesizable | 4 |
| `hw/ip/kmac/rtl/sha3pad.sv:859` | `PartialEndOfMsg_M` | unsupported assertion statement | 3 |
| `hw/ip/lc_ctrl/rtl/lc_ctrl_fsm.sv:891` | `SecCmCFILinear_A` | SVA match items and local-variable actions are not synthesizable | 4 |
| `hw/ip/otbn/rtl/otbn.sv:1278` | `SecWipeChangedBaseRegs_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1320` | `SecWipeChangedWideRegs_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1393` | `ImemSecWipeRequiresOtpKey_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1399` | `DmemSecWipeRequiresOtpKey_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1435` | `BusyOrLockingFromStartToDone_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn_core.sv:1319` | `OtbnStartStopGlobalEscCntrMeasure_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn_core.sv:1323` | `OtbnStartStopLocalEscCntrMeasure_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn_decoder.sv:1184` | `IbexRegImmAluOpBaseKnown` | encountered unsupported SVA feature | 2 |
| `hw/ip/otbn/rtl/otbn_rf_base.sv:243` | `OtbnRfBaseRdAKnown` | encountered unsupported SVA feature | 2 |
| `hw/ip/otbn/rtl/otbn_rf_base.sv:244` | `OtbnRfBaseRdBKnown` | encountered unsupported SVA feature | 2 |
| `hw/ip/otbn/rtl/otbn_rf_bignum.sv:191` | `OtbnRfBignumRdAKnown` | encountered unsupported SVA feature | 2 |
| `hw/ip/otbn/rtl/otbn_rf_bignum.sv:192` | `OtbnRfBignumRdBKnown` | encountered unsupported SVA feature | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:55` | `PingEn_M` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:108` | `AlertPingIgnored_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:94` | `PingEn_M` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:174` | `AlertPingOk_A` | encountered unsupported SVA feature | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:181` | `AlertPingIgnored_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/rtl/prim_packer.sv:325` | `FlushFollowedByDone_A` | unsupported assertion statement | 4 |
| `hw/ip/prim/rtl/prim_rom_adv.sv:46` | `noXOnCsI` | encountered unsupported SVA feature | 3 |
| `hw/ip/prim/rtl/prim_sync_reqack.sv:393` | `SyncReqAckRstSrc` | unsupported assertion statement | 2 |
| `hw/ip/prim/rtl/prim_sync_reqack.sv:396` | `SyncReqAckRstDst` | unsupported assertion statement | 2 |
| `hw/ip/prim_generic/rtl/prim_rom.sv:50` | `noXOnCsI` | encountered unsupported SVA feature | 3 |
| `hw/ip/rom_ctrl/rtl/rom_ctrl_fsm.sv:248` | `SecCmCFILinear_A` | SVA match items and local-variable actions are not synthesizable | 5 |
| `hw/ip/tlul/rtl/tlul_adapter_sram.sv:684` | `TlOutKnownIfFifoKnown_A` | encountered unsupported SVA feature | 9 |
| `hw/ip/tlul/rtl/tlul_assert.sv:173` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:174` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:175` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:176` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:177` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:178` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:179` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:180` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:219` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:220` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:221` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:222` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:223` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:224` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:225` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:226` | `(unattributed)` | encountered unsupported SVA feature | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:463` | `(unattributed)` | SVA match items and local-variable actions are not synthesizable | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:465` | `(unattributed)` | SVA match items and local-variable actions are not synthesizable | 35 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:832` | `IbexInstrLSBsKnown` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:834` | `IbexC0Known1` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:836` | `IbexC1Known1` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:838` | `IbexC1Known2` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:840` | `IbexC1Known3` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:843` | `IbexC2Known1` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_decoder.sv:1210` | `IbexRegImmAluOpKnown` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_if_stage.sv:834` | `IbexInstrAddrUnknown` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_load_store_unit.sv:610` | `IbexDataTypeKnown` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_load_store_unit.sv:611` | `IbexDataOffsetKnown` | encountered unsupported SVA feature | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_load_store_unit.sv:619` | `IbexDataAddrUnknown` | encountered unsupported SVA feature | 2 |

## Target results

| Configuration | Variant | Result | Checks | Assert | Assume | Cover | Live | Fair | Unsupported | Reason |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `pinmux_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | assignment pattern target type cannot be deduced in this context |
| `pinmux_chip_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | assignment pattern target type cannot be deduced in this context |
| `rv_plic_fpv` | earlgrey | PARTIAL | 289 | 266 | 10 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `alert_handler_esc_timer_fpv` | earlgrey | PASS | 47 | 39 | 7 | 0 | 1 | 0 | 0 | all elaborated properties lowered |
| `alert_handler_ping_timer_fpv` | earlgrey | PASS | 55 | 46 | 9 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_alert_rxtx_fpv` | earlgrey | PARTIAL | 67 | 59 | 4 | 0 | 4 | 0 | 2 | 2 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_fatal_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_tb' |
| `prim_alert_rxtx_async_fpv` | earlgrey | PARTIAL | 83 | 75 | 4 | 0 | 4 | 0 | 3 | 3 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_async_fatal_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_async_tb' |
| `prim_arbiter_fixed_fpv` | earlgrey | PASS | 10 | 10 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_arbiter_ppc_fpv` | earlgrey | PASS | 29 | 20 | 8 | 0 | 1 | 0 | 0 | all elaborated properties lowered |
| `prim_arbiter_tree_fpv` | earlgrey | PASS | 29 | 20 | 8 | 0 | 1 | 0 | 0 | all elaborated properties lowered |
| `prim_count_max_reset_fpv` | earlgrey | PASS | 15 | 14 | 0 | 1 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_count_zero_reset_fpv` | earlgrey | PASS | 15 | 14 | 0 | 1 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_esc_rxtx_fpv` | earlgrey | PASS | 64 | 58 | 3 | 0 | 2 | 0 | 0 | all elaborated properties lowered |
| `prim_fifo_sync_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'i_nopass1' |
| `prim_fifo_async_sram_adapter_fpv` | earlgrey | PASS | 15 | 10 | 4 | 1 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_lfsr_fpv` | earlgrey | PASS | 1483 | 1448 | 35 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_max_tree` | earlgrey | PASS | 5 | 5 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_onehot_check` | earlgrey | PASS | 3 | 3 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_packer_fpv` | earlgrey | PARTIAL | 152 | 85 | 67 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `prim_secded_22_16_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_28_22_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_39_32_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_64_57_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_72_64_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_hamming_22_16_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_hamming_39_32_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_hamming_72_64_fpv` | earlgrey | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_22_16_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_28_22_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_39_32_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_64_57_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_72_64_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_hamming_22_16_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_hamming_39_32_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_hamming_72_64_fpv` | earlgrey | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_sum_tree` | earlgrey | PASS | 3 | 3 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `aes_masked_sec_cm` | earlgrey | PARTIAL | 274 | 229 | 32 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `aes_unmasked_sec_cm` | earlgrey | PARTIAL | 272 | 227 | 32 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `alert_handler_sec_cm` | earlgrey | PARTIAL | 1923 | 1905 | 5 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `clkmgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 18 | multiple asynchronous loads unsupported |
| `csrng_sec_cm` | earlgrey | PARTIAL | 275 | 227 | 34 | 8 | 6 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `edn_sec_cm` | earlgrey | PARTIAL | 274 | 224 | 36 | 8 | 6 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `entropy_src_sec_cm` | earlgrey | PARTIAL | 1159 | 1046 | 96 | 12 | 5 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `flash_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | could not resolve hierarchical path name 'u_reg_top_prim' |
| `keymgr_sec_cm` | earlgrey | PARTIAL | 227 | 187 | 27 | 8 | 5 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `kmac_sec_cm` | earlgrey | PARTIAL | 366 | 282 | 66 | 13 | 5 | 0 | 24 | 24 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm` | earlgrey | PARTIAL | 248 | 197 | 26 | 16 | 9 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `otp_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | use of undeclared identifier 'ScrmblKeyNotAllZero_A0' |
| `otbn_sec_cm` | earlgrey | PARTIAL | 612 | 586 | 13 | 8 | 5 | 0 | 220 | 220 unsupported SVA diagnostic(s) |
| `pwrmgr_sec_cm` | earlgrey | PARTIAL | 142 | 119 | 10 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `rom_ctrl_sec_cm` | earlgrey | PARTIAL | 192 | 154 | 13 | 16 | 9 | 0 | 23 | 23 unsupported SVA diagnostic(s) |
| `rstmgr_sec_cm` | earlgrey | PARTIAL | 378 | 325 | 40 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `rv_core_ibex_sec_cm` | earlgrey | PARTIAL | 407 | 359 | 32 | 16 | 0 | 0 | 27 | 27 unsupported SVA diagnostic(s) |
| `sram_ctrl_sec_cm` | earlgrey | PARTIAL | 237 | 191 | 21 | 16 | 9 | 0 | 20 | 20 unsupported SVA diagnostic(s) |
| `pinmux_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'pinmux_tb' is not a valid top-level module |
| `rv_plic_sec_cm` | earlgrey | PARTIAL | 281 | 254 | 14 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm_fsm` | earlgrey | PARTIAL | 17 | 17 | 0 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `rom_ctrl_sec_cm_fsm` | earlgrey | PARTIAL | 13 | 13 | 0 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `keymgr_sec_cm_fsm` | earlgrey | PARTIAL | 22 | 22 | 0 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `pwrmgr_esc_unstoppable` | earlgrey | PARTIAL | 142 | 119 | 10 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `pinmux_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | no member named 'tdo_idx' in 'target_cfg_t' |
| `pinmux_chip_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | no member named 'tms_idx' in 'target_cfg_t' |
| `rv_plic_fpv` | darjeeling | PARTIAL | 236 | 213 | 10 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `alert_handler_esc_timer_fpv` | darjeeling | PASS | 47 | 39 | 7 | 0 | 1 | 0 | 0 | all elaborated properties lowered |
| `alert_handler_ping_timer_fpv` | darjeeling | PASS | 55 | 46 | 9 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_alert_rxtx_fpv` | darjeeling | PARTIAL | 67 | 59 | 4 | 0 | 4 | 0 | 2 | 2 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_fatal_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_tb' |
| `prim_alert_rxtx_async_fpv` | darjeeling | PARTIAL | 71 | 63 | 4 | 0 | 4 | 0 | 3 | 3 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_async_fatal_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_async_tb' |
| `prim_arbiter_fixed_fpv` | darjeeling | PASS | 10 | 10 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_arbiter_ppc_fpv` | darjeeling | PASS | 29 | 20 | 8 | 0 | 1 | 0 | 0 | all elaborated properties lowered |
| `prim_arbiter_tree_fpv` | darjeeling | PASS | 29 | 20 | 8 | 0 | 1 | 0 | 0 | all elaborated properties lowered |
| `prim_count_max_reset_fpv` | darjeeling | PASS | 14 | 14 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_count_zero_reset_fpv` | darjeeling | PASS | 14 | 14 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_esc_rxtx_fpv` | darjeeling | PASS | 64 | 58 | 3 | 0 | 2 | 0 | 0 | all elaborated properties lowered |
| `prim_fifo_sync_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'i_nopass1' |
| `prim_fifo_async_sram_adapter_fpv` | darjeeling | PASS | 15 | 10 | 4 | 1 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_lfsr_fpv` | darjeeling | PASS | 1483 | 1448 | 35 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_max_tree` | darjeeling | PASS | 5 | 5 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_onehot_check` | darjeeling | PASS | 3 | 3 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_packer_fpv` | darjeeling | PARTIAL | 152 | 85 | 67 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `prim_secded_22_16_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_28_22_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_39_32_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_64_57_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_72_64_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_hamming_22_16_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_hamming_39_32_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_hamming_72_64_fpv` | darjeeling | PASS | 8 | 7 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_22_16_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_28_22_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_39_32_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_64_57_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_72_64_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_hamming_22_16_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_hamming_39_32_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_secded_inv_hamming_72_64_fpv` | darjeeling | PASS | 10 | 9 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_sum_tree` | darjeeling | PASS | 3 | 3 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `aes_masked_sec_cm` | darjeeling | PARTIAL | 274 | 229 | 32 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `aes_unmasked_sec_cm` | darjeeling | PARTIAL | 272 | 227 | 32 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `alert_handler_sec_cm` | darjeeling | PARTIAL | 2247 | 2229 | 5 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `clkmgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 18 | multiple asynchronous loads unsupported |
| `csrng_sec_cm` | darjeeling | PARTIAL | 275 | 227 | 34 | 8 | 6 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `edn_sec_cm` | darjeeling | PARTIAL | 274 | 224 | 36 | 8 | 6 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `flash_ctrl_sec_cm` | darjeeling | SETUP_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'lowrisc:dv:top_darjeeling_flash_ctrl_sva' or any of its dependencies requires 'top_darjeeling_flash_ctrl_sva', but this core was not found |
| `keymgr_sec_cm` | darjeeling | PARTIAL | 227 | 187 | 27 | 8 | 5 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `kmac_sec_cm` | darjeeling | PARTIAL | 366 | 282 | 66 | 13 | 5 | 0 | 24 | 24 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm` | darjeeling | PARTIAL | 248 | 197 | 26 | 16 | 9 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `otp_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | use of undeclared identifier 'ScrmblKeyNotAllZero_A0' |
| `otbn_sec_cm` | darjeeling | PARTIAL | 612 | 586 | 13 | 8 | 5 | 0 | 220 | 220 unsupported SVA diagnostic(s) |
| `pwrmgr_sec_cm` | darjeeling | PARTIAL | 141 | 118 | 10 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `rom_ctrl_sec_cm` | darjeeling | PARTIAL | 192 | 154 | 13 | 16 | 9 | 0 | 23 | 23 unsupported SVA diagnostic(s) |
| `rstmgr_sec_cm` | darjeeling | PARTIAL | 237 | 199 | 25 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `rv_core_ibex_sec_cm` | darjeeling | PARTIAL | 407 | 359 | 32 | 16 | 0 | 0 | 27 | 27 unsupported SVA diagnostic(s) |
| `sram_ctrl_sec_cm` | darjeeling | PARTIAL | 237 | 191 | 21 | 16 | 9 | 0 | 20 | 20 unsupported SVA diagnostic(s) |
| `pinmux_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'pinmux_tb' is not a valid top-level module |
| `rv_plic_sec_cm` | darjeeling | PARTIAL | 228 | 201 | 14 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm_fsm` | darjeeling | PARTIAL | 17 | 17 | 0 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `rom_ctrl_sec_cm_fsm` | darjeeling | PARTIAL | 13 | 13 | 0 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `keymgr_sec_cm_fsm` | darjeeling | PARTIAL | 22 | 22 | 0 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `pwrmgr_esc_unstoppable` | darjeeling | PARTIAL | 141 | 118 | 10 | 8 | 5 | 0 | 18 | 18 unsupported SVA diagnostic(s) |
| `rv_timer_fpv` | standalone | PASS | 51 | 50 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `rom_ctrl_fpv` | standalone | PARTIAL | 159 | 158 | 1 | 0 | 0 | 0 | 5 | 5 unsupported SVA diagnostic(s) |

Machine-readable detail is in `results.json`, `targets.csv`, and `unsupported.csv`. Per-target scripts and complete logs are retained under the campaign build directory recorded in `results.json`.
