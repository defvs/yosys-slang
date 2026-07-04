module clocked_nonoverlap(input clk_i, input logic a, input logic b);
	reg gate = 0;
	reg a_d1;

	always @(posedge clk_i) begin
		nonoverlap_ref: assert(!gate || !a_d1 || b);

		gate <= 1;
		a_d1 <= a;
	end

	nonoverlap_test: assert property(@(posedge clk_i) a |=> b);
endmodule
