# Synthesizable SVA liveness

yosys-slang lowers temporal properties into explicit finite-state monitor logic
and Yosys formal cells. Finite failures become `$check` cells with
`FLAVOR="assert"` or `"assume"`. Unbounded progress becomes justice:

- an asserted progress obligation becomes `$check` with `FLAVOR="live"`;
- an assumed progress obligation becomes `$check` with `FLAVOR="fair"`;
- an asserted generalized regular sequence may also introduce an auxiliary
  `$fair` cell for exact attempt selection;
- a mixed property such as `s_until` becomes both a safety check and a
  liveness check.

No unbounded operator is replaced with a selected finite depth. Bounded ranges
are enumerated only up to the synthesis guard of 1024 samples; unbounded ranges
use finite-state monitors.

## Supported property forms

The following forms are supported when their operands satisfy the restrictions
in the next section:

| SVA form | Lowering |
| --- | --- |
| `eventually p`, `s_eventually p`, `s_eventually [m:$] p` | response monitor plus `$live` or `$fair` |
| bounded `eventually`, `s_eventually`, `nexttime`, `s_nexttime` | finite safety monitor |
| `always p`, `s_always p`, bounded and unbounded ranges | safety checks active over the requested interval |
| `not (s_eventually p)` / `not (always p)` | dual safety / liveness form |
| `p until q`, `p until_with q` | weak-until safety monitor |
| `p s_until q`, `p s_until_with q` | safety monitor plus progress obligation |
| `strong(sequence)` | finite safety or exact unbounded sequence acceptance |
| `weak(sequence)` | finite sequences only |
| `a |-> property`, `a |=> property` | one consequent attempt per antecedent match |
| `a #=# property`, `a #-# property` | left-side match plus consequent obligation |
| property `and` | union of both obligation sets |
| direct justice `or` | one justice goal over the disjunction |
| simple property `implies` and `iff` | gated property or cycle-local equality |
| property `if` / `else` and `case` | launch-time branch selection |
| `disable iff`, `accept_on`, `reject_on` | abort or rejection logic as described below |
| named sequences and named properties | recursively lowered, except recursion |

Finite sequences retain the existing yosys-slang lowering, including
concatenation delays, bounded ranges, consecutive repetition, sequence
`and`/`or`/`intersect`, `throughout`, implications, followed-by,
`first_match` without actions, and empty-match edge cases.

Unbounded sequence monitors support:

- finite and unbounded concatenation delays;
- bounded and unbounded consecutive repetition (`[*]`);
- nonconsecutive repetition (`[=]`) and goto repetition (`[->]`) of simple
  Boolean sequence expressions;
- sequence `or`;
- `throughout` with a simple Boolean left operand;
- `nexttime` and `s_nexttime`;
- named sequences and `first_match` when there are no match actions.

For asserted generalized regular liveness, high-level sequence `and`,
`intersect`, `within`, and `throughout` are composed from independently
monitored operands. A fresh `$anyseq` bit selects one enabled assertion attempt,
and an auxiliary fairness condition requires some enabled attempt to be
selected. Since the proof must hold for every fair selector trace, every
possible attempt is checked without merging histories from overlapping
attempts.

## Exact temporal behavior

Vacuity and triggering follow the property structure:

- an implication launches a consequent only when its antecedent matches;
- a nonoverlapped implication or followed-by delays the launch by one sampled
  clock;
- no antecedent match means no response obligation;
- a top-level eventual property launches on every enabled sampled clock.

Overlapping triggers are preserved. For an eventual finite suffix of maximum
duration `N`, the monitor keeps pending trigger ages `1..N-1` and a saturated
older bucket. A suffix match of duration `D` can discharge only triggers whose
age is at least `D`. Consequently, a sequence match that began before a request
cannot satisfy that request, while one later match can satisfy multiple older
overlapping requests.

An unbounded minimum delay is exact. For
`s_eventually [m:$] p`, observations before sample `m` cannot complete the
obligation; there is no artificial maximum.

`disable iff` prevents a launch in the disabled sample and clears pending
monitor state. A pending liveness goal is accepted in the disabling sample.
`accept_on` uses the same abort behavior. `reject_on` on a pure liveness
property adds a safety failure if the reject condition occurs while the
obligation is active; a reject after completion does not fail.

Weak until enforces its left operand only while waiting for the right operand
and does not require the right operand to arrive. Strong until adds that
progress requirement. `until_with` includes the completion sample in the
left-operand safety interval.

Monitor state and finite-history enables are initialized explicitly. A finite
check is disabled until every required historical sample exists, including
paths involving empty repetitions.

## Deliberate synthesis limits

The frontend diagnoses constructs for which the current Yosys formal model
cannot preserve the SVA meaning:

- SVA match items, sequence local-variable assignments, and action subroutines;
- recursive named properties or sequences;
- weak sequences with an unbounded match;
- liveness `cover property`, because justice is not a finite reachability
  query;
- generalized regular liveness used as an assumption when it needs prophecy
  selection. Using existential selection for an assumption would weaken the
  environment constraint;
- sequence-language products nested inside a larger unbounded concatenation
  when they would require full automaton product/determinization;
- `always` or `until` operands that are themselves temporal rather than simple
  Boolean properties;
- temporal `iff`, temporal left operands of property-level `implies`, and
  response-style property `or` whose acceptance cannot be represented by one
  justice goal;
- `reject_on` around a property that already mixes safety and liveness
  obligations;
- multiple or incompatible assertion clocks, sampled-value clock arguments,
  and temporal monitors without one edge-triggered procedural clock.

These cases produce an error rather than a bounded approximation or a silently
weakened proof.

## Proof tools

The solver-backed tests use SymbiYosys `mode live` with the `aiger suprove`
engine. Install the pinned runtime:

```sh
tools/setup_liveness_tools.sh
```

The script downloads the OSS CAD Suite Linux x64 release dated 2026-07-27,
verifies SHA-256
`d5cd8fb0276b51ae6b35945e70e03f222d82a863f2e3523b33b937be0ea416f4`,
and extracts `suprove` plus its runtime dependencies. The default install root
is the sibling `.tools` directory and can be changed with
`YOSYS_SLANG_TOOL_ROOT`. The script does not replace the system Yosys or ABC.

Distribution packages do not consistently name the standalone ABC executable.
CTest discovers either `yosys-abc` or `abc` and passes the exact path only to
the equivalence test; no global compatibility symlink is installed.

Configure and run the complete controlled proof set with the path printed by
the installer:

```sh
cmake -S . -B build \
  -DENABLE_LIVENESS_PROOFS=ON \
  -DENABLE_OPENTITAN_SVA_STRESS=ON \
  -DSUPROVE_EXECUTABLE=/absolute/path/to/suprove
cmake --build build
ctest --test-dir build --output-on-failure
```

The liveness proof driver creates an isolated temporary directory and requires
both expected `PASS` results and intentional `FAIL` results. It covers direct
response, minimum delay, fairness, disable/accept/reject behavior, exact suffix
age tracking, overlapping requests, regular-sequence composition, strong-until
safety, and antecedent reachability.

The OpenTitan tests use vendored RTL. The arbiter test proves safety,
reachability, and starvation freedom, then requires a mutated arbiter to fail.
The opt-in stress targets lower and inspect the real alert-handler,
escalation-timer, SHA3 padding, and TL-UL properties, checking their emitted
live/fair structure and running the applicable safety and liveness tasks.

## Inspecting generated obligations

For a small design, the emitted contract can be inspected directly:

```sh
yosys -m build/slang.so -p '
  read_slang design.sv
  prep -top top
  write_rtlil design.il
'
grep -E '\\$check|FLAVOR|sva_(pending|nfa|response|select)' design.il
```

Before relying on a proof, also establish non-vacuity. Prove or cover the
antecedent/request path separately, inspect the generated `live` and `fair`
cells, and retain an intentional broken-design run that the liveness engine
must reject.
