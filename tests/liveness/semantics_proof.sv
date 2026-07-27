module disable_abort_pass ((* gclk *) input logic clk);
	logic [1:0] age = 0;
	always_ff @(posedge clk)
		if (age != 3)
			age <= age + 1'b1;

	wire request = age == 0;
	wire reset = age >= 2;
	wire grant = 1'b0;

	assert property (@(posedge clk)
		disable iff (reset) request |-> s_eventually grant);
endmodule

module disable_abort_fail ((* gclk *) input logic clk);
	logic [1:0] age = 0;
	always_ff @(posedge clk)
		if (age != 3)
			age <= age + 1'b1;

	wire request = age == 0;
	wire grant = 1'b0;

	assert property (@(posedge clk)
		disable iff (1'b0) request |-> s_eventually grant);
endmodule

module minimum_delay_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire grant = age == 3;

	assert property (@(posedge clk)
		request |-> s_eventually [2:$] grant);
endmodule

module minimum_delay_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire grant = age == 1;

	assert property (@(posedge clk)
		request |-> s_eventually [2:$] grant);
endmodule

module fairness_pass (
	(* gclk *) input logic clk,
	input logic progress
);
	assume property (@(posedge clk) s_eventually progress);
	assert property (@(posedge clk) s_eventually progress);
endmodule

module fairness_fail (
	(* gclk *) input logic clk,
	input logic progress,
	input logic stuck
);
	assume property (@(posedge clk) !stuck);
	assume property (@(posedge clk) s_eventually progress);
	assert property (@(posedge clk) s_eventually stuck);
endmodule

module suffix_overlap_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 1;
	wire a = age == 1;
	wire b = age == 2;

	assert property (@(posedge clk)
		request |-> s_eventually (a ##1 b));
endmodule

module suffix_pretrigger_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	// This a ##1 b match starts before the request.  It must not satisfy
	// the response obligation triggered alongside b.
	wire request = age == 1;
	wire a = age == 0;
	wire b = age == 1;

	assert property (@(posedge clk)
		request |-> s_eventually (a ##1 b));
endmodule

module overlapping_requests_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age <= 1;
	wire a = age == 1;
	wire b = age == 2;

	// The match starting at age 1 discharges both requests.
	assert property (@(posedge clk)
		request |-> s_eventually (a ##1 b));
endmodule

module reject_abort_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire grant = age == 1;
	wire reject = age == 2;

	assert property (@(posedge clk)
		request |-> reject_on (reject) s_eventually grant);
endmodule

module reject_abort_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire grant = 1'b0;
	wire reject = age == 1;

	assert property (@(posedge clk)
		request |-> reject_on (reject) s_eventually grant);
endmodule
