module frozen_first_tick(input clk_i, input logic a, input logic b, input logic c, input logic [3:0] v);
	function automatic logic same(logic y);
		return y;
	endfunction

	function logic cast(logic y);
		return const'(y);
	endfunction

	logic x = 1'b0;
	logic gate = 1'b0;
	logic a_d1;
	logic b_d1;

	always @(posedge clk_i) begin
		automatic logic t = a;
		automatic logic k = 1'b1;
		automatic int n = 2;

		x = a;

		single_ref: assert(a);
		sequence_ref: assert(!gate || (a_d1 && b));
		implication_ref: assert(!gate || !a_d1 || b);
		consequent_ref: assert(!b || a);
		and_ref: assert(!gate || (a_d1 && b_d1 && c));
		zero_delay_ref: assert(a && b);
		disable_ref: assert(a || b);
		function_ref: assert(!gate || (a_d1 && b));
		cast_ref: assert(a);
		cast_sequence_ref: assert(!gate || (a_d1 && b));
		cast_function_ref: assert(!gate || (b_d1 && a));
		constant_ref: assert(!gate || (b_d1 && v[2]));

		single_test: assert property(t);
		sequence_test: assert property(t ##1 b);
		implication_test: assert property(t |=> b);
		consequent_test: assert property(b |-> t);
		and_test: assert property(t and (b ##1 c));
		zero_delay_test: assert property(b ##0 t);
		disable_test: assert property(disable iff (t) b);
		function_test: assert property(same(t) ##1 b);
		cast_test: assert property(const'(x));
		cast_sequence_test: assert property(const'(x) ##1 b);

		// Only values frozen with the attempt are bound to its first tick
		cast_function_test: assert property(b ##1 cast(a));
		constant_test: assert property(b ##1 v[n] && k);

		gate <= 1'b1;
		a_d1 <= a;
		b_d1 <= b;
	end
endmodule
