module disable_one_cycle(input clk_i, input logic rst, input logic a);
	always @(posedge clk_i) begin
		if (!rst)
			disable_ref: assert(a);
	end

	disable_test: assert property(@(posedge clk_i) disable iff (rst) a);
endmodule
