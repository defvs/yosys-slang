// An initial procedure queues one attempt of an assertion with a clock of its own, which
// starts on the first tick of that clock (IEEE Std 1800-2017 section 16.14.6). The
// reference queues an attempt on the first tick alone.
module initial_own_clock(input logic clk_i, input logic a, input logic b);
	reg first = 1;
	always @(negedge clk_i)
		first <= 0;

	always @(negedge clk_i)
		if (first)
			own_ref: assert property (a ##1 b);

	initial
		own_test: assert property (@(negedge clk_i) a ##1 b);
endmodule
