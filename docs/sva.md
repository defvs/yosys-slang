# SystemVerilog assertions

sv-elab lowers the assertions of SystemVerilog (IEEE 1800-2017 chapter 16) into logic
a formal tool can check. Each assertion statement becomes one Yosys `$check` cell, fed
by registers that delay the values the assertion samples. This document describes the
subset that gets lowered, when the checks fire, where the lowering departs from the
standard, and which constructs are rejected.

A construct outside the subset is an error, so a design that reads in has every
assertion it states. The exceptions are the deviations listed under [Deviations from
IEEE 1800-2017](#deviations-from-ieee-1800-2017) and the two statements [dropped with a
warning](#dropped-with-a-warning).

## Checks

Each `assert property`, `assume property` and `cover property` becomes a `$check` cell
with the `FLAVOR` parameter `"assert"`, `"assume"` or `"cover"`. The ports are:

- `TRG`, with `TRG_POLARITY`: the clock of the assertion and its edge;
- `EN`: an attempt is reported on this tick;
- `A`: the property holds on that attempt.

An assert or an assume fails on a tick with `EN` high and `A` low. A cover is hit on a
tick with `EN` and `A` both high. That includes a vacuous success: `cover property (a |->
b)` is hit on any tick where the attempt sees no `a`. IEEE 1800-2017 16.14.3 counts
vacuous successes too, in a counter of their own.

The cell takes the label of the statement, prefixed by the path of any generate block it
is in, as in `g.req_gnt`. A label reached more than once, in a loop or in a function
called more than once, gives cells `label`, `label_1` and so on. A statement without a
label gets an internal name.

Immediate assertions (`assert`, `assume` and `cover`, also deferred with `#0` or `final`)
become `$check` cells too. They take the trigger of the process they are in, with the
conditions around them as `EN`. In `always_comb` that makes an unclocked check, which
holds at every point in time.

Action blocks are not part of the check, see [Action blocks](#action-blocks).

## Clocking

A concurrent assertion is clocked by, in this order:

1. its leading clocking event, such as `@(posedge clk)` or `@(negedge clk)` at the top of
   the property, or at the top of a named property or sequence the assertion consists of;
2. in a process with one edge in its event control, such as `always @(posedge clk)` or
   `always_ff @(negedge clk iff en)`, that edge. A leading clocking event of the assertion
   has to be the same edge of the same signal, in a process with no `iff` guard. The
   ticks of a process with an `iff` guard are those on which the guard holds;
3. the default clocking of the scope (`default clocking @(posedge clk); endclocking`),
   also when the clocking block declares signals of its own.

Assertions on different clocks can sit side by side. A clock has to be one edge of a 1-bit
signal: an event list, an `iff` on the assertion's clock or on the default clocking, `edge`,
a clocking block or sequence as the event, and a clock within the property are rejected.
So is any concurrent assertion in a process on both edges of its clock, `always @(edge
clk)` with `--allow-dual-edge-ff`, as such a process queues attempts on both edges. An
immediate assertion there is checked on one edge, see
[Deviations](#immediate-assertions-on-both-edges).

Without any of these clocks, an assertion with no delay and no sampled value function
becomes an unclocked check, like an immediate assertion in `always_comb`. IEEE 1800-2017
16.16 requires a clock for it. A delay or a sampled value function needs a clock and is
an error there.

## Attempts

A concurrent assertion outside procedural code starts an attempt on every tick of its
clock.

In procedural code an attempt is queued when the statement is reached (IEEE 1800-2017
16.14.6). The conditions of the code around it, as the process computes them up to that
point, decide whether an attempt starts on that tick. Attempts already started run to
completion whatever the conditions do later. Each pass of a loop over the statement
queues an attempt of its own and gives a cell of its own.

- In a process with an edge, the conditions are worked out like any other code of the
  process, blocking assignments before the assertion included.
- In a process with no edge in its event control (`always_comb`, `always @*`) no clock is
  inferred, so the assertion takes its own clock or the default clocking. The conditions
  are then read on the tick of that clock, from the values before the edge; see
  [Deviations](#deviations-from-ieee-1800-2017).
- In the branches of an asynchronous reset, as in `always @(posedge clk or posedge rst)`,
  an assertion with a clock of its own, a delay or a sampled value function is an error.
  One without is lowered like an immediate assertion in the same place: on the clock edge
  in the clocked branch, and as an unclocked check while the reset is active in the reset
  branch.
- An `initial` procedure infers no clock either, so the assertion takes its own clock or
  the default clocking. The procedure runs once, at time 0, and queues one attempt, which
  starts on the first tick of that clock with the values sampled then. That tick need not
  be the first step of a trace when there are several clocks. Automatic variables are read
  as they are at the statement, and the conditions around it are worked out at time 0.
  An assertion the procedure reaches under an `if` on a value unknown then (`x`), or in a
  `case` statement, is an error, and so is one in a procedure with a timing control under
  `--ignore-timing`, which runs the procedure as if there were none. With neither a clock
  of its own nor a default clocking, an assertion with no delay and no sampled value
  function is lowered like an immediate assertion there: it is checked once, on the
  initial values.

## Sampling

The operands of a property are read with their sampled values (IEEE 1800-2017 16.5.1),
the values before the clock edge of the tick. A blocking assignment the process makes
before the assertion does not change them.

Automatic variables, `const'` casts, and the variables of a function while a call of it
is running are read as they are at the statement, which is the value they had when the
attempt was queued (16.14.6.1). That value exists on the first tick of the attempt only,
so reading one that is not constant on a later tick of the attempt is an error. Loop
indices and other constant values can be read on any tick. A static variable of a function
read from outside a call of that function, as in `f.k`, by the assertion or by a function
it calls, is a sampled value like any other; no function of the assertion may assign it
(see [Functions](#functions)).

The sampled value functions `$past(e)`, `$past(e, n)`, `$rose`, `$fell`, `$stable` and
`$changed` use registers on the clock of the assertion, or of the process for an immediate
assertion. They need a single clock edge. These registers have no initial value: on the
first ticks of a trace, before there is a value to go back to, they read an unconstrained
value where 16.9.3 uses the default sampled value. The gating and clocking arguments of
`$past`, the clocking argument of the others, `$sampled` and the global clocking functions
(`$past_gclk`, `$rose_gclk` and so on) are rejected.

## Supported subset

### Sequences

- boolean expressions, including calls of functions without effects (see
  [Functions](#functions)), `let` and the sampled value functions above;
- concatenation with `##n` and `##[m:n]`, including `##0` and a leading delay;
- consecutive repetition `[*n]` and `[*m:n]` of booleans and of sequences, including
  `[*0]`;
- `and`, `or` and `intersect` of sequences;
- `throughout`, with a boolean on its left;
- `first_match` without match items;
- named sequences, with arguments.

### Properties

- a sequence, which holds if it has a match; `strong(s)` and `weak(s)` of it;
- `not`, `and` and `or` of properties;
- `|->` and `|=>`;
- `nexttime`, `s_nexttime`, `nexttime [n]` and `s_nexttime [n]`;
- `eventually [m:n]` and `s_eventually [m:n]`;
- `disable iff` at the top of the property, and `default disable iff`, see
  [Disable iff](#disable-iff);
- named properties, with arguments, unless recursive.

Ranges and repetitions are bounded, but of any size; there is no limit on `n` in `##[1:n]`
or `[*1:n]`. Ticks are counted in 64 bits. The registers and cells grow linearly with the
bounds, with two exceptions, which take a number of cells quadratic in `n`:

- a ranged repetition of a sequence whose matches differ in length, as in
  `(a ##[1:2] b)[*1:n]`;
- a sequence with matches ending on several ticks, placed in a concatenation after a range
  or after a sequence that ends on several ticks, if it is built again for each of those
  ticks, as in `a ##[1:n] ((b ##[1:n] c) and d)`. That is the case for `and` and
  `intersect`; for `first_match`, unless of a concatenation of elements that each match on
  one tick from their start, with a range as the last delay and fixed delays otherwise,
  as in `first_match(b ##2 c ##[1:n] d)`; for `throughout` of a concatenation with an
  element that can match empty; for a concatenation whose first two elements can both
  match empty; and for `or`, `throughout`, concatenation and repetition of one of these.

An assumption takes some more logic, see [Assumptions](#assumptions).

A bounded sequence has settled all its matches once its longest path is over, which is
where its strong and weak forms agree. Likewise `nexttime` and `s_nexttime` are lowered
alike, and so are `eventually` and `s_eventually`: checks come at the end of an attempt
(see [When checks fire](#when-checks-fire)), so an attempt cut short by the end of a trace,
or by a clock that stops for good, is not reported either way. An assumption can rule out
such an attempt before its end, but only once it has failed on the values sampled so far,
see [Assumptions](#assumptions).

Local variables of sequences and properties, `local` arguments, match items, unbounded
ranges and repetitions, and the other operators of chapter 16 are rejected, see
[Rejected constructs](#rejected-constructs).

### Empty matches

An empty match, such as that of `a[*0]` or the empty case of `a[*0:1]`, spans no ticks.
Concatenation follows IEEE 1800-2017 16.9.2.1:

- `empty ##0 seq` and `seq ##0 empty` have no match;
- `empty ##n seq` with `n` > 0 is `##(n-1) seq`;
- `seq ##n empty` with `n` > 0 is `seq ##(n-1) 1'b1`.

So `##0 a[*0:1] ##1 b` keeps only the nonempty match of `a[*0:1]`, while the bare first
element of `a[*0:1] ##1 b` keeps both. The operators treat an empty match as follows:

- an empty match of one operand of `and` puts no constraint on the other one;
- `intersect` matches empty only if both operands do;
- `throughout` matches empty where its right operand does;
- `first_match` of a sequence with an empty match is that empty match;
- an empty match of the antecedent of `|->` starts nothing, while one of the antecedent
  of `|=>` starts the consequent on the first tick of the attempt (16.12.7);
- a sequence used as a property does not hold by an empty match. If it has no other match,
  the property fails on every attempt.

## When checks fire

### Assertions and covers

An attempt is reported once, on the last tick it can take: `L` ticks after its first tick,
where `L` is the length of the longest path through the property. `EN` is the attempt
delayed by `L` ticks on a line of registers that starts out low, so no attempt is reported
on the first `L` ticks of a trace. The report comes on that tick even when the outcome was
settled earlier, for a failure as well as for a match:

- `a |-> b ##[1:3] c` with no `b` on the tick of `a` fails 3 ticks after `a`;
- `cover property (a ##[1:3] b)` with `b` on the tick after `a` is hit 3 ticks after `a`.

A bounded model check therefore sees a failure or a cover only if it reaches `L` ticks
past the first tick of the attempt. A path that can never match counts for `L` too, and a
property with no possible match at all fails on the first tick of each attempt.

### Assumptions

An assumption constrains a trace only on the ticks it gets checked on. Checked on the last
tick alone, it would leave a trace ending sooner unconstrained, and a bounded or inductive
proof, or a cover, could pick a trace the environment rules out. So an assumption is also
checked on each earlier tick of an attempt on which the values sampled so far show that
no way of going on can satisfy the property. All of these checks share the one `$check`
cell, whose `EN` is high on any of them.

One way of going on is the clock stopping for good: the clock of a process with an `iff`
guard, or any clock in a flow with several clocks (such as `multiclock on` in
SymbiYosys), can tick no more. A weak operator whose ticks never come then holds (IEEE
1800-2017 16.12.10 and 16.12.13), and so does a weak sequence that can match once the
missing ticks are padded with letters that satisfy every boolean, `1'b0` included (Annex
F.5.3.1). So whatever is sampled on a later tick, a constant too, counts as unknown until
that tick:

- `a |-> ##2 1'b0`, `a |-> nexttime [2] 1'b0` and `a |=> 1'b0` are rejected on their
  last tick only;
- `a |-> b ##2 1'b0` is also rejected on the tick of `a` if `b` is low then, as that
  failure stands whatever comes next.

Over traces on which the clock keeps ticking, an assumption allows the same traces as with
the last check alone. A trace that ends, or whose clock stops for good, after an attempt
has failed on the values sampled so far is ruled out sooner.

The checks before the last tick come from evaluating the property with the samples not
yet taken as unknown, in three-valued logic. They are not always on the earliest tick
possible:

- Values that are unknown but related are taken as unrelated. `a |-> (##2 c) and (##2 !c)`
  is doomed on the tick of `a`, but rejected on its last tick only.
- A strong operator over ticks still to come is taken like a weak one. `a |-> s_nexttime
  [2] 1'b0` fails whether or not the clock stops, but is rejected on its last tick only.
- The analysis gets a budget in proportion to the size of the property. It is done for the
  first tick of the attempt, then for twice as many ticks at a time. Once a pass goes over
  the budget, the later ticks get checks that may come later than they could, or only the
  check on the last tick. For instance `a |-> b ##[1:n] c ##[1:n] d` with `n` of 200 or
  more gets the earliest checks on about the first third to half of its ticks only.
- Where nothing can be doomed before the last tick, as with `a |=> b`, `a |-> ##[1:n] b`
  or `not (a ##1 b)`, there is no earlier check.

Under a disable, a failure settled with no disable seen so far is rejected on its tick, as
it stands whatever comes later (see [Disable iff](#disable-iff)). The extra logic for the
earlier checks is bounded by a few times the size of the property.

Assertions and covers are reported on the last tick only.

## Disable iff

`disable iff (c)` is lowered at the top of a property, also when it comes from a named
property the assertion consists of. `default disable iff c;` applies to the assertions of
its scope that have none of their own. A `disable iff` anywhere else is rejected.

The lowering follows Annex F of IEEE 1800-2017. The attempt is disabled from the first of
its ticks, the first tick of the attempt included, on which the condition is true. The
property then holds if it holds on the ticks before that one, extended with letters that
satisfy every boolean. Under `not`, and in the antecedent of an implication, the letters
satisfy no boolean. A failure settled before the disable therefore stands, and an attempt
disabled before its outcome is settled passes. For a cover the letters are the dual ones:
a cover is hit by a match, or a property settled true, ahead of the disable.

The condition of a `disable iff` is read differently from the standard, see the next
section.

## Deviations from IEEE 1800-2017

### The disable condition is sampled

IEEE 1800-2017 16.12 has a simulator test the disable condition on current values, not
sampled ones, at any time from the start of the attempt in the Observed region to its end.
sv-elab reads the condition of a `disable iff`, explicit or default, as a sampled value on
the ticks of the assertion's clock, as formal tools do. A condition that changes on a
clock edge thus takes effect one tick later than in simulation, and a pulse between two
ticks goes unseen. For example:

```systemverilog
always @(posedge clk) rst <= b;
p: assert property (@(posedge clk) disable iff (rst) !b);
```

On a tick where `b` is sampled high, a simulator already sees `rst` high and disables
the attempt, so `p` never fails in simulation. Here `rst` is sampled as `b` was on the tick
before, so `p` fails on a tick where `b` rises. To match simulation for a reset that a register on the clock loads on every
edge, disable on the register's input instead, here `disable iff (b)`: its sampled value on
a tick is what a simulator sees in the register on that tick.

### Conditions of processes with no edge

For an assertion in `always_comb` or `always @*`, the conditions around it, automatic
variables and `const'` casts are read on the tick of the assertion's clock, from the values
before the edge. A simulator starts the attempt the process queued last in the time step
of the tick (16.14.6), and such a process runs again after the edge has updated what it
reads. So a condition that depends on a register on the assertion's clock takes effect one
tick later than in simulation, and a pulse between two ticks, which in simulation queues an
attempt for the next tick, goes unseen.

### No clock, reset branches

Without a clock, also in an `initial` procedure, and in the reset branch of an asynchronous
reset, an assertion of a single tick is lowered like an immediate assertion, as described
under [Clocking](#clocking) and [Attempts](#attempts). It is then checked at every point in
time, or once on the initial values, rather than on the ticks of a clock.

### Immediate assertions on both edges

An immediate assertion in a process on both edges of its clock, `always @(edge clk)` with
`--allow-dual-edge-ff`, is checked on the falling edge alone, and `$past` and the other
sampled value functions in it take their values on that edge. A simulator runs it on both.

### Initial values of sampled value functions

The registers of `$past`, `$rose`, `$fell`, `$stable` and `$changed` have no initial value,
see [Sampling](#sampling).

## Functions

A function called by a concurrent assertion, anywhere in it (its operands, the disable
condition, the clock, the arguments of named sequences and properties), has to be free of
effects as IEEE 1800-2017 16.6 requires. It is lowered by running its body like any other
call, so it is rejected if it

- assigns a variable that is not its own, that is, other than its arguments, its return
  value and its local variables, also through a function it calls or an output argument;
- assigns one of its own static variables, including an argument of a static function,
  which a hierarchical name refers to anywhere in the design, as in `f.k`: that name would
  read what the design left there, not what the call did;
- reads one of its static variables, or its static return value or output argument,
  before every path through the call has assigned it, as that reads what an earlier call
  left there. A read of the variable by a function the call runs, through a hierarchical
  name, counts as the function's own read at that call.

The last check follows paths, not values, so it can reject a static function that fills
a variable bit by bit and then reads it whole. Declaring the function `automatic`, or
assigning the variable whole first, avoids that. Immediate assertions are not checked
this way.

## Action blocks

Action blocks are left out: the check stands in for what they report. An action block is
accepted when nothing it does can be seen by the design or by another assertion:

- the severity tasks, `$display`, `$write`, `$strobe` and `$monitor` with their variants,
  `$monitoron` and `$monitoroff`;
- `$finish`, `$stop` and `$exit`;
- writing to files (`$fdisplay`, `$fwrite`, `$fstrobe`, `$fmonitor` and their variants,
  `$fflush`), `$writememb` and `$writememh`, `$printtimescale`, `$timeformat` and the
  `$dump` tasks;
- system functions that compute a value, such as `$sformatf` or `$time`;
- `$urandom` and `$urandom_range`; `$random` without a seed; `$random` and the `$dist_`
  functions with a seed that is a variable of the action block;
- calls of functions that write only their own variables and keep no state;
- calls of imported (DPI) subroutines not declared `context`, which can only access their
  arguments (IEEE 1800-2017 35.5.3);
- blocks with variables of their own, `if`, `case` and loops, and `break` and `continue`
  within those loops.

A variable of the action block or of a function it calls counts as its own only while no
hierarchical name in the design, as in `blk.n` or `f.k`, refers to it: such a name reads
what the action block leaves there.

Anything else is rejected, such as an assignment to a variable outside the action block,
also in the argument of a report (`$error("%0d", cnt++)`) or through a class or virtual
interface handle, an assignment to a variable a hierarchical name refers to, a task call,
`$sformat`, `$value$plusargs`, `$readmemh`, `$fopen` or `$fclose`, `$assertoff` and the
other assertion control tasks, a call of a DPI import declared `context`, which can call
back into the design, an assertion, an event trigger, a delay or event control, also one
within an assignment (`x = @(posedge clk) y`), `fork`, `disable`, or a `break`, `continue`
or `return` that leaves the action block.

## Final procedures

Assertions in `final` procedures are rejected: they check the end of a simulation, which a
netlist does not have. That includes assertions in functions a `final` procedure calls,
reported at the call with a note at each assertion. The same function called from other
processes is lowered there as usual. A `final` procedure without assertions is ignored.

## Checkers

Checker declarations are skipped. A checker instance, procedural or not, is rejected, also
with `--ignore-assertions`, as a checker can drive the design through an output.

## Ignoring assertions

`read_slang --ignore-assertions` leaves out all immediate and concurrent assertions,
including those in `final` procedures, and skips the checks on action blocks and on the
functions of assertions. Checker instances are still rejected.

## Rejected constructs

Each of these is an error. The message column gives the start of the diagnostic, up to its
first variable part. The tests are under `tests/`.

| Construct | Message | Test |
| --- | --- | --- |
| Unbounded delay, `##[m:$]`, `##[*]`, `##[+]` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| Unbounded repetition, `[*m:$]`, `[*]`, `[+]` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| Nonconsecutive and goto repetition, `[=n]`, `[->n]` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| `within` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| Match items, `(s, v = e)`, `(s, f())`, `first_match(s, ...)` | `unsupported assertion statement` | `sva/unsupported_constructs.ys`, `sva/local_variables.ys` |
| Local variables of sequences and properties, `local` arguments | `unsupported assertion statement` | `sva/local_variables.ys` |
| `always`, `s_always`, with or without a range | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| `s_eventually` without a range or with `$` as its bound | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| `until`, `s_until`, `until_with`, `s_until_with` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| `implies`, `iff` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| `#-#`, `#=#` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| `accept_on`, `reject_on`, `sync_accept_on`, `sync_reject_on` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| Property `if`/`else` and `case` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| Recursive properties | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| `disable iff` other than at the top of the property | `unsupported assertion statement` | `sva/nested_disable.ys` |
| `restrict property`, `cover sequence` | `unsupported assertion statement` | `sva/unsupported_constructs.ys` |
| The sequence methods `.triggered`, `.matched` | `unsupported system task` | `sva/unsupported_constructs.ys` |
| `$sampled`, the global clocking functions `$past_gclk`, `$rose_gclk` and so on | `unsupported system task` | `sva/unsupported_constructs.ys` |
| Gating or clocking argument of `$past` | `$past gating_expr (3rd argument) and clocking_event (4th argument) are not supported` | `sva/unsupported_constructs.ys` |
| Clocking argument of `$rose`, `$fell`, `$stable`, `$changed` | `encountered unsupported SVA feature` | `sva/unsupported_sampled_clocking.ys` |
| Sampled value function without a single clock edge | `system function` | `sva/unsupported_constructs.ys` |
| Automatic variable or `const'` cast read after the first tick of an attempt | `automatic variable or const cast of non-constant value used past the first tick of an assertion attempt` | `sva/frozen_past_first_tick.ys` |
| Delay without a single clock edge: no clock, also in an `initial` procedure, a process with no edge and no clock for the assertion, a reset branch | `SVA temporal delay requires exactly one clock edge` | `sva/unsupported_constructs.ys` |
| Clock without an edge: `@(clk)`, a clocking block, a sequence or `$global_clock` as the event, as in `@(cb)` or `@(s)` | `SVA clocking requires a signal edge` | `sva/unsupported_constructs.ys` |
| Clock on both edges, `@(edge clk)`, also with `--allow-dual-edge-ff`, and any concurrent assertion in a process `always @(edge clk)` | `@(edge) will not be synthesized` | `sva/unsupported_constructs.ys` |
| `iff` on the clock of an assertion or on the default clocking | `iff qualifier not supported in combination with this event` | `sva/unsupported_clocking.ys`, `sva/unsupported_constructs.ys` |
| Clock of more than one bit, event list as the clock | `encountered unsupported SVA feature` | `sva/unsupported_clocking.ys`, `sva/unsupported_constructs.ys` |
| Clock of an assertion in a process other than the process's own: another signal or edge, a process with an `iff` guard, a reset branch | `encountered unsupported SVA feature` | `sva/unsupported_clocking.ys`, `sva/unsupported_constructs.ys` |
| Assertion with a clock in an `initial` procedure, reached under an `if` on an unknown value or in a `case` statement, or in a procedure with a timing control under `--ignore-timing` | `encountered unsupported SVA feature` | `sva/initial_attempt.ys` |
| Clock within a property: multiclocked properties, repetition of a clocked sequence | `encountered unsupported SVA feature` | `sva/unsupported_clocking.ys`, `sva/unsupported_constructs.ys` |
| Signals of a clocking block, read or driven | `clocking block signals unsupported` | `sva/unsupported_clocking.ys` |
| Event control of a process on a clocking block or a sequence, `always @(cb)`, `always @(s)` | `unsynthesizable feature` | `sva/unsupported_clocking.ys` |
| Clocking blocks other than the default one, `global clocking` | `unsynthesizable timing control` | `sva/unsupported_constructs.ys` |
| Sequence or property declared in an instance kept by `--keep-hierarchy`, or reading across its boundary | `hierarchical reference across preserved module boundary` | `sva/kept_hierarchy.ys` |
| Function of a concurrent assertion assigning a variable not its own | `function called in a concurrent assertion assigns a variable not its own` | `sva/assertion_function_effects.ys` |
| Function of a concurrent assertion reading state an earlier call left | `function called in a concurrent assertion may read state left by an earlier call` | `sva/assertion_function_effects.ys` |
| Function of a concurrent assertion assigning a static variable a hierarchical name refers to | `function called in a concurrent assertion assigns a variable a hierarchical name refers to` | `sva/assertion_function_effects.ys` |
| Action block with effects | `unsupported statement in assertion action block` | `sva/action_blocks.ys` |
| Assertion in a `final` procedure, or in a function it calls | `assertions in final procedures are unsupported` | `sva/final_assertions.ys` |
| Checker instance | `checker instances are unsupported` | `sva/checkers.ys` |

## Dropped with a warning

Two statements are left out with a warning rather than an error:

| Construct | Message | Test |
| --- | --- | --- |
| `expect` | `expect statement will not be synthesized` | `various/expect_test.ys` |
| Program blocks, with the assertions in them | `program block will not be synthesized` | `various/program_test.ys` |

## Inspecting the output

The checks can be found in the RTLIL a design is written to:

```systemverilog
module handshake(input logic clk, input logic rst, input logic req, input logic gnt);
	default clocking @(posedge clk); endclocking
	default disable iff (rst);

	req_gnt: assert property (req |-> ##[1:3] gnt);
	gnt_pulse: assume property (gnt |=> !gnt);
	gnt_seen: cover property (req ##1 gnt);
endmodule
```

```sh
yosys -m build/slang.so -p 'read_slang handshake.sv; write_rtlil handshake.il'
grep -A11 'cell $check' handshake.il
```

The single quotes keep the shell from expanding `$check`. The output has one cell per
assertion:

```
  cell $check \req_gnt
    parameter \TRG_ENABLE 1
    parameter \TRG_WIDTH 1
    parameter \TRG_POLARITY 1'1
    parameter \FLAVOR "assert"
    parameter \FORMAT ""
    parameter \ARGS_WIDTH 0
    parameter \PRIORITY 32'11111111111111111111111111111111
    connect \EN $sva_delay$5
    connect \TRG \clk
    connect \ARGS { }
    connect \A $37y
--
  cell $check \gnt_pulse
...
```

`EN` of `req_gnt` is the attempt delayed by the three ticks of its range. Within Yosys,
`select -list t:$check` lists the cells.
