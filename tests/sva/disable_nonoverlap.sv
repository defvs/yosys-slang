module disable_nonoverlap(input clk_i, input logic rst, input logic a, input logic b);
	reg gate = 0;
	reg rst_d1;
	reg a_d1;

	always @(posedge clk_i) begin
		if (gate && !rst_d1 && !rst)
			nonoverlap_ref: assert(!a_d1 || b);

		gate <= 1;
		rst_d1 <= rst;
		a_d1 <= a;
	end

	nonoverlap_test: assert property(@(posedge clk_i) disable iff (rst) a |=> b);
endmodule
