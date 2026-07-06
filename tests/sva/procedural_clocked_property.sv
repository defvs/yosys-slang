module procedural_clocked_property(input clk_i, input logic a, input logic b);
	reg gate = 0;
	reg a_d1;

	always_ff @(posedge clk_i) begin
		procedural_ref: assert(!gate || (a_d1 && b));
		procedural_test: assert property (@(posedge clk_i) a ##1 b);

		gate <= 1;
		a_d1 <= a;
	end
endmodule
