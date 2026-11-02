// Annex F of IEEE Std 1800-2017 for the aborts around liveness properties: the
// letters from the tick of a disable iff or an accept_on are T, and the
// antecedent of an implication sees them dual. A failure settled ahead of the
// abort stands, a pending obligation is met by T letters.

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
