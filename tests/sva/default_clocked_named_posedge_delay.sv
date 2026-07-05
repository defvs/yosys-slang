module default_clocked_named_posedge_delay(input clk_i, input logic a, input logic b);
	reg gate = 0;
	reg a_d1;

	clocking cb @(posedge clk_i); endclocking
	default clocking cb;

	always @(posedge clk_i) begin
		delay_ref: assert(!gate || (a_d1 && b));

		gate <= 1;
		a_d1 <= a;
	end

	delay_test: assert property(a ##1 b);
endmodule
