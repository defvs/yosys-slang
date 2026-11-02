// An eventuality of a sequence spelled strong(): a bare one would be weak in an
// assertion or an assumption (IEEE 1800-2017 16.12.2), which is rejected where its
// match is unbounded. The assumption makes `b` come again and again.
module strong_assumed_pass ((* gclk *) input logic clk, input logic a, input logic b);
	assume property (@(posedge clk) s_eventually strong(##[1:$] b));
	assert property (@(posedge clk) s_eventually b);
endmodule

// It says nothing of `a`
module strong_assumed_fail ((* gclk *) input logic clk, input logic a, input logic b);
	assume property (@(posedge clk) s_eventually strong(##[1:$] b));
	assert property (@(posedge clk) s_eventually (a && b));
endmodule

// The match of `a ##[1:$] b` from the `a` on phase 0 ends on phase 2
module strong_next_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 2;

	assert property (@(posedge clk) s_eventually (nexttime strong(a ##[1:$] b)));
endmodule

module strong_next_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = 1'b0;

	assert property (@(posedge clk) s_eventually (nexttime strong(a ##[1:$] b)));
endmodule

// weak() of a bounded sequence is strong(): no match of `a ##1 b` ever comes
module weak_bounded_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 2;

	assert property (@(posedge clk) s_eventually weak(a ##1 b));
endmodule

module strong_bounded_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 1;

	assert property (@(posedge clk) s_eventually strong(a ##1 b));
endmodule

// The operands of a sequence `and`, `intersect`, `within` or `first_match` in a strong
// sequence are matched from the start of one attempt, chosen by the prophecy input. `t`
// counts up to 7 and stays there.

// (a ##[1:$] b) and (c ##[1:$] d) ends on tick 3, with the later operand
module and_intersect_pass ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 1, c = t == 0, d = t == 3, e = t == 0, f = t == 3;
	assert property (@(posedge clk)
		start |-> strong(((a ##[1:$] b) and (c ##[1:$] d)) intersect (e ##[1:$] f)));
endmodule

// Both operands end on tick 1 and the `and` matches there only, not on tick 3
module and_intersect_fail ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 1, c = t == 0, d = t == 1, e = t == 0, f = t == 3;
	assert property (@(posedge clk)
		start |-> strong(((a ##[1:$] b) and (c ##[1:$] d)) intersect (e ##[1:$] f)));
endmodule

module and_within_pass ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 1, c = t == 0, d = t == 3, x = t == 2, y = t == 3;
	assert property (@(posedge clk)
		start |-> strong((x ##[1:$] y) within ((a ##[1:$] b) and (c ##[1:$] d))));
endmodule

// The match of the `and` ends on tick 1, ahead of the one of x ##[1:$] y
module and_within_fail ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 1, c = t == 0, d = t == 1, x = t == 2, y = t == 3;
	assert property (@(posedge clk)
		start |-> strong((x ##[1:$] y) within ((a ##[1:$] b) and (c ##[1:$] d))));
endmodule

module first_match_intersect_pass ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 3 || t == 5, c = t == 0, d = t == 3;
	assert property (@(posedge clk)
		start |-> strong(first_match(a ##[1:$] b) intersect (c ##[1:$] d)));
endmodule

// The first match ends on tick 1, the one on tick 3 is no first match
module first_match_intersect_fail ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 1 || t == 3, c = t == 0, d = t == 3;
	assert property (@(posedge clk)
		start |-> strong(first_match(a ##[1:$] b) intersect (c ##[1:$] d)));
endmodule

module first_match_within_pass ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 3 || t == 5, p = t == 2, q = t == 3;
	assert property (@(posedge clk)
		start |-> strong((p ##[1:$] q) within first_match(a ##[1:$] b)));
endmodule

module first_match_within_fail ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = t == 0, b = t == 1 || t == 5, p = t == 2, q = t == 3;
	assert property (@(posedge clk)
		start |-> strong((p ##[1:$] q) within first_match(a ##[1:$] b)));
endmodule

// An empty match of an operand of `and` puts no constraint on the other one
// (IEEE 1800-2017 F.3.4.2.4)
module and_empty_pass ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = 1'b0, b = t == 0, c = t == 1;
	assert property (@(posedge clk) start |-> strong(a[*0:$] and (b ##[1:$] c)));
endmodule

module and_empty_fail ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = 1'b0, b = t == 0, c = t == 1;
	assert property (@(posedge clk) start |-> strong(a[*1:$] and (b ##[1:$] c)));
endmodule

// An empty match lies within any match
module within_empty_pass ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = 1'b0, b = t == 0, c = t == 1;
	assert property (@(posedge clk) start |-> strong(a[*0:1] within (b ##[1:$] c)));
endmodule

module within_empty_fail ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire a = 1'b0, b = t == 0, c = t == 1;
	assert property (@(posedge clk) start |-> strong(a within (b ##[1:$] c)));
endmodule

// The inner sequence of `within` starts on any tick of the outer one
module within_throughout_pass ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire g = t != 0, a = t == 2, b = t == 3, e = t == 0, f = t == 5;
	assert property (@(posedge clk)
		start |-> strong((g throughout (a ##[1:$] b)) within (e ##[1:$] f)));
endmodule

module within_throughout_fail ((* gclk *) input logic clk);
	logic [2:0] t = 0;
	always_ff @(posedge clk)
		if (t != 7)
			t <= t + 1'b1;
	wire start = t == 0;
	wire g = t != 2, a = t == 2, b = t == 3, e = t == 0, f = t == 5;
	assert property (@(posedge clk)
		start |-> strong((g throughout (a ##[1:$] b)) within (e ##[1:$] f)));
endmodule

// An attempt on every tick. The first match of the attempt on phase 0 ends there,
// while the match of ##[0:$] b ends on phase 2.
module first_match_attempts_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;
	wire a = phase == 0 || phase == 2;
	wire b = phase == 2;
	assert property (@(posedge clk) strong(first_match(##[0:$] a) intersect (##[0:$] b)));
endmodule

module intersect_attempts_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;
	wire a = phase == 0 || phase == 2;
	wire b = phase == 2;
	assert property (@(posedge clk) strong((##[0:$] a) intersect (##[0:$] b)));
endmodule

// `throughout` with no product in it under an eventuality. The attempts see `a`
// on phase 0 and `c` on phase 2 with `d` high in between.
module eventual_throughout_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;
	wire a = phase == 0;
	wire c = phase == 2;
	wire d = phase != 3;
	assert property (@(posedge clk) s_eventually strong(d throughout (a ##[1:$] c)));
endmodule

module eventual_throughout_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;
	wire a = phase == 0;
	wire c = phase == 2;
	wire d = phase != 1;
	assert property (@(posedge clk) s_eventually strong(d throughout (a ##[1:$] c)));
endmodule

// strong(L and R) with a finite L is strong(L) and strong(R), but for an empty
// match of L, which makes one of the `and` with any match of R
module and_empty_finite_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;
	wire b = 1'b0;
	wire c = phase == 2;
	assert property (@(posedge clk) strong(b[*0:1] and (##[0:$] c)));
	assert property (@(posedge clk) strong((##[0:$] c) and b[*0:2]));
endmodule

// That leaves the match of R to be had
module and_empty_finite_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;
	wire b = 1'b0;
	wire c = 1'b0;
	assert property (@(posedge clk) strong(b[*0:1] and (##[0:$] c)));
endmodule

// A non-empty match of the finite operand is still needed where it has no empty one
module and_finite_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;
	wire b = 1'b0;
	wire c = phase == 2;
	assert property (@(posedge clk) strong((##[0:$] c) and b[*1:2]));
endmodule
