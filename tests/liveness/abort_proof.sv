// Annex F of IEEE Std 1800-2017 for the aborts around liveness properties: the
// letters from the tick of a disable iff or an accept_on are T, those of a
// reject_on their dual, and the antecedent of an implication sees them dual. A
// failure settled ahead of the abort stands, a pending obligation is met by T
// letters.

// A selected attempt of a regular sequence fails on its first tick, ahead of the
// disable on the third
module selected_settled_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = 1'b0, b = 1'b0, r = t == 2;

	assert property (@(posedge clk)
		disable iff (r) start |-> strong(a ##[1:$] b));
endmodule

module selected_disabled_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 0, b = 1'b0, r = t == 2;

	assert property (@(posedge clk)
		disable iff (r) start |-> strong(a ##[1:$] b));
endmodule

// The accept settles the attempt on the second tick, though the T letters
// complete a match only on the fifth, after the reject
module selected_accept_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 0, b = 1'b0, c = 1'b0, x = t == 1, q = t == 3;

	assert property (@(posedge clk)
		start |-> reject_on (q) accept_on (x) strong(a ##[1:$] b ##3 c));
endmodule

module selected_reject_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 0, b = 1'b0, c = 1'b0, x = t == 3, q = t == 2;

	assert property (@(posedge clk)
		start |-> reject_on (q) accept_on (x) strong(a ##[1:$] b ##3 c));
endmodule

// A reject during the minimum delay of a selected eventuality
module selected_minimum_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire q = t == 1, a = t == 3, b = t == 4;

	assert property (@(posedge clk)
		start |-> reject_on (q) s_eventually [2:$] strong(a ##[1:$] b));
endmodule

module selected_minimum_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire q = t == 5, a = t == 3, b = t == 4;

	assert property (@(posedge clk)
		start |-> reject_on (q) s_eventually [2:$] strong(a ##[1:$] b));
endmodule

// An antecedent sees the letters of a reject_on dual, so it matches from the
// tick of the reject: a reject on the first tick fails even an attempt that
// would be vacuous
module reject_vacuous_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = 1'b0, b = 1'b0, q = t == 0;

	assert property (@(posedge clk)
		start |-> reject_on (q) (a |-> s_eventually b));
endmodule

module reject_vacuous_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = 1'b0, b = 1'b0, q = t == 1;

	assert property (@(posedge clk)
		start |-> reject_on (q) (a |-> s_eventually b));
endmodule

// Of aborts firing on the same tick, the outer one wins, also on the tick the
// consequent of `|=>` starts on
module reject_accept_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire g = t == 3, x = t == 0, q = t == 0;

	assert property (@(posedge clk)
		start |-> reject_on (q) accept_on (x) s_eventually g);
endmodule

module accept_reject_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire g = 1'b0, x = t == 1, q = t == 1;

	assert property (@(posedge clk)
		start |-> accept_on (x) reject_on (q) s_eventually g);
endmodule

module reject_next_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 0, b = 1'b0, x = t == 1, q = t == 1;

	assert property (@(posedge clk)
		start |-> reject_on (q) accept_on (x) (a |=> s_eventually b));
endmodule

module reject_next_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 0, b = 1'b0, x = t == 1, q = t == 2;

	assert property (@(posedge clk)
		start |-> reject_on (q) accept_on (x) (a |=> s_eventually b));
endmodule

// The consequent of a regular antecedent fails on the third tick, ahead of
// the disable
module antecedent_settled_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 1, b = t == 2, c = 1'b0, d = 1'b1, r = t == 3;

	assert property (@(posedge clk)
		disable iff (r) (start ##1 a[*1:$] ##1 b) |-> (c ##1 d));
endmodule

module antecedent_disabled_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 1, b = t == 2, c = 1'b1, d = 1'b0, r = t == 3;

	assert property (@(posedge clk)
		disable iff (r) (start ##1 a[*1:$] ##1 b) |-> (c ##1 d));
endmodule

// The left side of a followed-by fails on the first tick, ahead of the disable
module followed_settled_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = 1'b0, b = 1'b0, c = 1'b0, r = t == 1;

	assert property (@(posedge clk)
		disable iff (r) start |-> ((a ##2 b) #-# s_eventually c));
endmodule

module followed_disabled_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t == 0, b = 1'b0, c = 1'b0, r = t == 1;

	assert property (@(posedge clk)
		disable iff (r) start |-> ((a ##2 b) #-# s_eventually c));
endmodule

// A disable on the last tick of `not always [1:2]` gives it T letters, the
// dual of which fail `a`
module not_always_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = 1'b1, r = t == 2;

	assert property (@(posedge clk)
		disable iff (r) start |-> not (always [1:2] a));
endmodule

module not_always_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = 1'b1, r = t == 3;

	assert property (@(posedge clk)
		disable iff (r) start |-> not (always [1:2] a));
endmodule

// A finite strong sequence fails ahead of the disable, or is in flight
module strong_settled_fail ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = t != 0, b = 1'b1, r = t == 1;

	assert property (@(posedge clk)
		disable iff (r) strong(a ##1 b));
endmodule

module strong_disabled_pass ((* gclk *) input logic clk);
	logic [3:0] t = 0;
	always_ff @(posedge clk)
		if (t != 15)
			t <= t + 1'b1;

	wire start = t == 0;
	wire a = 1'b1, b = t != 1, r = t == 1;

	assert property (@(posedge clk)
		disable iff (r) strong(a ##1 b));
endmodule
