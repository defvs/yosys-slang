module clocked_nested_after_implication(input clk_i, input logic a, input logic b,
										input logic c);
	reg gate1 = 0;
	reg gate2 = 0;
	reg a_d1;
	reg a_d2;
	reg b_d1;

	always @(posedge clk_i) begin
		nested_ref: assert(!gate2 || !a_d2 || (b_d1 && c));

		gate1 <= 1;
		gate2 <= gate1;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
	end

	nested_test: assert property(@(posedge clk_i) a |=> b ##1 c);
endmodule
