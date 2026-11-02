// The default disable of a cover drops the attempts it comes upon
module disable_obligations_default(input clk_i, input logic r, input logic a, input logic b);
	default disable iff r;

	reg gate1 = 0;
	reg r_d1;
	reg a_d1;
	reg b_d1;

	always @(posedge clk_i) begin
		cover_ref: cover(gate1 && a_d1 && !r_d1 && (b_d1 || (b && !r)));
		assert_ref: assert(!gate1 || r_d1 || (a_d1 && (r || b)));

		gate1 <= 1;
		r_d1 <= r;
		a_d1 <= a;
		b_d1 <= b;
	end

	cover_test: cover property(@(posedge clk_i) strong(a ##[0:1] b));
	assert_test: assert property(@(posedge clk_i) strong(a ##1 b));
endmodule
