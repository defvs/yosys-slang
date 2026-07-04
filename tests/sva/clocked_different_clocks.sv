module clocked_different_clocks(input clk_a, input clk_b, input logic a, input logic b,
								input logic c, input logic d);
	reg gate_a = 0;
	reg gate_b = 0;
	reg a_d1;
	reg c_d1;

	always @(posedge clk_a) begin
		delay_a_ref: assert(!gate_a || (a_d1 && b));

		gate_a <= 1;
		a_d1 <= a;
	end

	always @(posedge clk_b) begin
		delay_b_ref: assert(!gate_b || (c_d1 && d));

		gate_b <= 1;
		c_d1 <= c;
	end

	delay_a_test: assert property(@(posedge clk_a) a ##1 b);
	delay_b_test: assert property(@(posedge clk_b) c ##1 d);
endmodule
