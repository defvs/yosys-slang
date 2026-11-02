module disable_sampled(input clk_i, input logic a, input logic b, input logic [1:0] st);
	typedef enum logic [1:0] { IDLE = 2'd1, BUSY = 2'd2 } state_e;

	function logic same(logic x);
		return x;
	endfunction

	logic r = 1'b0;

	always @(posedge clk_i) begin
		// The value of the first assignment is gone once the attempt is evaluated
		r = b;
		transient_ref: assert(a);
		transient_test: assert property(disable iff (r) a);
		r = 1'b0;

		function_ref: assert(b || a);
		enum_ref: assert(st == 2'd2 || a);
	end

	function_test: assert property(@(posedge clk_i) disable iff (same(b)) a);
	enum_test: assert property(@(posedge clk_i) disable iff (st == BUSY) a);
endmodule
