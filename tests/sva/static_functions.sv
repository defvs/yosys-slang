package static_functions_pkg;
	function logic both(logic x, logic y);
		return x & y;
	endfunction
endpackage

function logic static_functions_either(logic x, logic y);
	return x | y;
endfunction

module static_functions(input logic clk_i, input logic a, input logic b, input logic [3:0] v);
	function logic same(logic x);
		return x;
	endfunction

	function logic forward(logic x);
		return same(x);
	endfunction

	function logic in_block(logic x);
		begin : blk
			logic y;
			y = !x;
			return y;
		end
	endfunction

	function logic by_name(logic x);
		by_name = x;
		by_name = !by_name;
	endfunction

	function int popcount(logic [3:0] x);
		int n;
		n = 0;
		for (int i = 0; i < 4; i++)
			n += x[i];
		return n;
	endfunction

	logic state = 1'b0;

	function logic with_state(logic x);
		return x & state;
	endfunction

	function logic latch(logic x);
		logic k;
		k = x;
		return k;
	endfunction

	logic valid = 1'b0;
	logic a_q;
	logic latched;

	always @(posedge clk_i) begin
		two_calls_ref: assert(a == b);
		temporal_ref: assert(!valid || a == b);
		implication_ref: assert(!valid || !a_q || b);
		sequence_ref: assert(!valid || (a_q && b));
		forward_ref: assert(a);
		block_local_ref: assert(!a);
		return_var_ref: assert(!a);
		local_var_ref: assert($countones(v) < 3);
		package_ref: assert(a & b);
		unit_ref: assert(a | b);
		rose_ref: assert(!valid || (a && !a_q));

		// The function's body reads the sampled value of the module's variable
		state = 1'b1;
		module_state_ref: assert(a & valid);
		module_state_test: assert property(with_state(a));

		// Outside of a call of the function its static variable is state like any other,
		// sampled in an assertion
		latched <= latch(a);
		static_outside_ref: assert(!valid || a_q == a);
		static_outside_test: assert property(!valid || latch.k == a);

		valid <= 1'b1;
		a_q <= a;
	end

	two_calls_test: assert property(@(posedge clk_i) same(a) == same(b));
	temporal_test: assert property(@(posedge clk_i) 1'b1 ##1 same(a) == same(b));
	implication_test: assert property(@(posedge clk_i) same(a) |=> same(b));
	sequence_test: assert property(@(posedge clk_i) 1'b1 ##1 same(a_q) ##0 same(b));
	forward_test: assert property(@(posedge clk_i) forward(a));
	block_local_test: assert property(@(posedge clk_i) in_block(a));
	return_var_test: assert property(@(posedge clk_i) by_name(a));
	local_var_test: assert property(@(posedge clk_i) popcount(v) < 3);
	package_test: assert property(@(posedge clk_i) static_functions_pkg::both(a, b));
	unit_test: assert property(@(posedge clk_i) static_functions_either(a, b));
	rose_test: assert property(@(posedge clk_i) disable iff (!valid) $rose(same(a)));
endmodule
