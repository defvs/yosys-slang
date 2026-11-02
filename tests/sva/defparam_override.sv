module defparam_override_sub #(parameter int LIMIT = 5) (input logic clk_i, input logic [7:0] x);
	always @(posedge clk_i)
		limit_ref: assert(x <= 8'd3);

	limit_test: assert property(@(posedge clk_i) x <= LIMIT);
endmodule

module defparam_override(input logic clk_i, input logic [7:0] x);
	defparam_override_sub u(.*);
	defparam u.LIMIT = 3;
endmodule
