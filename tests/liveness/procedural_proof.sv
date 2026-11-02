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
			assert property (s_eventually (a ##[1:$] b));
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
			assert property (s_eventually (a ##[1:$] b));
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
