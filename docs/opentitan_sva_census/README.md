# OpenTitan SVA census

This directory records a source-traceable compatibility census of every formal
configuration in a pinned OpenTitan revision. The campaign tests whether the
real property sources elaborate and lower through sv-elab into Yosys formal
cells. It does not claim that each property has been mathematically proved. A
companion BMC runner looks for counterexamples to what lowered.

`REPORT.md`, `targets.csv` and `unsupported.csv` are the result of the last
census of this branch. `results.json`, which has every cell and diagnostic, is
left out of the repository.

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
  fusesoc==2.4.5 pyyaml hjson==3.1.0 mako==1.3.10 semantic_version==2.10.0 \
  mistletoe==0.9.0 tabulate pycryptodome peakrdl-systemrdl
```

Run all 125 configurations:

```sh
PATH=/workspace/.tools/opentitan-census-venv/bin:$PATH \
  python3 tools/opentitan_sva_census.py \
  --build-root build/opentitan_sva_census \
  --output docs/opentitan_sva_census \
  --jobs 4 --setup-timeout 900 --lowering-timeout 3600
```

Without `--opentitan`, the runner checks the revision of
`tests/opentitan_sva_stress/common/fetch_opentitan.sh` out into
`tests/third_party/opentitan`, as the stress tests do. `--plugin` defaults to
`build/slang.so`, `--target` selects configurations by name or by target id, and
`--report-from results.json` writes the report files again without rerunning
anything. Yosys is the `yosys` on `PATH`.

Per-target EDAM, generated Yosys scripts, filtered sources, JSON netlists, and
complete logs stay under the build root. `results.json` is the machine-readable
aggregate; `targets.csv` is the target matrix; `unsupported.csv` has a row for
each property an unsupported diagnostic drops, with source attribution.

The script enables `FPV_ON` (and, for a configuration with a task,
`FPV_SEC_CM_ON` and `FPV_ALERT_NO_SIGINT_ERR`), disables the implicit `SYNTHESIS`
define, ignores simulation-only initial blocks, elaborates bind modules as
additional tops, and uses OpenTitan's FuseSoC `formal` file lists. It records
commercial-flow `stopats`, `after_load`, tasks, and SecCm blackbox declarations,
but does not claim to reproduce those proof abstractions. The plugin runs with
no options beyond the frontend flags in the generated `lower.ys`.

## Dropping unsupported properties

sv-elab rejects each SVA construct it can't lower with an error. The census
reads every error of a run (`--error-limit=0`) and attributes it to the
assertion statement or assertion macro invocation it is in: the statement
around it, or, for an error in a `sequence` or `property` declaration, every
statement that uses the declaration, directly or through other ones. Macros
the sources define around assertion macros, such as `TLUL_COVER` in
`tlul_assert.sv`, are expanded one level for that lookup.

An error that is an unsupported-SVA diagnostic, or an unsupported system task
in an assertion, drops the statements it is attributed to. They are blanked in
a copy of their file under `targets/<id>/filtered/` (a null item takes their
place, line numbers stay), and the target is elaborated again, until no such
error is left. Every dropped property is listed in `unsupported.csv` and in
`REPORT.md`; the census never passes a property it didn't lower.

Any other error, an unsupported-SVA error the census can't attribute to a
statement, or an error in an included file, blocks the target: it is a
`LOWERING_FAIL`, with the first such error as its reason.

### Source filters

The census reads a hard-linked copy of the OpenTitan tree, under
`<build-root>/opentitan`, in which a few files are rewritten. Each rewrite keeps
what the source does and its line count, and is listed with its reason in
`REPORT.md` and `results.json`:

| Source | Rewrite | Why |
| --- | --- | --- |
| `hw/ip/prim/rtl/prim_assert.sv` | `ASSERT_FPV_LINEAR_FSM` resets its flop from a signal assigned `__rst` | the macro clocks a flop on `posedge __rst`, an expression (`!rst_ni` by default), which an edge can't be taken of |
| `hw/ip/tlul/rtl/tlul_assert.sv` | the flop of `disable_sva` gets an `else` that keeps its value | an asynchronous reset with no `else` |
| `pwrmgr_sec_cm_checker_assert.sv` of `top_earlgrey` and `top_darjeeling` | the asynchronous reset of `clk_esc_dead_cnt` gets a branch of its own | it is tested together with two synchronous conditions |

A filter whose anchor text is missing stops the census, so a new OpenTitan
revision can't silently skip one.

## Result meanings

- `PASS`: all properties that elaborated in the target lowered to formal cells.
- `PARTIAL`: lowering completed once the listed unsupported properties had been
  dropped.
- `LOWERING_FAIL`: a non-SVA frontend, RTL, bind, or configuration error blocked
  target elaboration.
- `LOWERING_TIMEOUT`: elaboration ran past `--lowering-timeout`.
- `SETUP_FAIL`, `SETUP_TIMEOUT`: FuseSoC could not produce an EDAM file, or ran
  past `--setup-timeout`.
- `HARNESS_FAIL`: the runner itself failed on the target.

The runner returns nonzero for setup or harness failures but still writes a
complete report.

### Undriven variables

OpenTitan's FPV benches leave some variables undriven so that the formal tool
picks their values, such as the selectors of a symbolic index. sv-elab gives a
variable no process or assignment drives its simulation value: x for a 4-state
type, 0 for a 2-state one such as `int` or `bit`. The BMC flow below keeps x
bits through its optimisations (`-keepdc`) and makes them free inputs
(`setundef -anyseq`), but a 2-state variable stays 0. JasperGold makes both
free. BMC runs in the default mode with a test-only frontend change that made
them free gave, where the results differ (the alert handler targets come from
a census that let the `next()` gap below through):

| Target | Undriven variables free | Simulation values |
| --- | --- | --- |
| `alert_handler_esc_timer_fpv` | 1 failing assertion | 2 failing assertions |
| `alert_handler_ping_timer_fpv` | BMC_PASS | BMC_PASS VACUOUS |
| `prim_count_max_reset_fpv`, `prim_count_zero_reset_fpv` | BMC_PASS | 14 failing assertions |

## BMC

`tools/opentitan_sva_bmc.py` runs bounded model checking on the targets a census
lowered:

```sh
python3 tools/opentitan_sva_bmc.py \
  --results docs/opentitan_sva_census/results.json \
  --depth 20 --jobs 3 [--clk2fflogic --jasper-env --task-only] [--target NAME]
```

It reads each target again with the census's `lower.ys` and filtered sources,
from the census build root (the one `results.json` records unless
`--census-build` is given), and with the census plugin unless `--plugin` or
`PLUGIN` names another. `YOSYS`, `YOSYS_ABC` and `YOSYS_WITNESS` name the tools.
Its output, `bmc.json` and a directory per target with the scripts, model and
logs, goes to `<build-root>/bmc` or `--out`.

The model is the target after `prep -flatten`, with liveness, fairness and cover
checks removed and the rest lowered by `chformal -lower`; undefined values and
undriven wires become free inputs. ABC runs `fold; strash; bmc3 -a`. A bench
module bound into the top holds every reset active on the first step and leaves
it free after that, and carries the canary `fv_canary`, which fails once all
resets are released.

Modes:

- default: `async2sync` and `formalff -clk2ff`; one frame is one cycle of the
  clock. Designs with flops on both edges of a clock (the generated CSR
  assertions, which sample on the falling edge, in `rv_plic` and every
  `sec_cm` target) or with memories need `--clk2fflogic`.
- `--clk2fflogic`: `clk2fflogic` models every clock and asynchronous flop on the
  global clock; one frame is one step of it, a clock edge every frame at most.
  Resets then change only where the clock falls, as the sampled `disable iff`
  needs (see `docs/sva.md`).
- `--jasper-env`: the clocks, clock factors, input rates and reset expressions
  of the target in `hw/formal/tools/jaspergold/fpv.tcl`, and each reset held
  released once released, as JasperGold's `reset -expr` does. With
  `--clk2fflogic`, the clocks toggle with their factors and each input changes
  only on a rising edge of the clock of its rate.
- `--task-only`: for a configuration with a task (the `sec_cm` ones), only the
  assertions named after the task, which are the ones JasperGold proves.
- `--trace`: replays a counterexample to the first failing assertion on the
  model (`write_cex`, `yosys-witness aiw2yw`, `sim -r`) into `cex.vcd`;
  `--trace-signals ping_req_i,i_prim_alert_sender.state_q` also tabulates
  those signals into `cex.txt`, one column per VCD time step (a name with a
  dot matches the end of any path).

Results, with each failing assertion followed by `@` and the ABC frame it fails
in (frame 0 is the step with resets held):

- `BMC_PASS`: no assertion fails within `--depth` frames.
- `BMC_FAIL`: the listed assertions fail; `(to depth N)` if ABC stopped at
  frame N before `--depth` with other assertions still open.
- `BMC_TIMEOUT (to depth N)`: no failure within the N frames ABC reached before
  `--timeout`.
- `VACUOUS`, after a pass or failure: the canary didn't fail, so no trace has
  all resets released; the assumptions, not the design, hold the checks.
- `ABC_CRASH`: ABC failed. `bmc3 -a` has been seen to crash after its first
  failing output; the runner then falls back to plain `bmc3`, tying each
  failing output off and running again, and reports `abc_fallback`.
- `PREP_FAIL`, `PREP_TIMEOUT`: Yosys couldn't build the model, or ran past
  `--timeout`; the reason is the last Yosys error.
- `NO_CHECKS`: the model has no assertion.

A failure is a counterexample to a lowered property under these assumptions,
not a bug in OpenTitan: the benches are written for JasperGold, whose stopats,
blackboxes and other abstractions the runner doesn't apply.

## Known failures

| Target | Result | Cause |
| --- | --- | --- |
| targets with a sparse FSM (`alert_handler_esc_timer_fpv`, `alert_handler_ping_timer_fpv`, most `sec_cm`) | LOWERING_FAIL | `prim_sparse_fsm_flop` calls the enum method `next()` in `is_undefined_state`, which sv-elab rejects (`unsupported system task 'next'`) |
| `clkmgr_sec_cm` | LOWERING_FAIL | under `FPV_ON`, `prim_reg_cdc_arb` has a flop with two asynchronous loads, a reset and `posedge src_update_o` (`multiple asynchronous loads unsupported`) |
| `prim_alert_rxtx_async_fpv` | PARTIAL | `AlertPingOk_A` is dropped: its disable condition has `$changed(ping_skew_i, @clk_i)`, a sampled value function with a clock of its own (`encountered unsupported SVA feature`) |
| `pinmux_*`, `flash_ctrl_sec_cm`, `otp_ctrl_sec_cm`, `prim_alert_rxtx*_fatal_fpv` | LOWERING_FAIL | frontend and configuration errors outside the properties, such as undeclared identifiers and unresolved hierarchical paths in this revision's benches; see `REPORT.md` |
| `prim_fifo_sync_fpv` | LOWERING_FAIL | the bind module binds into instance names of the bench (`bind i_nopass1 ...`), which aren't visible from the bind module, a top of its own |
| `prim_alert_rxtx_fpv`, `prim_alert_rxtx_async_fpv` | BMC_PASS on fewer traces | `PingEn_M` lacks parentheses: `throughout` binds looser than `##1`, so the last step of its sequence needs both `ping_req_i` and `$fell(ping_req_i)`, and it never matches. As an assumption it fails, and so ends the trace, one step after a ping raised out of reset is answered (`ping_ok_o` or `error_present`). With the parentheses it matches, and the 61 assertions of `prim_alert_rxtx_fpv` still pass |
| `alert_handler_esc_timer_fpv` | BMC_FAIL | `CrashdumpTrigger_A` is disabled when `esc_state_o == FsmError`, a register output; with the disable condition sampled (see `docs/sva.md`), the step the FSM enters `FsmError` isn't disabled |
| `prim_packer_fpv`, `prim_lfsr_fpv` | BMC_FAIL | multiple drivers: `prim_packer_tb` ties the outputs of its 17 instances together, and `prim_lfsr` has conflicting drivers on `lfsr_q` of its 64- and 128-bit Fibonacci XNOR instances |
| `rv_plic_fpv`, `rv_plic_sec_cm` | PREP_FAIL | the `always_comb` of `rv_plic_assert_fpv` assigns `i_high_prio` only when a source wins, a latch, which is lowered into a combinational loop (`--clk2fflogic`; without it the negedge flops of the CSR assertions stop the model first) |
| `otbn_sec_cm` | PREP_TIMEOUT | `prep -flatten` doesn't finish within the timeout |
| `pwrmgr_sec_cm`, `pwrmgr_esc_unstoppable` | BMC_FAIL | `fpv.tcl` doesn't declare the escalation clock and reset (`clk_esc_i`, `rst_esc_ni`) of `pwrmgr`; with `--jasper-env` they are inputs like any other |
| `sec_cm` targets without `--task-only` | BMC_FAIL | the checks of other tasks fail under the `FPV_SEC_CM_ON` bench, which JasperGold doesn't prove for the task |

Three assertions of `otbn.sv`, `ImemSecWipeRequiresOtpKey_A`,
`DmemSecWipeRequiresOtpKey_A` and `BusyOrLockingFromStartToDone_A`, end in
`##[0:$] x` with no `strong(...)`. A weak sequence fails only where no match is
left to come, so they hold on any trace, and sv-elab warns that they never
fail. They are in `otbn_sec_cm`, which stops at LOWERING_FAIL first.

The BMC rows of `alert_handler_esc_timer_fpv`, `otbn_sec_cm`, `pwrmgr` and the
`sec_cm` tasks come from runs on a census that let the `next()` error through;
with this census, those targets stop at LOWERING_FAIL first.

The frontend gaps behind the `next()` failures, behind the four source filters
(an asynchronous reset with no `else`, tested together with other conditions,
or taken as an edge of an expression) and behind hierarchical enum literals in
`inside` sets have fixes on the unpublished July branch
(`feature/opentitan-sva-census`) that were never ported.
