// The default clocking applies as well when it declares signals of its own, the
// property reading the module's signals rather than those of the clocking block
module default_clocked_signals(input clk_i, input logic a, input logic b, output logic q);
	reg gate = 0;
	reg a_d1;

	default clocking cb @(posedge clk_i);
		input a, b;
		output q;
	endclocking

	always @(posedge clk_i) begin
		delay_ref: assert(!gate || (a_d1 && b));

		gate <= 1;
		a_d1 <= a;
	end

	delay_test: assert property(a ##1 b);
endmodule
