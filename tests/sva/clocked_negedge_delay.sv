module clocked_negedge_delay(input clk_i, input logic a, input logic b);
	reg gate = 0;
	reg a_d1;

	always @(negedge clk_i) begin
		delay_ref: assert(!gate || (a_d1 && b));

		gate <= 1;
		a_d1 <= a;
	end

	delay_test: assert property(@(negedge clk_i) a ##1 b);
endmodule
