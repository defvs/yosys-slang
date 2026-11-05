// A disable doesn't cancel what's settled ahead of it. The ticks of an attempt before
// the first one with the condition are extended by letters which satisfy any boolean,
// and the property holds if it holds on that (IEEE Std 1800-2017 Annex F).
module disable_settled_sequence(input clk_i, input logic r, input logic a, input logic b,
								input logic c, input logic [1:0] v);
	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg r_d1, r_d2, r_d3;
	reg a_d1, a_d2, a_d3;
	reg b_d1, b_d2;
	reg c_d1, c_d2, c_d3;
	reg [1:0] v_d1;

	always @(posedge clk_i) begin
		single_ref: assert(!gate2 || r_d2 || (a_d2 && (r_d1 || (b_d1 && (r || c)))));

		// Up to the end of the shorter path a disable leaves both to go by
		lengths_ref: assert(!gate3 || r_d3 || (r_d2 ? a_d3 || c_d3
				: (a_d3 && b_d2) || (c_d3 && (r_d1 || r || b))));

		range_ref: assert(!gate2 || r_d2 || (a_d2 && (r_d1 || (b_d2 && c_d1) || (b_d1 && (r || c)))));
		throughout_ref: assert(!gate2 || r_d2 || (a_d2 && c_d2 && (r_d1 || (c_d1 && (r || (c && b))))));

		// With `b` on the first tick, the first match goes on with `c` on the second
		first_match_ref: assert(!gate2 || r_d2 || (a_d2 && (r_d1 || (b_d2 ? c_d1 : b_d1 && (r || c)))));

		repeat_ref: assert(!gate3 || r_d3 || (a_d3 && (r_d2 || (b_d2 && (r_d1 || (a_d1 && (r || b)))))));
		repeat_range_ref: assert(!gate2 || r_d2 || (a_d2 && (r_d1 || b_d1 || (a_d1 && (r || b)))));

		vector_condition_ref: assert(!gate1 || v_d1 != 0 || (a_d1 && (v != 0 || b)));
		vector_operand_ref: assert(!gate1 || r_d1 || (v_d1 != 0 && (r || a)));
		never_disabled_ref: assert(!gate1 || (a_d1 && b));
		always_disabled_ref: assert(1);
		// The assumption rules out an attempt without `a` on its first tick as of that
		// tick, as no disable later on can cancel that
		assume_ref: assume((r || a) && (!gate1 || r_d1 || (a_d1 && (r || b))));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		r_d1 <= r;
		r_d2 <= r_d1;
		r_d3 <= r_d2;
		a_d1 <= a;
		a_d2 <= a_d1;
		a_d3 <= a_d2;
		b_d1 <= b;
		b_d2 <= b_d1;
		c_d1 <= c;
		c_d2 <= c_d1;
		c_d3 <= c_d2;
		v_d1 <= v;
	end

	single_test: assert property(@(posedge clk_i) disable iff (r) a ##1 b ##1 c);
	lengths_test: assert property(@(posedge clk_i) disable iff (r) (a ##1 b) or (c ##3 b));
	range_test: assert property(@(posedge clk_i) disable iff (r) a ##[0:1] b ##1 c);
	throughout_test: assert property(@(posedge clk_i) disable iff (r) c throughout (a ##2 b));
	first_match_test: assert property(@(posedge clk_i) disable iff (r) first_match(a ##[0:1] b) ##1 c);
	repeat_test: assert property(@(posedge clk_i) disable iff (r) (a ##1 b)[*2]);
	repeat_range_test: assert property(@(posedge clk_i) disable iff (r) a[*1:2] ##1 b);
	vector_condition_test: assert property(@(posedge clk_i) disable iff (v) a ##1 b);
	vector_operand_test: assert property(@(posedge clk_i) disable iff (r) v ##1 a);
	never_disabled_test: assert property(@(posedge clk_i) disable iff (1'b0) a ##1 b);
	always_disabled_test: assert property(@(posedge clk_i) disable iff (1'b1) a ##1 b);
	assume_test: assume property(@(posedge clk_i) disable iff (r) a ##1 b);
endmodule
