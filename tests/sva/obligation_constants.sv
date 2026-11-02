// Constant expressions in the operands of properties the liveness lowering takes
// apart: always, until, property if and case, implies and iff
package obligation_constants_pkg;
	typedef enum logic [1:0] { PKG_IDLE = 2'd1, PKG_BUSY = 2'd2 } pkg_state_e;
endpackage

module obligation_constants_sub #(parameter int LIMIT = 5) (input logic clk_i, input logic [7:0] x, input logic b);
	typedef enum logic [1:0] { IDLE = 2'd1, BUSY = 2'd2 } state_e;

	always @(posedge clk_i)
		limit_ref: assert(x <= 8'd3 || b);

	limit_test: assert property(@(posedge clk_i) (x <= LIMIT) until b);
endmodule

module obligation_constants #(
	parameter int ARR [2] = '{5, 9},
	parameter real RATE = 2.5,
	parameter string NAME = "abc"
) (input logic clk_i, input logic [1:0] st, input logic [3:0] v, input logic [7:0] x,
   input logic a, input logic b);
	typedef enum logic [1:0] { IDLE = 2'd1, BUSY = 2'd2, DONE = 2'd3 } state_e;
	state_e s;

	function automatic int fact(int n);
		return n <= 1 ? 1 : n * fact(n - 1);
	endfunction

	// Static, as functions are by default
	function logic same(logic y);
		return y;
	endfunction

	obligation_constants_sub u(.*);
	defparam u.LIMIT = 3;

	always @(posedge clk_i) begin
		enum_ref: assert(st != 2'd3);
		enum_pkg_ref: assert(st != 2'd2 || b);
		enum_first_ref: assert((st == 2'd1) == b);
		bits_ref: assert(v[3] || b);
		array_ref: assert(x != 8'd9 || b);
		real_ref: assert(!x[0] || b);
		string_ref: assert(x[0]);
		function_ref: assert(x == 8'd6);
		static_ref: assert(a && !b);
		hierarchical_ref: assert(x != 8'd3 || b);
		hierarchical_enum_ref: assert(st != 2'd2);
	end

	enum_test: assert property(@(posedge clk_i) always (st != DONE));
	enum_pkg_test: assert property(@(posedge clk_i) if (st == obligation_constants_pkg::PKG_BUSY) b);
	enum_first_test: assert property(@(posedge clk_i) (st == s.first()) iff b);
	bits_test: assert property(@(posedge clk_i) v[$bits(v) - 1] until b);
	array_test: assert property(@(posedge clk_i) case (x) ARR[1]: b; endcase);
	real_test: assert property(@(posedge clk_i) (RATE > 1.5 && x[0]) implies b);
	string_test: assert property(@(posedge clk_i) always (NAME == "abc" && x[0]));
	function_test: assert property(@(posedge clk_i) always (x == fact(3)));
	static_test: assert property(@(posedge clk_i) always (same(a) && !same(b)));
	hierarchical_test: assert property(@(posedge clk_i) (x == u.LIMIT) implies b);
	hierarchical_enum_test: assert property(@(posedge clk_i) always (st != u.BUSY));

	for (genvar i = 0; i < 2; i++) begin : gen
		always @(posedge clk_i)
			select_ref: assert(v[i] || b);

		select_test: assert property(@(posedge clk_i) v[i] until b);
	end
endmodule
