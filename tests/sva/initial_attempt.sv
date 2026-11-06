// An initial procedure infers no clock, so a concurrent assertion in it takes its own
// clock or the default clocking. The procedure queues one attempt at time 0, and that
// attempt starts on the first tick of the clock (IEEE Std 1800-2017 section 16.14.6).
// The references queue an attempt on the first tick alone.
module initial_attempt(input logic clk_i, input logic clk2_i, input logic a, input logic b,
					   input logic c);
	default clocking @(posedge clk_i); endclocking

	reg first = 1;
	always @(posedge clk_i)
		first <= 0;

	reg first2 = 1;
	always @(posedge clk2_i)
		first2 <= 0;

	logic known = 1;

	always @(posedge clk_i) begin
		if (first) begin
			boolean_ref: assert property (a);
			sequence_ref: assert property (a ##1 b);
			implication_ref: assert property (a |=> b ##1 c);
			cover_ref: cover property (a ##1 !b);
			assume_ref: assume property (c |-> ##2 b);
			disable_ref: assert property (disable iff (c) a |=> b);
			known_ref: assert property (b |=> c);
		end
	end

	always @(posedge clk2_i)
		if (first2)
			own_clock_ref: assert property (a |=> b);

	initial begin
		boolean_test: assert property (a);
		sequence_test: assert property (a ##1 b);
		implication_test: assert property (a |=> b ##1 c);
		cover_test: cover property (a ##1 !b);
		assume_test: assume property (c |-> ##2 b);
		disable_test: assert property (disable iff (c) a |=> b);
		// A condition on a value known at time 0
		if (known)
			known_test: assert property (b |=> c);
		own_clock_test: assert property (@(posedge clk2_i) a |=> b);
	end
endmodule
