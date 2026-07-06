module sample_value_functions(input clk_i, input logic a, input logic [1:0] b);
	logic valid = 1'b0;
	logic a_q;
	logic [1:0] b_q;

	always_ff @(posedge clk_i) begin
		rose_ref: assert(!valid || (a && !a_q));
		fell_ref: assert(!valid || (!a && a_q));
		stable_ref: assert(!valid || (b == b_q));
		changed_ref: assert(!valid || (b != b_q));
		past_empty_tick_ref: assert(!valid || ($past(a) == a_q));

		valid <= 1'b1;
		a_q <= a;
		b_q <= b;
	end

	rose_test: assert property(@(posedge clk_i) disable iff (!valid) $rose(a));
	fell_test: assert property(@(posedge clk_i) disable iff (!valid) $fell(a));
	stable_test: assert property(@(posedge clk_i) disable iff (!valid) $stable(b));
	changed_test: assert property(@(posedge clk_i) disable iff (!valid) $changed(b));
	past_empty_tick_test: assert property(@(posedge clk_i) disable iff (!valid) $past(a,) == a_q);
endmodule
