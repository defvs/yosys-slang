module pr_disable(input clk_i, input logic rst_n, input logic [3:0] data);
	always @(posedge clk_i) begin
		if (rst_n)
			disable_ref: assert(data != 4'hf);
	end

	disable_test: assert property(@(posedge clk_i) disable iff (!rst_n) data != 4'hf);
endmodule
