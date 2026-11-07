# OpenTitan SVA compatibility census

Generated: `2026-11-07T10:00:32.082498+00:00`

OpenTitan revision: `98a020b74d02c316fc2cc682bb212214b5c27e0e`

sv-elab revision: `ff51bd60b4e4120bf8ae310f6784635c24c4ed8c`

## Scope and interpretation

All **125** formal configurations in the pinned OpenTitan snapshot were attempted using their FuseSoC `formal` target. `FPV_ON` was enabled and bind modules were elaborated as extra tops.

A PASS means every elaborated property lowered into Yosys formal cells. It does **not** mean the properties were mathematically proved. PARTIAL means lowering completed once the properties the frontend rejected had been dropped from a filtered copy of their source; each of them is listed below with the diagnostic that rejected it.

Commercial-flow Tcl (`stopats`, `after_load`, task filtering) is recorded but not claimed to be reproduced by this frontend compatibility census. The SecCm primitive blackbox list is also recorded but deliberately not applied: this census checks source/SVA lowering, while those blackboxes are proof abstractions that would prevent open elaboration of the hierarchical countermeasure checks.

## Source filters

The census reads a copy of the OpenTitan tree with these rewrites, which keep what the sources do:

| Source | Rewrite |
| --- | --- |
| `hw/ip/prim/rtl/prim_assert.sv` | `ASSERT_FPV_LINEAR_FSM` clocks a flop on `posedge __rst`, an expression (`!rst_ni` by default); the flop is reset from a signal assigned the expression instead |
| `hw/ip/tlul/rtl/tlul_assert.sv` | the flop of `disable_sva` has an asynchronous reset and no `else`; it gets one that keeps the value |
| `hw/top_earlgrey/ip_autogen/pwrmgr/dv/sva/pwrmgr_sec_cm_checker_assert.sv` | the asynchronous reset of `clk_esc_dead_cnt` is tested together with two synchronous conditions; it gets a branch of its own |
| `hw/top_darjeeling/ip_autogen/pwrmgr/dv/sva/pwrmgr_sec_cm_checker_assert.sv` | the asynchronous reset of `clk_esc_dead_cnt` is tested together with two synchronous conditions; it gets a branch of its own |

## Summary

| Result | Targets |
| --- | ---: |
| PASS | 59 |
| PARTIAL | 6 |
| LOWERING_FAIL | 59 |
| LOWERING_TIMEOUT | 0 |
| SETUP_FAIL | 1 |
| SETUP_TIMEOUT | 0 |

De-duplicated source-level property candidates in attempted EDAM file lists: **5967**.

This source count includes transitive file-list candidates and is not presented as an elaborated-instance denominator. Emitted cells and diagnostics below are elaboration results.

Emitted formal cells: **5347** `$check`, **0** legacy `$live`, **0** `$fair`.

Obligations by `$check` flavor: **4914** assert, **351** assume, **36** cover, **44** live, **2** fair.

Unsupported-SVA diagnostics: **793**.

Unique unsupported source forms: **35**.

Property sites dropped, over all configurations: **26**.

## Conclusions

- **65/125** configurations (52.0%) completed elaboration and lowering; 59 were clean and 6 were partial.
- The completed configurations emitted **44** liveness obligations. This is direct evidence that OpenTitan liveness properties now reach Yosys, but it is not a proof result.
- **35** unique unsupported source forms produced **793** configuration/instance diagnostics. Repetition is expected because common TL-UL, Ibex, and OTBN sources elaborate in many targets and generated instances.
- **60** configurations remain blocked before a complete property inventory. Their exact root diagnostics are listed below; they must not be counted as property passes or failures.
- **22** stale config core identifiers were mapped to an unambiguous same-component core in the pinned source tree. Every such resolution is retained in `results.json`.

## Blocked configuration roots

| First blocking diagnostic | Configurations |
| --- | ---: |
| unsupported system task 'next' | 42 |
| assignment pattern target type cannot be deduced in this context | 2 |
| unknown module 'prim_alert_rxtx_tb' | 2 |
| unknown module 'prim_alert_rxtx_async_tb' | 2 |
| unknown module 'i_nopass1' | 2 |
| multiple asynchronous loads unsupported | 2 |
| use of undeclared identifier 'ScrmblKeyNotAllZero_A0' | 2 |
| 'pinmux_tb' is not a valid top-level module | 2 |
| no member named 'tdi_idx' in 'target_cfg_t' | 2 |
| could not resolve hierarchical path name 'u_reg_top_prim' | 1 |
| 'lowrisc:dv:top_darjeeling_flash_ctrl_sva' or any of its dependencies requires 'top_darjeeling_flash_ctrl_sva', but this core was not found | 1 |

## Unsupported SVA source forms

| Source | Property | Diagnostic | Affected configurations |
| --- | --- | --- | ---: |
| `hw/ip/keymgr/rtl/keymgr_ctrl.sv:895` | `SecCmCFILinear_A` | SVA match items and local-variable actions are not synthesizable | 4 |
| `hw/ip/keymgr/rtl/keymgr_ctrl.sv:895` | `SecCmCFILinear_A` | unsupported assertion statement | 4 |
| `hw/ip/lc_ctrl/rtl/lc_ctrl_fsm.sv:891` | `SecCmCFILinear_A` | SVA match items and local-variable actions are not synthesizable | 4 |
| `hw/ip/lc_ctrl/rtl/lc_ctrl_fsm.sv:891` | `SecCmCFILinear_A` | unsupported assertion statement | 4 |
| `hw/ip/otbn/rtl/otbn.sv:1278` | `SecWipeChangedBaseRegs_A` | weak sequence with an unbounded match is unsupported under `not`, property `or`, `iff`, `until`, `nexttime` or an eventuality, or with sequence `and`, `intersect`, `within` or `first_match` in it; in `assert` and `assume` a sequence used as a property is weak unless written `strong(...)` | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1320` | `SecWipeChangedWideRegs_A` | weak sequence with an unbounded match is unsupported under `not`, property `or`, `iff`, `until`, `nexttime` or an eventuality, or with sequence `and`, `intersect`, `within` or `first_match` in it; in `assert` and `assume` a sequence used as a property is weak unless written `strong(...)` | 2 |
| `hw/ip/otbn/rtl/otbn_decoder.sv:1184` | `IbexRegImmAluOpBaseKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_base.sv:243` | `OtbnRfBaseRdAKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_base.sv:244` | `OtbnRfBaseRdBKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_bignum.sv:191` | `OtbnRfBignumRdAKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_bignum.sv:192` | `OtbnRfBignumRdBKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:174` | `AlertPingOk_A` | encountered unsupported SVA feature | 2 |
| `hw/ip/prim/rtl/prim_rom_adv.sv:46` | `noXOnCsI` | unsupported system task '$isunknown' | 3 |
| `hw/ip/prim_generic/rtl/prim_rom.sv:50` | `noXOnCsI` | unsupported system task '$isunknown' | 3 |
| `hw/ip/rom_ctrl/rtl/rom_ctrl_fsm.sv:248` | `SecCmCFILinear_A` | SVA match items and local-variable actions are not synthesizable | 5 |
| `hw/ip/rom_ctrl/rtl/rom_ctrl_fsm.sv:248` | `SecCmCFILinear_A` | unsupported assertion statement | 5 |
| `hw/ip/tlul/rtl/tlul_adapter_sram.sv:684` | `TlOutKnownIfFifoKnown_A` | unsupported system task '$isunknown' | 9 |
| `hw/ip/tlul/rtl/tlul_assert.sv:283` | `aDataKnown_A` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:301` | `dDataKnown_M` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:315` | `aDataKnown_M` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:336` | `dDataKnown_A` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:463` | `b2bReqWithSameAddr` | SVA match items and local-variable actions are not synthesizable | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:463` | `b2bReqWithSameAddr` | unsupported assertion statement | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:465` | `b2bSameSource` | cover of a property with `always`, `until`, an unbounded eventuality or a sequence with an unbounded match is unsupported | 35 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:832` | `IbexInstrLSBsKnown` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:834` | `IbexC0Known1` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:836` | `IbexC1Known1` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:838` | `IbexC1Known2` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:840` | `IbexC1Known3` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_compressed_decoder.sv:843` | `IbexC2Known1` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_decoder.sv:1210` | `IbexRegImmAluOpKnown` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_if_stage.sv:834` | `IbexInstrAddrUnknown` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_load_store_unit.sv:610` | `IbexDataTypeKnown` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_load_store_unit.sv:611` | `IbexDataOffsetKnown` | unsupported system task '$isunknown' | 2 |
| `hw/vendor/lowrisc_ibex/rtl/ibex_load_store_unit.sv:619` | `IbexDataAddrUnknown` | unsupported system task '$isunknown' | 2 |

## Target results

| Configuration | Variant | Result | Checks | Assert | Assume | Cover | Live | Fair | Unsupported | Reason |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `pinmux_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | assignment pattern target type cannot be deduced in this context |
| `pinmux_chip_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | assignment pattern target type cannot be deduced in this context |
| `rv_plic_fpv` | earlgrey | PARTIAL | 289 | 266 | 10 | 8 | 5 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `alert_handler_esc_timer_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unsupported system task 'next' |
| `alert_handler_ping_timer_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unsupported system task 'next' |
| `prim_alert_rxtx_fpv` | earlgrey | PASS | 69 | 60 | 5 | 0 | 4 | 0 | 0 | all elaborated properties lowered |
| `prim_alert_rxtx_fatal_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_tb' |
| `prim_alert_rxtx_async_fpv` | earlgrey | PARTIAL | 85 | 76 | 5 | 0 | 4 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
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
| `prim_packer_fpv` | earlgrey | PASS | 169 | 102 | 67 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
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
| `aes_masked_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `aes_unmasked_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `alert_handler_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `clkmgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | multiple asynchronous loads unsupported |
| `csrng_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `edn_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `entropy_src_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `flash_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | could not resolve hierarchical path name 'u_reg_top_prim' |
| `keymgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 21 | unsupported system task 'next' |
| `kmac_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 23 | unsupported system task 'next' |
| `lc_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 21 | unsupported system task 'next' |
| `otp_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | use of undeclared identifier 'ScrmblKeyNotAllZero_A0' |
| `otbn_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 30 | unsupported system task 'next' |
| `pwrmgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `rom_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 25 | unsupported system task 'next' |
| `rstmgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `rv_core_ibex_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `sram_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 21 | unsupported system task 'next' |
| `pinmux_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'pinmux_tb' is not a valid top-level module |
| `rv_plic_sec_cm` | earlgrey | PARTIAL | 281 | 254 | 14 | 8 | 5 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm_fsm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 2 | unsupported system task 'next' |
| `rom_ctrl_sec_cm_fsm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 2 | unsupported system task 'next' |
| `keymgr_sec_cm_fsm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 2 | unsupported system task 'next' |
| `pwrmgr_esc_unstoppable` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `pinmux_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | no member named 'tdi_idx' in 'target_cfg_t' |
| `pinmux_chip_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | no member named 'tdi_idx' in 'target_cfg_t' |
| `rv_plic_fpv` | darjeeling | PARTIAL | 236 | 213 | 10 | 8 | 5 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `alert_handler_esc_timer_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unsupported system task 'next' |
| `alert_handler_ping_timer_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unsupported system task 'next' |
| `prim_alert_rxtx_fpv` | darjeeling | PASS | 69 | 60 | 5 | 0 | 4 | 0 | 0 | all elaborated properties lowered |
| `prim_alert_rxtx_fatal_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_tb' |
| `prim_alert_rxtx_async_fpv` | darjeeling | PARTIAL | 73 | 64 | 5 | 0 | 4 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
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
| `prim_packer_fpv` | darjeeling | PASS | 169 | 102 | 67 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
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
| `aes_masked_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `aes_unmasked_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `alert_handler_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `clkmgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | multiple asynchronous loads unsupported |
| `csrng_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `edn_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `flash_ctrl_sec_cm` | darjeeling | SETUP_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'lowrisc:dv:top_darjeeling_flash_ctrl_sva' or any of its dependencies requires 'top_darjeeling_flash_ctrl_sva', but this core was not found |
| `keymgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 21 | unsupported system task 'next' |
| `kmac_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 23 | unsupported system task 'next' |
| `lc_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 21 | unsupported system task 'next' |
| `otp_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | use of undeclared identifier 'ScrmblKeyNotAllZero_A0' |
| `otbn_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 30 | unsupported system task 'next' |
| `pwrmgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `rom_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 25 | unsupported system task 'next' |
| `rstmgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `rv_core_ibex_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `sram_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 21 | unsupported system task 'next' |
| `pinmux_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'pinmux_tb' is not a valid top-level module |
| `rv_plic_sec_cm` | darjeeling | PARTIAL | 228 | 201 | 14 | 8 | 5 | 0 | 19 | 19 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm_fsm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 2 | unsupported system task 'next' |
| `rom_ctrl_sec_cm_fsm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 2 | unsupported system task 'next' |
| `keymgr_sec_cm_fsm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 2 | unsupported system task 'next' |
| `pwrmgr_esc_unstoppable` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 19 | unsupported system task 'next' |
| `rv_timer_fpv` | standalone | PASS | 51 | 50 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `rom_ctrl_fpv` | standalone | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 6 | unsupported system task 'next' |

Machine-readable detail is in `results.json`, `targets.csv`, and `unsupported.csv`. Per-target scripts, filtered sources and complete logs are retained under the campaign build directory recorded in `results.json`.
