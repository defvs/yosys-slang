module clocked_negedge_range(input clk_i, input logic a, input logic b);
	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg a_d1;
	reg a_d2;
	reg a_d3;
	reg b_d1;
	reg b_d2;

	always @(negedge clk_i) begin
		range_ref: assert(!gate3 || (a_d3 && (b_d2 || b_d1 || b)));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		a_d1 <= a;
		a_d2 <= a_d1;
		a_d3 <= a_d2;
		b_d1 <= b;
		b_d2 <= b_d1;
	end

	range_test: assert property(@(negedge clk_i) a ##[1:3] b);
endmodule
