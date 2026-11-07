# OpenTitan SVA compatibility census

Generated: `2026-11-07T08:41:57.914008+00:00`

OpenTitan revision: `98a020b74d02c316fc2cc682bb212214b5c27e0e`

sv-elab revision: `bfe5d70eaa4b9b9d9766c17b106e5482cae59717`

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
| PASS | 49 |
| PARTIAL | 16 |
| LOWERING_FAIL | 59 |
| LOWERING_TIMEOUT | 0 |
| SETUP_FAIL | 1 |
| SETUP_TIMEOUT | 0 |

De-duplicated source-level property candidates in attempted EDAM file lists: **5967**.

This source count includes transitive file-list candidates and is not presented as an elaborated-instance denominator. Emitted cells and diagnostics below are elaboration results.

Emitted formal cells: **5253** `$check`, **0** legacy `$live`, **0** `$fair`.

Obligations by `$check` flavor: **4870** assert, **347** assume, **36** cover, **0** live, **0** fair.

Unsupported-SVA diagnostics: **1496**.

Unique unsupported source forms: **89**.

Property sites dropped, over all configurations: **88**.

## Conclusions

- **65/125** configurations (52.0%) completed elaboration and lowering; 49 were clean and 16 were partial.
- The completed configurations emitted no liveness obligations.
- **89** unique unsupported source forms produced **1496** configuration/instance diagnostics. Repetition is expected because common TL-UL, Ibex, and OTBN sources elaborate in many targets and generated instances.
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
| `generated/generator_cache/lowrisc_darjeeling_dv_alert_handler_sva-csr_assert_gen_0.1-c02685d4f9360bfbe0eda9672f9a342247d4685b96aa76f2ad5785d2e92e01ee/alert_handler_csr_assert_fpv.sv:1185` | `TlulOOBAddrErr_A` | unsupported assertion statement | 1 |
| `generated/generator_cache/lowrisc_darjeeling_dv_clkmgr_sva-csr_assert_gen_0.1-96e3ebbbf4827733392a911af1214afa8d2aab9cfe80fac478ceec76eb6d0240/clkmgr_csr_assert_fpv.sv:239` | `TlulOOBAddrErr_A` | unsupported assertion statement | 1 |
| `generated/generator_cache/lowrisc_darjeeling_dv_pwrmgr_sva-csr_assert_gen_0.1-c2485cf546b6abd872916db83397b970d63c6e5a3dc11547f15a7e956b28472f/pwrmgr_csr_assert_fpv.sv:266` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_darjeeling_dv_rstmgr_sva-csr_assert_gen_0.1-6ae800502b7149e2cf0e09245b9c1ea5d29d62399ab3e1d5de92d386e9d1faeb/rstmgr_csr_assert_fpv.sv:283` | `TlulOOBAddrErr_A` | unsupported assertion statement | 1 |
| `generated/generator_cache/lowrisc_darjeeling_ip_rv_plic_fpv-csr_assert_gen_0.1-2ae7e88d6cbe4cad70d72d841e443d9e1180cc2a4b72a09f5ef066dc1263ef24/rv_plic_csr_assert_fpv.sv:1724` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_aes_sva-csr_assert_gen_0.1-de1df75cf4d10b8ff8eadb3b2a7177b82848423181895983ac642e4111059eb9/aes_csr_assert_fpv.sv:206` | `TlulOOBAddrErr_A` | unsupported assertion statement | 4 |
| `generated/generator_cache/lowrisc_dv_csrng_sva-csr_assert_gen_0.1-e450b0cc78762bb3ef9a3d1137077b952651946ccf836a6c50d46a720d182f45/csrng_csr_assert_fpv.sv:304` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_edn_sva-csr_assert_gen_0.1-3d387bb0e518b1118852a68f330120acb02137bd0daf464c8d791c909b9efe25/edn_csr_assert_fpv.sv:292` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_entropy_src_sva-csr_assert_gen_0.1-aba5d23d69c1871540d6ec58d36c416bbe9d87d34b6f8c42b396b3a53817d7ee/entropy_src_csr_assert_fpv.sv:342` | `TlulOOBAddrErr_A` | unsupported assertion statement | 1 |
| `generated/generator_cache/lowrisc_dv_keymgr_sva-csr_assert_gen_0.1-2c26552f13e6786f6943f27be941d3e6c1c29dbdc9c01f020b2af70c107ba6ad/keymgr_csr_assert_fpv.sv:541` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_kmac_sva-csr_assert_gen_0.1-db5440173c58f44fec33a569a5ea0e2194abc6e90893e319ed240d5e3f191994/kmac_csr_assert_fpv.sv:520` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_lc_ctrl_sva-csr_assert_gen_0.1-b0d2bf878fdced60a69b4845d77dec3483be5b449ce342b4a4a336673a5f2c7d/lc_ctrl_regs_csr_assert_fpv.sv:211` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_otbn_sva-csr_assert_gen_0.1-b9e827d73b9a7c8f32cc71023d23a3a01e04690fadc7d9937a84c6783f8a6707/otbn_csr_assert_fpv.sv:218` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_rom_ctrl_sva-csr_assert_gen_0.1-7c5016bfb51678e89e8e027d12fab8032e5722d8937dd46f977f96b0f4261545/rom_ctrl_regs_csr_assert_fpv.sv:195` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_dv_sram_ctrl_sva-csr_assert_gen_0.1-5be0c97a84452472996c5e05757cee78394d9edd5ab6ec95fa32171dd7823f4b/sram_ctrl_regs_csr_assert_fpv.sv:255` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_earlgrey_dv_alert_handler_sva-csr_assert_gen_0.1-7bb878a2ee0c580b1bae276c162cc4a6897feda2d579b9a02eb90083c50995a0/alert_handler_csr_assert_fpv.sv:1031` | `TlulOOBAddrErr_A` | unsupported assertion statement | 1 |
| `generated/generator_cache/lowrisc_earlgrey_dv_clkmgr_sva-csr_assert_gen_0.1-6930c32e0fb697246dc1514d3c52a5215f8abbbadb0267d053f3f0bceb17a524/clkmgr_csr_assert_fpv.sv:261` | `TlulOOBAddrErr_A` | unsupported assertion statement | 1 |
| `generated/generator_cache/lowrisc_earlgrey_dv_pwrmgr_sva-csr_assert_gen_0.1-52b3c5eb47515c1fa842e733d8b27613aa0758d34f455a2d583f98ef8d479020/pwrmgr_csr_assert_fpv.sv:266` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `generated/generator_cache/lowrisc_earlgrey_dv_rstmgr_sva-csr_assert_gen_0.1-450d701edc1589478ce8e533bb6c141b151261ab5cb020107ca15be139838125/rstmgr_csr_assert_fpv.sv:393` | `TlulOOBAddrErr_A` | unsupported assertion statement | 1 |
| `generated/generator_cache/lowrisc_earlgrey_ip_rv_plic_fpv-csr_assert_gen_0.1-0661c1961c53b0d795200738e06c061c051e431420d13ad0ae20a9f345ef2b4b/rv_plic_csr_assert_fpv.sv:2307` | `TlulOOBAddrErr_A` | unsupported assertion statement | 2 |
| `hw/ip/edn/rtl/edn.sv:125` | `EdnDataStable_A` | unsupported assertion statement | 2 |
| `hw/ip/keymgr/rtl/keymgr_ctrl.sv:895` | `SecCmCFILinear_A` | unsupported assertion statement | 4 |
| `hw/ip/kmac/rtl/sha3pad.sv:859` | `PartialEndOfMsg_M` | unsupported assertion statement | 3 |
| `hw/ip/lc_ctrl/rtl/lc_ctrl_fsm.sv:891` | `SecCmCFILinear_A` | unsupported assertion statement | 4 |
| `hw/ip/otbn/rtl/otbn.sv:1278` | `SecWipeChangedBaseRegs_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1320` | `SecWipeChangedWideRegs_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1393` | `ImemSecWipeRequiresOtpKey_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1399` | `DmemSecWipeRequiresOtpKey_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn.sv:1435` | `BusyOrLockingFromStartToDone_A` | unsupported assertion statement | 2 |
| `hw/ip/otbn/rtl/otbn_decoder.sv:1184` | `IbexRegImmAluOpBaseKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_base.sv:243` | `OtbnRfBaseRdAKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_base.sv:244` | `OtbnRfBaseRdBKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_bignum.sv:191` | `OtbnRfBignumRdAKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/otbn/rtl/otbn_rf_bignum.sv:192` | `OtbnRfBignumRdBKnown` | unsupported system task '$isunknown' | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:108` | `AlertPingIgnored_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:120` | `AlertCheck1_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:128` | `FsmLivenessSender_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:134` | `FsmLivenessReceiver_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:55` | `PingEn_M` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_assert_fpv.sv:94` | `AlertReqAck_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:150` | `AlertReqAck_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:174` | `AlertPingOk_A` | encountered unsupported SVA feature | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:181` | `AlertPingIgnored_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:204` | `AlertCheck1_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:210` | `FsmLivenessSender_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:213` | `FsmLivenessReceiver_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_alert_rxtx_async_assert_fpv.sv:94` | `PingEn_M` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_esc_rxtx_assert_fpv.sv:128` | `FsmLivenessSender_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_esc_rxtx_assert_fpv.sv:135` | `FsmLivenessReceiver_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_esc_rxtx_assert_fpv.sv:166` | `AutoEscalation1_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/fpv/vip/prim_esc_rxtx_assert_fpv.sv:79` | `FiniteEsc_M` | unsupported assertion statement | 2 |
| `hw/ip/prim/rtl/prim_alert_receiver.sv:361` | `InBandInitSequence_A` | unsupported assertion statement | 6 |
| `hw/ip/prim/rtl/prim_arbiter_ppc.sv:183` | `NoStarvation_A` | unsupported assertion statement | 6 |
| `hw/ip/prim/rtl/prim_arbiter_tree.sv:264` | `NoStarvation_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/rtl/prim_packer.sv:325` | `FlushFollowedByDone_A` | unsupported assertion statement | 4 |
| `hw/ip/prim/rtl/prim_reg_cdc_arb.sv:278` | `UpdateTimeout_A` | unsupported assertion statement | 2 |
| `hw/ip/prim/rtl/prim_rom_adv.sv:46` | `noXOnCsI` | unsupported system task '$isunknown' | 3 |
| `hw/ip/prim/rtl/prim_sync_reqack.sv:393` | `SyncReqAckRstSrc` | unsupported assertion statement | 2 |
| `hw/ip/prim/rtl/prim_sync_reqack.sv:396` | `SyncReqAckRstDst` | unsupported assertion statement | 2 |
| `hw/ip/prim_generic/rtl/prim_rom.sv:50` | `noXOnCsI` | unsupported system task '$isunknown' | 3 |
| `hw/ip/rom_ctrl/rtl/rom_ctrl_fsm.sv:248` | `SecCmCFILinear_A` | unsupported assertion statement | 5 |
| `hw/ip/tlul/rtl/tlul_adapter_sram.sv:684` | `TlOutKnownIfFifoKnown_A` | unsupported system task '$isunknown' | 9 |
| `hw/ip/tlul/rtl/tlul_assert.sv:283` | `aDataKnown_A` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:301` | `dDataKnown_M` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:315` | `aDataKnown_M` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:336` | `dDataKnown_A` | unsupported system task '$isunknown' | 37 |
| `hw/ip/tlul/rtl/tlul_assert.sv:341` | `legalAOpcodeErr_A` | unsupported assertion statement | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:345` | `sizeGTEMaskErr_A` | unsupported assertion statement | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:349` | `sizeMatchesMaskErr_A` | unsupported assertion statement | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:353` | `addrSizeAlignedErr_A` | unsupported assertion statement | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:463` | `b2bReqWithSameAddr` | unsupported assertion statement | 35 |
| `hw/ip/tlul/rtl/tlul_assert.sv:465` | `b2bSameSource` | unsupported assertion statement | 35 |
| `hw/top_darjeeling/ip_autogen/alert_handler/fpv/vip/alert_handler_esc_timer_assert_fpv.sv:97` | `TerminalState_A` | unsupported assertion statement | 1 |
| `hw/top_darjeeling/ip_autogen/pwrmgr/dv/sva/pwrmgr_sec_cm_checker_assert.sv:165` | `EscClkStopEscTimeout_A` | unsupported assertion statement | 2 |
| `hw/top_darjeeling/ip_autogen/pwrmgr/rtl/pwrmgr.sv:365` | `GlitchStatusPersist_A` | unsupported assertion statement | 2 |
| `hw/top_earlgrey/ip_autogen/alert_handler/fpv/vip/alert_handler_esc_timer_assert_fpv.sv:97` | `TerminalState_A` | unsupported assertion statement | 1 |
| `hw/top_earlgrey/ip_autogen/pwrmgr/dv/sva/pwrmgr_sec_cm_checker_assert.sv:165` | `EscClkStopEscTimeout_A` | unsupported assertion statement | 2 |
| `hw/top_earlgrey/ip_autogen/pwrmgr/rtl/pwrmgr.sv:369` | `GlitchStatusPersist_A` | unsupported assertion statement | 2 |
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
| `rv_plic_fpv` | earlgrey | PARTIAL | 284 | 266 | 10 | 8 | 0 | 0 | 26 | 26 unsupported SVA diagnostic(s) |
| `alert_handler_esc_timer_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 1 | unsupported system task 'next' |
| `alert_handler_ping_timer_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unsupported system task 'next' |
| `prim_alert_rxtx_fpv` | earlgrey | PARTIAL | 62 | 58 | 4 | 0 | 0 | 0 | 7 | 7 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_fatal_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_tb' |
| `prim_alert_rxtx_async_fpv` | earlgrey | PARTIAL | 78 | 74 | 4 | 0 | 0 | 0 | 8 | 8 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_async_fatal_fpv` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_async_tb' |
| `prim_arbiter_fixed_fpv` | earlgrey | PASS | 10 | 10 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_arbiter_ppc_fpv` | earlgrey | PARTIAL | 28 | 20 | 8 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `prim_arbiter_tree_fpv` | earlgrey | PARTIAL | 28 | 20 | 8 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `prim_count_max_reset_fpv` | earlgrey | PASS | 15 | 14 | 0 | 1 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_count_zero_reset_fpv` | earlgrey | PASS | 15 | 14 | 0 | 1 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_esc_rxtx_fpv` | earlgrey | PARTIAL | 60 | 57 | 3 | 0 | 0 | 0 | 4 | 4 unsupported SVA diagnostic(s) |
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
| `aes_masked_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 26 | unsupported system task 'next' |
| `aes_unmasked_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 26 | unsupported system task 'next' |
| `alert_handler_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `clkmgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | multiple asynchronous loads unsupported |
| `csrng_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `edn_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `entropy_src_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `flash_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | could not resolve hierarchical path name 'u_reg_top_prim' |
| `keymgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 29 | unsupported system task 'next' |
| `kmac_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 32 | unsupported system task 'next' |
| `lc_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 29 | unsupported system task 'next' |
| `otp_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | use of undeclared identifier 'ScrmblKeyNotAllZero_A0' |
| `otbn_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 226 | unsupported system task 'next' |
| `pwrmgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `rom_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 33 | unsupported system task 'next' |
| `rstmgr_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 26 | unsupported system task 'next' |
| `rv_core_ibex_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `sram_ctrl_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `pinmux_sec_cm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'pinmux_tb' is not a valid top-level module |
| `rv_plic_sec_cm` | earlgrey | PARTIAL | 276 | 254 | 14 | 8 | 0 | 0 | 26 | 26 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm_fsm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 3 | unsupported system task 'next' |
| `rom_ctrl_sec_cm_fsm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 3 | unsupported system task 'next' |
| `keymgr_sec_cm_fsm` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 3 | unsupported system task 'next' |
| `pwrmgr_esc_unstoppable` | earlgrey | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `pinmux_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | no member named 'tdi_idx' in 'target_cfg_t' |
| `pinmux_chip_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | no member named 'tdi_idx' in 'target_cfg_t' |
| `rv_plic_fpv` | darjeeling | PARTIAL | 231 | 213 | 10 | 8 | 0 | 0 | 26 | 26 unsupported SVA diagnostic(s) |
| `alert_handler_esc_timer_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 1 | unsupported system task 'next' |
| `alert_handler_ping_timer_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unsupported system task 'next' |
| `prim_alert_rxtx_fpv` | darjeeling | PARTIAL | 62 | 58 | 4 | 0 | 0 | 0 | 7 | 7 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_fatal_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_tb' |
| `prim_alert_rxtx_async_fpv` | darjeeling | PARTIAL | 66 | 62 | 4 | 0 | 0 | 0 | 8 | 8 unsupported SVA diagnostic(s) |
| `prim_alert_rxtx_async_fatal_fpv` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | unknown module 'prim_alert_rxtx_async_tb' |
| `prim_arbiter_fixed_fpv` | darjeeling | PASS | 10 | 10 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_arbiter_ppc_fpv` | darjeeling | PARTIAL | 28 | 20 | 8 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `prim_arbiter_tree_fpv` | darjeeling | PARTIAL | 28 | 20 | 8 | 0 | 0 | 0 | 1 | 1 unsupported SVA diagnostic(s) |
| `prim_count_max_reset_fpv` | darjeeling | PASS | 14 | 14 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_count_zero_reset_fpv` | darjeeling | PASS | 14 | 14 | 0 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `prim_esc_rxtx_fpv` | darjeeling | PARTIAL | 60 | 57 | 3 | 0 | 0 | 0 | 4 | 4 unsupported SVA diagnostic(s) |
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
| `aes_masked_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 26 | unsupported system task 'next' |
| `aes_unmasked_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 26 | unsupported system task 'next' |
| `alert_handler_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `clkmgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | multiple asynchronous loads unsupported |
| `csrng_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `edn_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `flash_ctrl_sec_cm` | darjeeling | SETUP_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'lowrisc:dv:top_darjeeling_flash_ctrl_sva' or any of its dependencies requires 'top_darjeeling_flash_ctrl_sva', but this core was not found |
| `keymgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 29 | unsupported system task 'next' |
| `kmac_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 32 | unsupported system task 'next' |
| `lc_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 29 | unsupported system task 'next' |
| `otp_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | use of undeclared identifier 'ScrmblKeyNotAllZero_A0' |
| `otbn_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 226 | unsupported system task 'next' |
| `pwrmgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `rom_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 33 | unsupported system task 'next' |
| `rstmgr_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 26 | unsupported system task 'next' |
| `rv_core_ibex_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 27 | unsupported system task 'next' |
| `sram_ctrl_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `pinmux_sec_cm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 'pinmux_tb' is not a valid top-level module |
| `rv_plic_sec_cm` | darjeeling | PARTIAL | 223 | 201 | 14 | 8 | 0 | 0 | 26 | 26 unsupported SVA diagnostic(s) |
| `lc_ctrl_sec_cm_fsm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 3 | unsupported system task 'next' |
| `rom_ctrl_sec_cm_fsm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 3 | unsupported system task 'next' |
| `keymgr_sec_cm_fsm` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 3 | unsupported system task 'next' |
| `pwrmgr_esc_unstoppable` | darjeeling | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 28 | unsupported system task 'next' |
| `rv_timer_fpv` | standalone | PASS | 51 | 50 | 1 | 0 | 0 | 0 | 0 | all elaborated properties lowered |
| `rom_ctrl_fpv` | standalone | LOWERING_FAIL | 0 | 0 | 0 | 0 | 0 | 0 | 7 | unsupported system task 'next' |

Machine-readable detail is in `results.json`, `targets.csv`, and `unsupported.csv`. Per-target scripts, filtered sources and complete logs are retained under the campaign build directory recorded in `results.json`.
