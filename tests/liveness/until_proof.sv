module until_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire released = age >= 3;
	wire hold = age < 3;

	assert property (@(posedge clk) hold s_until released);
endmodule

module until_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire released = age >= 3;
	wire hold = age == 0;

	assert property (@(posedge clk) hold s_until released);
endmodule

module until_with_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire released = age >= 3;
	wire hold = 1'b1;

	assert property (@(posedge clk) hold s_until_with released);
endmodule

module until_with_fail ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;

	wire released = age >= 3;
	wire hold = age < 3;

	assert property (@(posedge clk) hold s_until_with released);
endmodule
