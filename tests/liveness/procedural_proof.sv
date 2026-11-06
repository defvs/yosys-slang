// Concurrent assertions in procedural code. The condition around the assertion
// decides on the tick the statement is reached whether an attempt starts, the
// attempt then runs to completion regardless (IEEE Std 1800-2017 section 16.14.6).

module guard_response_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	// The one attempt starts at age 2, when the a ##1 b match does
	wire c = age == 2;
	wire a = age == 2;
	wire b = age == 3;

	always @(posedge clk)
		if (c)
			assert property (s_eventually (a ##1 b));
endmodule

module guard_response_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	// The one attempt starts at age 3, the only a ##1 b match a tick earlier
	wire c = age == 3;
	wire a = age == 2;
	wire b = age == 3;

	always @(posedge clk)
		if (c)
			assert property (s_eventually (a ##1 b));
endmodule

module guard_selected_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire c = age == 2;
	wire a = age == 2;
	wire b = age == 5;

	always @(posedge clk)
		if (c)
			assert property (s_eventually strong(a ##[1:$] b));
endmodule

module guard_selected_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire c = age == 3;
	wire a = age == 2;
	wire b = age == 5;

	always @(posedge clk)
		if (c)
			assert property (s_eventually strong(a ##[1:$] b));
endmodule

module guard_or_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	// The a ##1 b match starts before the attempt, the d ##1 e one after it
	wire c = age == 3;
	wire a = age == 2;
	wire b = age == 3;
	wire d = age == 4;
	wire e = age == 5;

	always @(posedge clk)
		if (c)
			assert property ((s_eventually (a ##1 b)) or (s_eventually (d ##1 e)));
endmodule

module guard_or_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	// Both matches start before the attempt
	wire c = age == 3;
	wire a = age == 2;
	wire b = age == 3;
	wire d = age == 1;
	wire e = age == 2;

	always @(posedge clk)
		if (c)
			assert property ((s_eventually (a ##1 b)) or (s_eventually (d ##1 e)));
endmodule

module guard_or_mixed_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire c = age == 3;
	wire a = age == 2;
	wire d = age == 4;
	wire e = age == 5;

	always @(posedge clk)
		if (c)
			assert property ((s_eventually a) or (s_eventually (d ##1 e)));
endmodule

module guard_or_mixed_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire c = age == 3;
	wire a = age == 2;
	wire d = age == 2;
	wire e = age == 3;

	always @(posedge clk)
		if (c)
			assert property ((s_eventually a) or (s_eventually (d ##1 e)));
endmodule

module guard_assume_pass (
	(* gclk *) input logic clk,
	input logic g,
	input logic h
);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	// The assumption is about the attempt at age 3 only, a g ##1 h match
	// starting before it doesn't meet it
	wire c = age == 3;

	always @(posedge clk)
		if (c)
			assume property (s_eventually (g ##1 h));

	assert property (@(posedge clk) c |-> s_eventually (g ##1 h));
endmodule

module guard_assume_fail (
	(* gclk *) input logic clk,
	input logic g,
	input logic h
);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	// Nothing is assumed about the ticks after age 3
	wire c = age == 3;

	always @(posedge clk)
		if (c)
			assume property (s_eventually (g ##1 h));

	assert property (@(posedge clk) (age == 5) |-> s_eventually (g ##1 h));
endmodule

module sampled_if_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire g = age == 2;

	// The condition is the sampled value of v, high on the first tick only
	logic v = 1'b1;
	always @(posedge clk) begin
		v = 1'b0;
		assert property (if (v) s_eventually g);
	end
endmodule

module sampled_if_fail ((* gclk *) input logic clk);
	wire g = 1'b0;

	logic v = 1'b1;
	always @(posedge clk) begin
		v = 1'b0;
		assert property (if (v) s_eventually g);
	end
endmodule

module comb_default_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	default clocking @(posedge clk); endclocking

	wire c = age == 2;
	wire a = age == 2;
	wire b = age == 3;

	always_comb
		if (c)
			assert property (s_eventually (a ##1 b));
endmodule

module comb_default_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	default clocking @(posedge clk); endclocking

	wire c = age == 3;
	wire a = age == 2;
	wire b = age == 3;

	always_comb
		if (c)
			assert property (s_eventually (a ##1 b));
endmodule

// The clock of `always @(posedge clk iff en)` ticks only where en is high, the
// goals don't count on the other cycles

module iff_zero_pass ((* gclk *) input logic clk);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	wire g = en;

	always @(posedge clk iff en)
		assert property (s_eventually g);
endmodule

module iff_zero_fail ((* gclk *) input logic clk);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	wire g = !en;

	always @(posedge clk iff en)
		assert property (s_eventually g);
endmodule

module iff_minimum_pass ((* gclk *) input logic clk);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	wire g = en;

	always @(posedge clk iff en)
		assert property (s_eventually [2:$] g);
endmodule

module iff_minimum_fail ((* gclk *) input logic clk);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	wire g = !en;

	always @(posedge clk iff en)
		assert property (s_eventually [2:$] g);
endmodule

module iff_response_pass ((* gclk *) input logic clk, input logic r);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	wire g = en;

	always @(posedge clk iff en)
		assert property (r |-> s_eventually g);
endmodule

module iff_response_fail ((* gclk *) input logic clk, input logic r);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	wire g = !en;

	always @(posedge clk iff en)
		assert property (r |-> s_eventually g);
endmodule

module iff_fair_pass ((* gclk *) input logic clk, input logic g);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	// The assumption promises g on infinitely many ticks of the process clock
	always @(posedge clk iff en)
		assume property (s_eventually g);

	assert property (@(posedge clk) s_eventually (en && g));
endmodule

module iff_fair_fail ((* gclk *) input logic clk, input logic g);
	logic en = 0;
	always_ff @(posedge clk)
		en <= !en;

	always @(posedge clk iff en)
		assume property (s_eventually g);

	assert property (@(posedge clk) s_eventually (!en && g));
endmodule

// Each tick from the one the attempt of `always P` starts on, an attempt of P starts
module guard_always_pass ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	logic first = 1;
	always_ff @(posedge clk) begin
		cnt <= cnt + 1'b1;
		first <= 0;
	end

	always @(posedge clk)
		if (first)
			assert property (always ((cnt == 1) |-> s_eventually (cnt == 5)));
endmodule

// The attempt of P on the second tick with cnt == 1 never sees cnt == 5 unwrapped
module guard_always_fail ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	logic first = 1;
	logic wrapped = 0;
	always_ff @(posedge clk) begin
		cnt <= cnt + 1'b1;
		first <= 0;
		if (cnt == 7)
			wrapped <= 1;
	end

	always @(posedge clk)
		if (first)
			assert property (always ((cnt == 1) |-> s_eventually (cnt == 5 && !wrapped)));
endmodule

// The attempts of P start on the ticks 2 and 3 only, past the one with cnt == 1
module guard_ranged_always_pass ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	logic first = 1;
	always_ff @(posedge clk) begin
		cnt <= cnt + 1'b1;
		first <= 0;
	end

	always @(posedge clk)
		if (first)
			assert property (always [2:3] ((cnt == 1) |-> s_eventually first));
endmodule

module guard_ranged_always_fail ((* gclk *) input logic clk);
	logic [2:0] cnt = 0;
	logic first = 1;
	always_ff @(posedge clk) begin
		cnt <= cnt + 1'b1;
		first <= 0;
	end

	always @(posedge clk)
		if (first)
			assert property (always [1:2] ((cnt == 1) |-> s_eventually first));
endmodule

// An initial procedure queues one attempt, which starts on the first tick of its clock
// (IEEE Std 1800-2017 section 16.14.6)
module initial_eventually_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire b = n == 2;

	default clocking @(posedge clk); endclocking
	initial
		assert property (s_eventually b);
endmodule

module initial_eventually_fail ((* gclk *) input logic clk, input logic b);
	default clocking @(posedge clk); endclocking
	initial
		assert property (s_eventually b);
endmodule

// The antecedent is read on the first tick only, a later `a` starts no attempt
module initial_single_pass ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire a = n == 2;
	wire b = 1'b0;

	default clocking @(posedge clk); endclocking
	initial
		assert property (a |-> s_eventually b);
endmodule

module initial_single_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire a = n == 0;
	wire b = 1'b0;

	default clocking @(posedge clk); endclocking
	initial
		assert property (a |-> s_eventually b);
endmodule

// `always` in the attempt starts the response on every tick
module initial_always_fail ((* gclk *) input logic clk);
	logic [1:0] n = 0;
	always_ff @(posedge clk)
		if (n != 3)
			n <= n + 1'b1;
	wire a = n == 2;
	wire b = 1'b0;

	default clocking @(posedge clk); endclocking
	initial
		assert property (always (a |-> s_eventually b));
endmodule

module initial_always_pass ((* gclk *) input logic clk, input logic a);
	logic b = 0;
	always_ff @(posedge clk)
		b <= a;

	default clocking @(posedge clk); endclocking
	initial
		assert property (always (a |-> s_eventually b));
endmodule

module initial_until_fail ((* gclk *) input logic clk, input logic b);
	default clocking @(posedge clk); endclocking
	initial
		assert property (1'b1 s_until b);
endmodule

// The assumption keeps the response coming
module initial_assume_pass ((* gclk *) input logic clk, input logic b);
	logic seen = 0;
	always_ff @(posedge clk)
		seen <= seen || b;

	default clocking @(posedge clk); endclocking
	initial
		assume property (s_eventually b);
	assert property (s_eventually seen);
endmodule

module initial_assume_fail ((* gclk *) input logic clk, input logic b);
	logic seen = 0;
	always_ff @(posedge clk)
		seen <= b;

	default clocking @(posedge clk); endclocking
	initial
		assume property (s_eventually b);
	assert property (s_eventually seen);
endmodule
