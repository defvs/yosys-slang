// An attempt of `always P` starts one of P on each tick of its interval. Those of the
// attempts that overlap meet on the same ticks, and the attempts of P a disable came
// upon on the way are accepted, as is each of the always attempts it is part of.
module always_property(input clk_i, input logic en, input logic r, input logic a,
					   input logic b);
	reg w_d1 = 0;
	reg en_d1 = 0, en_d2 = 0, en_d3 = 0;
	reg r_d1 = 0, r_d2 = 0, r_d3 = 0;
	reg a_d1 = 0, a_d2 = 0;
	reg b_d1 = 0;

	sequence s;
		a;
	endsequence

	always @(posedge clk_i) begin
		// The response fails where the request of two ticks before saw no `b` since
		response_ref: assert(!a_d2 || b_d1 || b);
		disabled_ref: assert(!a_d1 || r_d1 || r || b);
		// A repeated sequence is no boolean, the operand is `not (a ##1 a)`
		repeated_ref: assert(!(a_d1 && a));
		// Some attempt started on an `en` before, or the one on this tick
		guard_ref: assert(!(w_d1 && a_d1) || b);
		// The attempts of P start one or two ticks after `en`, the disable takes the
		// ones it came upon on the way
		ranged_ref: assert(!a_d1 || b || r || r_d1 ||
						   !((en_d2 && !r_d2) || (en_d3 && !r_d3 && !r_d2)));
		zero_ref: assert(!(en_d1 && a_d1) || b);

		if (en) begin
			guard_test: assert property (always (a |-> ##1 b));
			ranged_test: assert property (disable iff (r) always [1:2] (a |-> ##1 b));
			zero_test: assert property (always [0:0] (a |-> ##1 b));
		end

		w_d1 <= w_d1 || en;
		en_d1 <= en;
		en_d2 <= en_d1;
		en_d3 <= en_d2;
		r_d1 <= r;
		r_d2 <= r_d1;
		r_d3 <= r_d2;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
	end

	response_test: assert property (@(posedge clk_i) always (a |-> ##[1:2] b));
	disabled_test: assert property (@(posedge clk_i) disable iff (r) always (a |-> ##1 b));
	repeated_test: assert property (@(posedge clk_i) always (not s[*2]));
endmodule
