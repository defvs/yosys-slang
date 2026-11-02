// A cover has no letters to extend a match by, it takes a match with no disable from the
// attempt start up to the end of that match
module disable_cover(input clk_i, input logic r, input logic r2, input logic a, input logic b);
	default disable iff r2;

	reg gate1 = 0;
	reg gate2 = 0;
	reg r_d1, r_d2;
	reg r2_d1;
	reg a_d1, a_d2;
	reg b_d1;

	always @(posedge clk_i) begin
		sequence_ref: cover(gate1 && !r_d1 && !r && a_d1 && b);
		range_ref: cover(gate2 && !r_d2 && !r_d1 && a_d2 && (b_d1 || (!r && b)));
		default_ref: cover(gate1 && !r2_d1 && !r2 && a_d1 && b);

		gate1 <= 1;
		gate2 <= gate1;
		r_d1 <= r;
		r_d2 <= r_d1;
		r2_d1 <= r2;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
	end

	sequence_test: cover property(@(posedge clk_i) disable iff (r) a ##1 b);
	range_test: cover property(@(posedge clk_i) disable iff (r) a ##[1:2] b);
	default_test: cover property(@(posedge clk_i) a ##1 b);
endmodule
