// The progress part of strong until, and liveness under `implies`, a property
// `if` and a property `case`, whose conditions are sampled when the attempt
// starts. until_pass and until_with_pass from until_proof.sv are proven here
// for their progress part too.

module until_progress_fail ((* gclk *) input logic clk);
	wire hold = 1'b1;
	wire released = 1'b0;

	// The left operand never fails, the right one never comes
	assert property (@(posedge clk) hold s_until released);
endmodule

module until_with_progress_fail ((* gclk *) input logic clk);
	wire hold = 1'b1;
	wire released = 1'b0;

	assert property (@(posedge clk) hold s_until_with released);
endmodule

module implies_live_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire grant = age == 2;

	assert property (@(posedge clk) request implies s_eventually grant);
endmodule

module implies_live_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 2;
	wire grant = age == 1;

	assert property (@(posedge clk) request implies s_eventually grant);
endmodule

module if_live_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire select = age == 0;
	wire grant = age == 2;
	wire other = 1'b0;

	// The branch is chosen on the tick of the request
	assert property (@(posedge clk)
		request |-> if (select) s_eventually grant else s_eventually other);
endmodule

module if_live_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire select = age == 1;
	wire grant = age == 2;
	wire other = 1'b0;

	assert property (@(posedge clk)
		request |-> if (select) s_eventually grant else s_eventually other);
endmodule

module case_live_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire [1:0] mode = age[1:0];
	wire a = age == 3;
	wire b = 1'b0;

	assert property (@(posedge clk)
		request |-> case (mode) 2'd0: s_eventually a; default: s_eventually b; endcase);
endmodule

module case_live_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire request = age == 0;
	wire [1:0] mode = age[1:0] + 2'd1;
	wire a = age == 3;
	wire b = 1'b0;

	assert property (@(posedge clk)
		request |-> case (mode) 2'd0: s_eventually a; default: s_eventually b; endcase);
endmodule
