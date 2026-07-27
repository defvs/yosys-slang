module async_reset_no_else(
	input logic clk,
	input logic rst_n,
	output logic q
);
	always_ff @(posedge clk or negedge rst_n)
		if (!rst_n)
			q <= 1'b0;
endmodule

module async_reset_composite_condition(
	input logic clk,
	input logic rst_n,
	input logic enable,
	input logic d,
	output logic q
);
	always_ff @(posedge clk or negedge rst_n)
		if (!rst_n || enable)
			q <= d;
endmodule

module async_inverted_event(
	input logic clk,
	input logic rst_n,
	input logic d,
	output logic q
);
	always_ff @(posedge clk or posedge !rst_n)
		if (!rst_n)
			q <= 1'b0;
		else
			q <= d;
endmodule
