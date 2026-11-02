module hierarchical_constants_sub #(parameter int LIMIT = 5, parameter int ARR [2] = '{5, 9}) ();
	typedef enum logic [1:0] { IDLE = 2'd1, BUSY = 2'd2 } state_e;
endmodule

module hierarchical_constants(input logic clk_i, input logic [7:0] x, input logic [1:0] st);
	hierarchical_constants_sub u();
	defparam u.LIMIT = 3;

	always @(posedge clk_i) begin
		param_ref: assert(x == 8'd3);
		array_ref: assert(x == 8'd9);
		enum_ref: assert(st == 2'd2);
	end

	param_test: assert property(@(posedge clk_i) x == u.LIMIT);
	array_test: assert property(@(posedge clk_i) x == u.ARR[1]);
	enum_test: assert property(@(posedge clk_i) st == u.BUSY);
endmodule
