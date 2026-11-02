// Values frozen when the attempt is queued, automatic variables and const casts, read
// on the first tick of the attempt by the monitors of the liveness lowering
module frozen_obligations(input clk_i, input logic a, input logic b, input logic c);
	logic x = 1'b0;
	logic t_d1 = 1'b0;
	// An attempt is in flight with t on some tick before and b on each one after it
	logic run = 1'b0;

	always @(posedge clk_i) begin
		automatic logic t = a ^ c;

		x = a;

		if_ref: assert(!t || b);
		case_ref: assert(!t || b);
		implies_ref: assert(!t || b);
		iff_ref: assert(t == b);
		always_ref: assert(t);
		not_always_ref: assert(!t);
		next_ref: assert(!t_d1 || b);
		antecedent_ref: assert(!(run && b) || c);
		cast_ref: assert(!a || b);

		if_test: assert property (if (t) b);
		case_test: assert property (case (t) 1'b1: b; endcase);
		implies_test: assert property (t implies b);
		iff_test: assert property (t iff b);
		always_test: assert property (always [0:0] t);
		not_always_test: assert property (not (always [0:0] t));
		next_test: assert property (t |=> always [0:0] b);
		antecedent_test: assert property (t ##1 b[*1:$] |-> c);
		cast_test: assert property (if (const'(x)) b);

		t_d1 <= t;
		run <= t || (run && b);
	end
endmodule
