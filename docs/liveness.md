# Synthesizable SVA liveness

sv-elab lowers temporal properties into explicit finite-state monitor logic
and Yosys formal cells. Finite failures become `$check` cells with
`FLAVOR="assert"` or `"assume"`. Unbounded progress becomes justice:

- an asserted progress obligation becomes `$check` with `FLAVOR="live"`;
- an assumed progress obligation becomes `$check` with `FLAVOR="fair"`;
- an asserted generalized regular sequence may also introduce an auxiliary
  `$fair` cell for exact attempt selection;
- a mixed property such as `s_until` becomes both a safety check and a
  liveness check.

The label of a property names its progress obligation, or its first check
when it has none. The other checks of the property are named after the label:
`label$obligation` for a further obligation and `label$select_fair` for the
fairness of a selected attempt.

No unbounded operator is replaced with a selected finite depth. Bounded ranges
are lowered whatever their size, with no limit on the number of samples;
unbounded ranges use finite-state monitors.

## Supported property forms

The following forms are supported where they appear as described in the next
section. `b` and `c` stand for Boolean expressions, `s` for a sequence, `f` for
a finite property made of sequences and their `not`, `and`, `or` and
implications, and `p` and `q` for properties.

| SVA form | Lowering |
| --- | --- |
| `s_eventually f`, `s_eventually [m:$] f` | response monitor plus `$live` or `$fair` |
| `eventually [m:n] f`, `s_eventually [m:n] f`, `nexttime f`, `s_nexttime f`, `nexttime [n] f`, `s_nexttime [n] f` | finite safety monitor |
| `always p`, `always [m:$] p`, `always [m:n] p`, `s_always [m:n] p` | one attempt of `p` per tick of the interval |
| `not (s_eventually b)`, `not (s_eventually [m:$] b)` | safety monitor of the dual `always` |
| `not (always b)`, `not (always [m:$] b)` | liveness of the dual eventuality |
| `b until c`, `b until_with c` | weak-until safety monitor |
| `b s_until c`, `b s_until_with c` | safety monitor plus progress obligation |
| `strong(s)` | finite safety check of a bounded `s`, exact acceptance of an unbounded one |
| `weak(s)` | bounded `s` only, where it agrees with `strong(s)` |
| `s \|-> p`, `s \|=> p` | one attempt of `p` per match of `s` |
| `s #-# p`, `s #=# p` | a match of `s` plus `p` started on it, with the limits listed under rejected constructs |
| `p and q` | union of both obligation sets |
| `p or q` of two unbounded eventualities | one response monitor over the matches of both sides |
| `b implies p` | `p` started where `b` holds |
| `b iff c` | cycle-local equality |
| `if (b) p else q`, `case (e) ... endcase` | branch chosen on the first tick of the attempt |
| `disable iff (b)`, `accept_on (b)`, `reject_on (b)` and their `sync_` forms | abort letters, see below |
| named sequences and named properties | lowered in place, recursion rejected |

Finite properties are lowered to safety checks. Sequences and their `not`,
`or` and `and` are lowered along the paths of their matches; property `if`,
`case`, `implies`, `iff`, `disable iff` and `accept_on` as in the table, which
leaves out `not` and `or` of them.

`eventually p` and `s_always p` with no range, and `eventually [m:$] p` and
`s_always [m:$] p`, are not legal SystemVerilog: the grammar of IEEE 1800-2017
16.12 gives these two operators a bounded range. slang rejects them before
lowering.

## Where unbounded operators may appear

An unbounded eventuality, `always` with any range, an `until` operator, or
`strong(s)` of an unbounded `s` may appear:

- at the top of the property, and under `disable iff`, `accept_on` and
  `reject_on` (see the limits on `reject_on` below);
- as the operand of `always` with any range and of a bounded `s_always`;
- in the branches of a property `if`/`else` and `case`;
- in either operand of property `and`;
- in the consequent of `|->`, `|=>`, `#-#` and `#=#`, and on the right of
  `implies`;
- on both sides of `or` where both sides are unbounded eventualities
  (`s_eventually` or `not always` with no upper bound).

They may not appear under `not` other than in the dual forms of the table,
under `nexttime` or `s_nexttime`, under an eventuality other than as
`s_eventually strong(s)` (`s_eventually always b` is rejected), in an operand
of `until` or `iff`, on the left of `implies`, or in any other `or`.

## Sequences

Finite sequences retain the path-based lowering, including
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
- named sequences;
- `first_match` without match actions as the goal of an eventuality, where
  only the existence of a match matters, and with sequence `and`,
  `intersect` and `within` as described below. A monitor merging overlapping
  attempts cannot tell which match of an attempt came first, so it is
  rejected in a concatenation or a repetition.

Empty matches follow IEEE 1800-2017 16.9.2.1 in concatenation and repetition,
so `a ##1 b[*0:$] ##1 c` matches `a ##1 c`, while `##0 b[*0:$]` has only the
non-empty matches of `b[*0:$]`. An empty match of an implication antecedent
does not start the consequent of `|->`, and starts the consequent of `|=>` on
the first tick. The same goes for the left side of `#-#` and `#=#`. A sequence
used as a property, as under `strong`, `nexttime` or `s_eventually`, is not met
by an empty match, and with no other match it fails every attempt.

A sequence used as a property is weak in `assert` and `assume` unless written
`strong(...)`, and strong in `cover` (IEEE 1800-2017 16.12.2). So is the
operand of an eventuality: `s_eventually (a ##[1:$] b)` is met by an `a` with
no `b` after it, while `s_eventually strong(a ##[1:$] b)` needs the `b`. A
weak sequence with an unbounded match is rejected: `a |-> b ##[1:$] c` in an
assertion is, while `a |-> strong(b ##[1:$] c)`, which also requires the `c`
to come, is lowered.

Sequence `and`, `intersect`, `within` and `first_match` compose matches of
their operands from one and the same start, which a monitor taking the
matches of overlapping attempts at once cannot keep apart. They are supported
in an asserted `strong(...)` sequence, at the top of the property or in a
consequent. A fresh `$anyseq` bit selects one enabled assertion attempt, and
an auxiliary fairness condition requires some enabled attempt to be selected.
Since the proof must hold for every fair selector trace, every possible
attempt is checked without merging histories from overlapping attempts. The
operands of the selected attempt are monitored apart and composed as IEEE
1800-2017 F.3.4.2.4 defines them: a match of `and` ends with the later of a
match of each operand, an empty match of an operand puts no constraint on the
other one, `first_match` keeps the match of the attempt that ends first, and
the left operand of `within` may start on any tick of the right one.

Under an eventuality, whose attempts start on many ticks, and on the left of
`within`, all that counts is whether there was a match by a tick. There
`first_match` is the identity and sequence `or` and `throughout` compose,
while `and`, `intersect` and `within` of an unbounded sequence are rejected.
A `throughout` with none of these in its right operand is taken by the
sequence monitor, under any number of attempts.

## Exact temporal behavior

Vacuity and triggering follow the property structure:

- an implication launches a consequent only when its antecedent matches;
- a nonoverlapped implication or followed-by delays the launch by one sampled
  clock;
- no antecedent match means no response obligation;
- a top-level eventual property launches on every enabled sampled clock;
- `always p` launches `p` on every tick of its interval, where the attempts
  of overlapping intervals are one and the same.

The condition of a property `if` or `case` is a sampled value, like the
operands, and so is the condition of an abort, synchronous or not. An abort
condition, like that of a `disable iff`, is read on the ticks of the
assertion clock, so `accept_on` and `reject_on` act as `sync_accept_on` and
`sync_reject_on`. That is exact for a condition that changes with the
assertion clock, while a pulse between two of its ticks goes unseen.
The items of a property `case` compare with case equality: an `x` in an item
is no wildcard, it matches nothing but the same constant `x`.

An assertion in procedural code launches an attempt on the clock ticks where
the code around it reaches the statement, the attempt then runs to completion
whatever that code does on later ticks (IEEE 1800-2017 16.14.6). An assertion
in `always_comb` or `always @*` runs on its own clock or else the default
clocking. In `always @(posedge clk iff en)` the clock ticks only where `en` is
high: a justice goal is met only on such a tick, not on the cycles between. An
attempt still pending when the clock stops for good is never met, so it fails,
even where the match ending on the last tick meets an older attempt.

Overlapping triggers are preserved. For an eventual finite suffix of maximum
duration `N`, the monitor keeps pending trigger ages `1..N-1` and a saturated
older bucket. A suffix match of duration `D` can discharge only triggers whose
age is at least `D`. Consequently, a sequence match that began before a request
cannot satisfy that request, while one later match can satisfy multiple older
overlapping requests.

An unbounded minimum delay is exact. For
`s_eventually [m:$] p`, observations before sample `m` cannot complete the
obligation; there is no artificial maximum.

Aborts follow Annex F of IEEE 1800-2017. From the sample a `disable iff` or
`accept_on` condition holds in, the letters of the attempts in flight are T:
they meet every Boolean, so a pending liveness goal is accepted in that
sample, while the antecedent of an implication sees them dual and starts no
consequent. A failure settled before the abort stands, so does one of a
finite part of a liveness property, such as the left side of a followed-by or
the first element of a selected regular sequence. A `reject_on` gives the
dual letters: every attempt in flight in the rejecting sample fails,
including one in its first sample whose antecedent would not match, while a
reject after completion or after an earlier abort does not fail. Of aborts
holding in the same sample, the outermost one wins.

`reject_on` is supported around a liveness property with no safety part (so
no `until`, no finite consequent and no `strong` sequence that settles a
failure on its own), no `always`, and nothing but a Boolean antecedent ahead
of its obligations, as `b |-> s_eventually c`. Elsewhere, around a finite
property too, a monitor merging overlapping attempts cannot tell whether one
is still in flight, and it is rejected.

A `cover` is the dual of an assertion of the negated property, so the letters
of its `disable iff` are the dual of T: an attempt is a success where it was
settled true before the disable, such as by a completed match, while one
still in progress in the disabling sample is dropped. An `accept_on` in a
`cover` is a success with its T letters, as in an assertion, and a disable in
a later sample has no bearing on it.

Weak until enforces its left operand only while waiting for the right operand
and does not require the right operand to arrive. Strong until adds that
progress requirement. `until_with` includes the completion sample in the
left-operand safety interval.

Monitor state and finite-history enables are initialized explicitly. A finite
check is disabled until every required historical sample exists, including
paths involving empty repetitions.

## Rejected constructs

The frontend raises an error for every construct it cannot lower exactly,
rather than lowering it to a bounded approximation or a weaker check. Each
of these has a negative test in `tests/liveness/unsupported.ys` or
`tests/sva/*.ys`. The diagnostic is given by the start of its message.

| Construct | Diagnostic |
| --- | --- |
| `restrict property`, `cover sequence` | unsupported assertion statement |
| a weak sequence with an unbounded match, as a bare `a ##[1:$] b` in `assert` or `assume`, in a consequent, under an eventuality or in `weak(...)` | weak sequence with an unbounded match is unsupported |
| `cover property` of `always`, `until`, an unbounded eventuality or a sequence with an unbounded match | cover of a property with `always`, `until`, ... |
| `cover property` with a property `if`, `case` or `implies` in it, or with an implication that has an unbounded antecedent or `if`, `case`, `implies`, `iff` or an abort in its consequent: no check would see its vacuous successes | cover of a property with a property `if`, ... |
| `cover property` of an `and` or followed-by with `iff` or an abort in an operand, whose parts would each be covered on their own | cover of a property `and`, or of followed-by, ... |
| an unbounded generalized sequence in an assumption where it needs attempt selection, which would weaken the assumption | property needs an acceptance condition ... |
| `or` with a temporal operand, unless both sides are unbounded eventualities of sequences the monitor takes without an attempt selection; a temporal operand of `iff` or on the left of `implies` | property needs an acceptance condition ... |
| sequence `and`, `intersect`, `within` or `first_match` of an unbounded sequence under an eventuality or on the left of `within` | sequence `and`, `intersect`, `within` or `first_match` of an unbounded sequence is supported where ... |
| `first_match` or a sequence product of an unbounded sequence inside a concatenation or a repetition | unsupported assertion statement |
| followed-by with an unbounded left operand, or whose left operand ends on more than one tick, when the right operand is temporal or has a property `if`, `case`, `implies`, `iff` or abort in it | followed-by with an unbounded left operand, ... |
| `reject_on` around a finite property, or around a liveness property with a safety part, with `always`, or with more than a Boolean antecedent ahead of its obligations | `reject_on` is supported around a liveness property ... |
| temporal operands of `until`, `s_until`, `until_with` and `s_until_with` | unsupported assertion statement |
| liveness under `not` (other than the dual forms), `nexttime` or `s_nexttime`, or under an eventuality other than as `strong(s)`; `not` of `until` or of an unbounded `strong` sequence | unsupported assertion statement |
| recursive named properties | unsupported assertion statement |
| SVA match items, sequence local-variable assignments and action subroutines | SVA match items and local-variable actions are not synthesizable |
| a `disable iff` nested in a property by way of instances, which IEEE 1800-2017 16.12 does not allow | unsupported assertion statement |
| an automatic variable or a const cast read on a later tick than the one the attempt was queued on (in the condition of a `disable iff` or an abort, in an operand of `always`, `until` or an eventuality, after a delay or an unbounded repetition, or in a consequent that starts on a later tick) | automatic variable or const cast of non-constant value used past the first tick of an assertion attempt |
| several or incompatible clocks in one property, such as liveness on a second clock, and sampled-value clock arguments | encountered unsupported SVA feature |
| a temporal monitor with no single clock edge to run on: in `always_comb` or `always @*` with neither a clock of the assertion's own nor a default clocking, or in a process with an asynchronous reset | SVA temporal delay requires exactly one clock edge |

An automatic variable or a const cast read on the first tick of the attempt,
as in the antecedent `t` of `t |-> s_eventually b` or in the condition of a
property `if`, is read as frozen (IEEE 1800-2017 16.14.6.1).

## Proving liveness

### Prerequisites

- Yosys with the sv-elab plugin;
- SymbiYosys (`sby`), with `yosys-smtbmc` and z3 for the safety and
  reachability checks;
- `suprove`, for the liveness checks, from the pinned OSS CAD Suite release.
  `tools/setup_liveness_tools.sh` installs it and needs `curl`, `sha256sum`
  and `tar`;
- for the OpenTitan proofs, `git` to fetch OpenTitan and yices, the default
  solver of the stress run (`SOLVER` selects another).

The setup script downloads the OSS CAD Suite Linux x64 release dated
2026-07-27, verifies SHA-256
`d5cd8fb0276b51ae6b35945e70e03f222d82a863f2e3523b33b937be0ea416f4`,
and extracts `suprove` plus its runtime dependencies. The default install root
is the sibling `.tools` directory and can be changed with
`YOSYS_SLANG_TOOL_ROOT`. The script does not replace the system Yosys or ABC,
and prints the path of `suprove` as `SUPROVE_EXECUTABLE=...`.

Distribution packages do not consistently name the standalone ABC executable.
CTest discovers either `yosys-abc` or `abc` and passes the exact path only to
the equivalence test; no global compatibility symlink is installed.

### Running a proof

Lower the design as the tests do:

```sh
yosys -m build/slang.so -p '
  read_slang design.sv
  prep -top top
  async2sync
  formalff -clk2ff
  chformal -lower
  opt_clean
  write_rtlil top.il
'
```

SymbiYosys `mode live` proves the `$live` checks under the `$fair` ones, but
it turns every `assert` into an assumption and drops the covers. The safety
part of a property, such as the left operand of `s_until` or the finite
first element of a selected sequence, is then assumed rather than proven. Run
it in `mode prove` as well, where SymbiYosys removes the live and fair checks:

```
[tasks]
live
safety

[options]
live: mode live
safety: mode prove

[engines]
live: aiger suprove
safety: smtbmc z3

[script]
read_rtlil top.il
prep -top top

[files]
top.il
```

```sh
YOSYS_SLANG_REAL_SUPROVE=/absolute/path/to/suprove \
  sby --suprove tools/suprove_all_justice.py -f top.sby
```

`tools/suprove_all_justice.py` stands in for `suprove`. A model with several
justice properties is common: two liveness assertions, or a strong sequence
whose selected attempt brings a fairness constraint of its own next to any other
liveness obligation. For
such a model suprove reports one status per justice property, of which
SymbiYosys reads only the first, and with a fairness constraint as well it
reports none. The wrapper proves each justice property on its own, on a copy of
the model that keeps every fairness constraint, and reports a failure with the
witness of the property that failed, or a pass when all of them pass. A model
with one justice property goes to suprove unchanged.

Before relying on a proof, also establish non-vacuity: cover the
antecedent or request path in `mode cover`, inspect the generated `live` and
`fair` checks, and keep a broken-design run that the liveness engine must
reject.

### Tests

`tests/liveness/run_response_proof.sh PLUGIN SUPROVE` runs every
solver-backed liveness test in an isolated temporary directory and requires
each expected `PASS` and each intentional `FAIL`. In `mode live`, most shapes
with a failing variant next to the passing one:

- `response.sby`: a request-grant response, and the same with the grant
  mutated away;
- `semantics.sby`: `disable iff` and `reject_on` around a response, `or` of
  eventualities, minimum delays, fairness assumptions, suffix ages and
  overlapping requests;
- `regular.sby`: the unbounded sequence operators;
- `procedural.sby`: procedural assertions, guards, `iff` clocking and default
  clocking;
- `clock_stop.sby`: an `iff` qualified clock that stops for good, or pauses,
  with attempts pending;
- `sequence.sby`: named sequences, `nexttime` and empty matches;
- `abort.sby`: aborts around selected sequences, implications and
  followed-by, and their order;
- `constants.sby`: elaboration-time constants in properties;
- `composition.sby`: strong and weak sequences, sequence products and
  followed-by;
- `justice.sby`: several justice properties with a fairness constraint;
- `operators.sby`: the progress part of strong until, and liveness under
  `implies`, `if` and `case`.

The safety parts of mixed properties are proven in `mode prove`
(`*_safety.sby`), the antecedents are covered (`*reachability.sby`), and
`raw_live.sby` checks the solver flow on hand-written RTLIL.

The OpenTitan liveness tests fetch a pinned OpenTitan revision on demand. The
arbiter test proves safety, reachability and starvation freedom of
`prim_arbiter_ppc`, then requires separate safety and liveness mutations to
fail. The stress-target test lowers and inspects the liveness properties of
`prim_alert_rxtx`, `prim_esc_rxtx`, `sha3pad` and `tlul` and runs their
bounded safety portions after checking the number of live and fair checks.
Small target-specific models separately prove the corresponding liveness
shapes, antecedent reachability and intentional non-progress failures. The
unconstrained whole peripheral harnesses are not claimed as end-to-end
liveness proofs.

Through CTest:

```sh
cmake -S . -B build \
  -DENABLE_LIVENESS_PROOFS=ON \
  -DSUPROVE_EXECUTABLE=/absolute/path/to/suprove
cmake --build build
ctest --test-dir build -L liveness --output-on-failure
```

`ENABLE_LIVENESS_PROOFS` alone registers the response proofs. With
`-DENABLE_OPENTITAN_SVA_STRESS=ON` as well, the OpenTitan liveness tests are
registered too, next to the OpenTitan SVA stress test.

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
