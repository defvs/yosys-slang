// Models with two justice properties and a fairness constraint, which suprove
// gives no result for when it is handed them together. The wrapper proves each
// justice property on its own under every fairness constraint.

module justice_fair_pass (
	(* gclk *) input logic clk,
	input logic progress
);
	logic seen = 0;
	always_ff @(posedge clk)
		seen <= seen | progress;

	// Both hold only on the fair paths
	assume property (@(posedge clk) s_eventually progress);
	assert property (@(posedge clk) s_eventually seen);
	assert property (@(posedge clk) s_eventually (seen | progress));
endmodule

module justice_first_fail (
	(* gclk *) input logic clk,
	input logic progress
);
	logic seen = 0;
	always_ff @(posedge clk)
		seen <= seen | progress;

	assume property (@(posedge clk) s_eventually progress);
	assert property (@(posedge clk) s_eventually !seen);
	assert property (@(posedge clk) s_eventually seen);
endmodule

module justice_second_fail (
	(* gclk *) input logic clk,
	input logic progress
);
	logic seen = 0;
	always_ff @(posedge clk)
		seen <= seen | progress;

	assume property (@(posedge clk) s_eventually progress);
	assert property (@(posedge clk) s_eventually seen);
	assert property (@(posedge clk) s_eventually !seen);
endmodule

// A selected attempt brings its own fairness next to the second obligation
module justice_selected_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire a = age == 1;
	wire b = age == 2;
	wire c = age == 3;

	assert property (@(posedge clk)
		age == 0 |=> strong(a ##[1:$] b) and s_eventually c);
endmodule

module justice_selected_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire a = age == 1;
	wire b = age == 2;
	wire c = 1'b0;

	assert property (@(posedge clk)
		age == 0 |=> strong(a ##[1:$] b) and s_eventually c);
endmodule
