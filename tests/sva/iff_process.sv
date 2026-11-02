module iff_process(input clk_i, input logic en, input logic a, input logic b);
	reg valid = 0;
	reg a_q;

	always_ff @(posedge clk_i iff en) begin
		delay_ref: assert(!valid || !a_q || b);
		delay_test: assert property(a |=> b);

		valid <= 1;
		a_q <= a;
	end
endmodule
