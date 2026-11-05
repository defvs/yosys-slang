// The clock of `always @(posedge clk iff en)` stops for good, here after the ticks
// with n = 0, 1 and 2 or n = 0 to 4. A strong property is met on a tick, so an
// attempt still pending on the last tick fails, even where the match that ends on
// that tick meets an older attempt (IEEE Std 1800-2017 sections 16.12.13 and 16.14.6).

// The attempt of tick 1 needs b on tick 3 or later, there is none
module stop_minimum_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 1;
	wire b = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually [2:$] b);
endmodule

// The attempt of tick 0 alone, which b on tick 2 meets
module stop_minimum_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0;
	wire b = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually [2:$] b);
endmodule

// The attempt of tick 1 alone
module stop_minimum_single_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 1;
	wire b = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually [2:$] b);
endmodule

// Ticks 0 to 4, the attempts of ticks 2 and 3, b on tick 4 meets the first one only
module stop_minimum_late_fail ((* gclk *) input logic clk);
	logic [2:0] n = 0;
	always_ff @(posedge clk)
		if (n != 5)
			n <= n + 1'b1;
	wire en = n != 5;
	wire a = n == 2 || n == 3;
	wire b = n == 4;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually [2:$] b);
endmodule

// The clock keeps ticking, b never comes again
module stop_minimum_ticking_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = 1;
	wire a = n == 0 || n == 1;
	wire b = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually [2:$] b);
endmodule

// The b ##1 c match over ticks 1 and 2 starts ahead of the attempt of tick 2
module stop_sequence_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 2;
	wire b = n == 1;
	wire c = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually (b ##1 c));
endmodule

// The attempts of ticks 0 and 1, which the match meets both
module stop_sequence_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 1;
	wire b = n == 1;
	wire c = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually (b ##1 c));
endmodule

module stop_or_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 2;
	wire b = n == 1;
	wire c = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> (s_eventually (b ##1 c)) or (s_eventually [1:$] c));
endmodule

// The attempt of tick 1 sees c a tick later
module stop_or_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 1;
	wire b = n == 1;
	wire c = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> (s_eventually (b ##1 c)) or (s_eventually [1:$] c));
endmodule

module stop_not_always_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 2;
	wire b = n != 2;

	always @(posedge clk iff en)
		assert property (a |-> not always [1:$] b);
endmodule

module stop_not_always_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 1;
	wire b = n != 2;

	always @(posedge clk iff en)
		assert property (a |-> not always [1:$] b);
endmodule

module stop_strong_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 2;
	wire b = n == 1;
	wire c = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> strong(##[0:$] b ##1 c));
endmodule

module stop_strong_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 1;
	wire b = n == 1;
	wire c = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> strong(##[0:$] b ##1 c));
endmodule

// An attempt starts on each tick, those of ticks 3 and 4 are left pending
module stop_top_fail ((* gclk *) input logic clk);
	logic [2:0] n = 0;
	always_ff @(posedge clk)
		if (n != 5)
			n <= n + 1'b1;
	wire en = n != 5;
	wire g = n == 4;

	always @(posedge clk iff en)
		assert property (s_eventually [2:$] g);
endmodule

// A goal with no duration takes a monitor of its own for each attempt, the one of
// tick 2 needs b on tick 2 or later
module stop_zero_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 2;
	wire b = n == 1;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually b);
endmodule

module stop_zero_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire en = n != 3;
	wire a = n == 0 || n == 2;
	wire b = n == 2;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually b);
endmodule

// The clock pauses on n = 3 and 4 with the attempt of n = 2 pending, then meets it
// on n = 6, two ticks later
module pause_pass ((* gclk *) input logic clk);
	logic [2:0] n = 0;
	always_ff @(posedge clk)
		n <= n + 1'b1;
	wire en = n != 3 && n != 4;
	wire a = n == 1 || n == 2;
	wire b = n == 6;

	always @(posedge clk iff en)
		assert property (a |-> s_eventually [2:$] b);
	always @(posedge clk iff en)
		assert property (s_eventually [2:$] b);
endmodule

// The ticks it takes before the attempt of the latest trigger can be met
module stop_assume_pass ((* gclk *) input logic clk, input logic en, a, b);
	logic [1:0] need = 0;
	always_ff @(posedge clk)
		if (en)
			need <= a ? 2'd2 : need - (need != 0);

	// The assumption rules out a clock that stops with an attempt pending
	always @(posedge clk iff en)
		assume property (a |-> s_eventually [2:$] b);

	assert property (@(posedge clk) need != 0 |-> s_eventually en);
endmodule

// Without an attempt pending the clock may stop all the same
module stop_assume_fail ((* gclk *) input logic clk, input logic en, a, b);
	always @(posedge clk iff en)
		assume property (a |-> s_eventually [2:$] b);

	assert property (@(posedge clk) s_eventually en);
endmodule
