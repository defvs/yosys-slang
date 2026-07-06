module named_clocked_property(input clk_i, input logic a, input logic b);
	reg gate = 0;
	reg a_d1;

	property delayed_p;
		@(posedge clk_i) a ##1 b;
	endproperty

	always @(posedge clk_i) begin
		delayed_ref: assert(!gate || (a_d1 && b));

		gate <= 1;
		a_d1 <= a;
	end

	delayed_test: assert property (delayed_p);
endmodule
