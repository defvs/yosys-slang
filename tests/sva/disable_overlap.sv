module disable_overlap(input clk_i, input logic rst, input logic a, input logic b);
	always @(posedge clk_i) begin
		if (!rst)
			overlap_ref: assert(!a || b);
	end

	overlap_test: assert property(@(posedge clk_i) disable iff (rst) a |-> b);
endmodule
