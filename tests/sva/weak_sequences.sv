// Weak sequences with an unbounded match against hand-written monitors. An attempt of
// a weak sequence fails on the tick it can no longer match whatever comes next, and
// holds from its first match on (IEEE 1800-2017 16.12.2, F.3.4.4). Up to then it is
// pending and fails on no tick, so a reference fails exactly where the attempts have
// no way left.
module weak_sequences(input clk_i, input logic a, input logic b, input logic c,
					  input logic d, input logic e, input logic r);
	// The previous sample of each signal, low before the first tick
	reg a1 = 0, b1 = 0;

	// Attempts of `b throughout c[->1]` waiting for `c` with `b` held so far
	reg goto_q = 0;
	wire goto_w = a || goto_q;
	// The same under `disable iff (r)` or `accept_on (r)`
	reg dis_q = 0;
	wire dis_w = (a || dis_q) && !r;
	// Attempts of `c[->1] ##1 d` waiting for `c`, and those that saw it on the last tick
	reg next_q = 0, next_armed = 0;
	wire next_w = a || next_q;
	// Attempts of `b[*1:$] ##1 c` in a run of `b`
	reg run_q = 0;
	// Whether an attempt of `always (...)` has started
	reg seen_q = 0;
	wire seen = a || seen_q;
	// Attempts of the named `(b throughout c[->1]) ##1 d` waiting for `c`, and those
	// that saw it on the last tick
	reg named_q = 0, named_armed = 0;
	wire named_w = a || named_q;
	// Attempts of the `if (e)` branch waiting for `c`
	reg if_q = 0;
	wire if_w = (a && e) || if_q;
	// Attempts of the bare `b ##1 c[->1] ##1 d`, one starting on each tick, waiting for
	// `c` or past it
	reg bare_q = 0, bare_armed = 0;
	wire bare_w = b1 || bare_q;
	// Attempts of `b throughout c[->1] ##1 !b` waiting for `c` with `b` held, and those
	// that saw it on the last tick
	reg never_q = 0, never_armed = 0;
	wire never_w = (a || never_q) && !r;

	always @(posedge clk_i) begin
		a1 <= a;
		b1 <= b;
		goto_q <= goto_w && b && !c;
		dis_q <= dis_w && b && !c;
		next_q <= next_w && !c;
		next_armed <= next_w && c;
		run_q <= (a && b) || (run_q && b && !c);
		seen_q <= seen;
		named_q <= named_w && b && !c;
		named_armed <= named_w && b && c;
		if_q <= if_w && b && !c;
		bare_q <= bare_w && !c;
		bare_armed <= bare_w && c;
		never_q <= never_w && b && !c;
		never_armed <= never_w && b && c;
	end

	always @(posedge clk_i) begin
		goto_ref: assert(!(goto_w && !b));
		goto_assume_ref: assume(!(goto_w && !b));
		disable_ref: assert(!(dis_w && !b));
		accept_ref: assert(!(dis_w && !b));
		next_ref: assume(!(next_armed && !d));
		run_ref: assert(!((a && !b) || (run_q && !b && !c)));
		always_ref: assert(!(seen && !b));
		named_ref: assert(!((named_w && !b) || (named_armed && !d)));
		if_ref: assert(!((if_w && !b) || (a && !e && !d)));
		bare_ref: assert(!(!b || (bare_armed && !d)));
		// Past the bounded head an unbounded wait is always left
		head_ref: assert(!(a1 && b));
		weak_ref: assert(!(a1 && !b));
		delay_ref: assert(a);
		delay_plus_ref: assert(a);
		delay_star_ref: assert(a);
		repetition_ref: assert(a);
		repetition_star_ref: assert(b);
		repetition_plus_ref: assert(b && (!b1 || a));
		weak_top_ref: assert(a);
		consequent_ref: assert(!(a && !b));
		bare_assume_ref: assume(a);
		// It never matches, so an attempt that sees `b` fails on the next tick
		never_matches_ref: assert(!((never_w && !b) || (never_armed && !r)));
	end

	// `b` holds up to and including the first `c`
	goto_test: assert property (@(posedge clk_i) a |-> b throughout c[->1]);
	// An assumption takes a weak sequence its attempts each follow on one path at a time
	goto_assume_test: assume property (@(posedge clk_i) a |-> b throughout c[->1]);
	disable_test: assert property (@(posedge clk_i) disable iff (r) a |-> b throughout c[->1]);
	accept_test: assert property (@(posedge clk_i) accept_on (r) (a |-> b throughout c[->1]));
	next_test: assume property (@(posedge clk_i) a |-> c[->1] ##1 d);
	run_test: assert property (@(posedge clk_i) a |-> b[*1:$] ##1 c);
	always_test: assert property (@(posedge clk_i) a |-> always (b throughout c[->1]));

	sequence handshake;
		(b throughout c[->1]) ##1 d;
	endsequence
	named_test: assert property (@(posedge clk_i) a |-> handshake);

	if_test: assert property (@(posedge clk_i) a |-> if (e) (b throughout c[->1]) else d);
	bare_test: assert property (@(posedge clk_i) b ##1 c[->1] ##1 d);
	head_test: assert property (@(posedge clk_i) a |=> !b ##[1:$] $stable(b) ##1 c);
	weak_test: assert property (@(posedge clk_i) a |=> weak(b ##[1:$] c));

	// A sequence used as the whole property, starting on each tick
	delay_test: assert property (@(posedge clk_i) a ##[1:$] b);
	delay_plus_test: assert property (@(posedge clk_i) a ##[+] b);
	delay_star_test: assert property (@(posedge clk_i) a ##[*] b);
	repetition_test: assert property (@(posedge clk_i) a[*1:$] ##1 b);
	// `a[*]` matches empty, so `b` alone is a match
	repetition_star_test: assert property (@(posedge clk_i) b ##1 a[*]);
	repetition_plus_test: assert property (@(posedge clk_i) b ##1 a[+]);
	weak_top_test: assert property (@(posedge clk_i) weak(a ##[1:$] b));
	consequent_test: assert property (@(posedge clk_i) a |-> b ##[1:$] c);
	bare_assume_test: assume property (@(posedge clk_i) a ##[1:$] b);

	// `##` binds tighter than `throughout`, so the last tick needs both `b` and `!b`
	never_matches_test: assert property (@(posedge clk_i) disable iff (r)
		a |-> b throughout c[->1] ##1 !b);
endmodule
