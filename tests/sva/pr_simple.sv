module pr_simple(input clk_i, input logic x);
	always @(posedge clk_i) begin
		simple_ref: assert(x);
	end

	simple_test: assert property(@(posedge clk_i) x);
endmodule
