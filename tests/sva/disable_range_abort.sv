module disable_range_abort(input clk_i, input logic rst, input logic a, input logic b);
	reg gate1 = 0;
	reg gate2 = 0;
	reg rst_d1;
	reg rst_d2;
	reg a_d1;
	reg a_d2;
	reg b_d1;

	always @(posedge clk_i) begin
		if (gate2 && !rst_d2 && !rst_d1 && !rst)
			range_ref: assert(a_d2 && (b_d1 || b));

		gate1 <= 1;
		gate2 <= gate1;
		rst_d1 <= rst;
		rst_d2 <= rst_d1;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
	end

	range_test: assert property(@(posedge clk_i) disable iff (rst) a ##[1:2] b);
endmodule
