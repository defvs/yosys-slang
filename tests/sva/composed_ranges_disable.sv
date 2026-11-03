// Ranged operands composed with each other under a disable, each against the attempt
// spelled out tick by tick, with the letters of Annex F for the booleans (see
// disable_settled_sequence.sv)
module composed_ranges_disable(input clk_i, input logic r, input logic a, input logic b,
								input logic c, input logic d);
	sequence pair(x, y);
		x ##[1:2] y;
	endsequence

	reg gate1 = 0;
	reg gate2 = 0;
	reg gate3 = 0;
	reg gate4 = 0;
	reg gate5 = 0;
	reg gate6 = 0;
	reg gate7 = 0;
	reg r_d1, r_d2, r_d3, r_d4, r_d5, r_d6, r_d7;
	reg a_d1, a_d2, a_d3, a_d4, a_d5, a_d6, a_d7;
	reg b_d1, b_d2, b_d3, b_d4, b_d5, b_d6;
	reg c_d1, c_d2, c_d3, c_d4, c_d5;
	reg d_d1, d_d2, d_d3, d_d4, d_d5;

	// The disable on any tick up to tick u of an attempt started L ticks ago, disL_u
	wire dis3_0 = r_d3;
	wire dis3_1 = r_d2 || dis3_0;
	wire dis3_2 = r_d1 || dis3_1;
	wire dis3_3 = r || dis3_2;
	wire dis4_0 = r_d4;
	wire dis4_1 = r_d3 || dis4_0;
	wire dis4_2 = r_d2 || dis4_1;
	wire dis4_3 = r_d1 || dis4_2;
	wire dis4_4 = r || dis4_3;
	wire dis5_0 = r_d5;
	wire dis5_1 = r_d4 || dis5_0;
	wire dis5_2 = r_d3 || dis5_1;
	wire dis5_3 = r_d2 || dis5_2;
	wire dis5_4 = r_d1 || dis5_3;
	wire dis5_5 = r || dis5_4;
	wire dis6_0 = r_d6;
	wire dis6_1 = r_d5 || dis6_0;
	wire dis6_2 = r_d4 || dis6_1;
	wire dis6_3 = r_d3 || dis6_2;
	wire dis6_4 = r_d2 || dis6_3;
	wire dis6_5 = r_d1 || dis6_4;
	wire dis6_6 = r || dis6_5;
	wire dis7_0 = r_d7;
	wire dis7_1 = r_d6 || dis7_0;
	wire dis7_2 = r_d5 || dis7_1;
	wire dis7_3 = r_d4 || dis7_2;
	wire dis7_4 = r_d3 || dis7_3;
	wire dis7_5 = r_d2 || dis7_4;
	wire dis7_6 = r_d1 || dis7_5;
	wire dis7_7 = r || dis7_6;

	// Matches ending on a tick of the attempt, or a consequent from one
	wire first_match_range_end2 = (a_d5 || dis5_0) && (b_d4 || dis5_1) && (c_d3 || dis5_2);
	wire first_match_range_end3 = ((a_d5 || dis5_0) && (b_d4 || dis5_1) && (c_d2 || dis5_3))
			|| ((a_d5 || dis5_0) && (b_d3 || dis5_2) && (c_d2 || dis5_3));
	wire first_match_range_end4 = (a_d5 || dis5_0) && (b_d3 || dis5_2) && (c_d1 || dis5_4);
	wire and_range_end1 = (((a_d3 || dis3_0) && (b_d2 || dis3_1))
				&& ((c_d3 || dis3_0) && (d_d3 || dis3_0)))
			|| (((a_d3 || dis3_0) && (b_d2 || dis3_1)) && ((c_d3 || dis3_0) && (d_d2 || dis3_1)));
	wire and_range_end2 = (((a_d3 || dis3_0) && (b_d2 || dis3_1))
				&& ((c_d3 || dis3_0) && (d_d1 || dis3_2)))
			|| (((a_d3 || dis3_0) && (b_d1 || dis3_2)) && ((c_d3 || dis3_0) && (d_d3 || dis3_0)))
			|| (((a_d3 || dis3_0) && (b_d1 || dis3_2)) && ((c_d3 || dis3_0) && (d_d2 || dis3_1)))
			|| (((a_d3 || dis3_0) && (b_d1 || dis3_2)) && ((c_d3 || dis3_0) && (d_d1 || dis3_2)));
	wire shared_consequent_from1 = ((c_d5 || dis7_2)
				&& ((d_d5 || dis7_2) || (d_d4 || dis7_3) || (d_d3 || dis7_4)))
			|| ((c_d4 || dis7_3) && ((d_d4 || dis7_3) || (d_d3 || dis7_4) || (d_d2 || dis7_5)));
	wire shared_consequent_from2 = ((c_d4 || dis7_3)
				&& ((d_d4 || dis7_3) || (d_d3 || dis7_4) || (d_d2 || dis7_5)))
			|| ((c_d3 || dis7_4) && ((d_d3 || dis7_4) || (d_d2 || dis7_5) || (d_d1 || dis7_6)));
	wire shared_consequent_from3 = ((c_d3 || dis7_4)
				&& ((d_d3 || dis7_4) || (d_d2 || dis7_5) || (d_d1 || dis7_6)))
			|| ((c_d2 || dis7_5) && ((d_d2 || dis7_5) || (d_d1 || dis7_6) || (d || dis7_7)));

	always @(posedge clk_i) begin
		throughout_range_ref: assert(!gate3
				|| (((a_d3 || dis3_0) && (b_d2 || dis3_1) && (c_d3 || dis3_0) && (c_d2 || dis3_1))
					|| ((a_d3 || dis3_0)
						&& (b_d1 || dis3_2)
						&& (c_d3 || dis3_0)
						&& (c_d2 || dis3_1)
						&& (c_d1 || dis3_2))
					|| ((a_d3 || dis3_0)
						&& (b || dis3_3)
						&& (c_d3 || dis3_0)
						&& (c_d2 || dis3_1)
						&& (c_d1 || dis3_2)
						&& (c || dis3_3))));
		first_match_range_ref: assert(!gate5
				|| ((first_match_range_end2 && (d_d2 || dis5_3))
					|| ((first_match_range_end3 && !first_match_range_end2) && (d_d1 || dis5_4))
					|| ((first_match_range_end4
							&& !(first_match_range_end2 || first_match_range_end3))
						&& (d || dis5_5))));
		and_range_ref: assert(!gate3
				|| ((and_range_end1 && (c_d1 || dis3_2)) || (and_range_end2 && (c || dis3_3))));
		window_ref: assert(!gate5
				|| (((a_d5 || dis5_0)
						&& (b_d4 || dis5_1)
						&& ((d_d3 || dis5_2)
							|| (d_d2 || dis5_3)
							|| ((c_d4 || dis5_1) && (d_d3 || dis5_2))
							|| ((c_d3 || dis5_2) && (d_d2 || dis5_3))
							|| ((c_d2 || dis5_3) && (d_d1 || dis5_4))))
					|| ((a_d5 || dis5_0)
						&& (b_d3 || dis5_2)
						&& ((d_d2 || dis5_3)
							|| (d_d1 || dis5_4)
							|| ((c_d3 || dis5_2) && (d_d2 || dis5_3))
							|| ((c_d2 || dis5_3) && (d_d1 || dis5_4))
							|| ((c_d1 || dis5_4) && (d || dis5_5))))));
		shared_consequent_ref: assert(!gate7
				|| ((!((a_d7 && !dis7_0) && (b_d6 && !dis7_1)) || shared_consequent_from1)
					&& (!((a_d7 && !dis7_0) && (b_d5 && !dis7_2)) || shared_consequent_from2)
					&& (!((a_d7 && !dis7_0) && (b_d4 && !dis7_3)) || shared_consequent_from3)));
		eventually_range_ref: assert(!gate4
				|| (!(a_d4 && !dis4_0)
					|| ((b_d3 || dis4_1) && ((c_d2 || dis4_2) || (c_d1 || dis4_3)))
					|| ((b_d2 || dis4_2) && ((c_d1 || dis4_3) || (c || dis4_4)))));
		parenthesized_ref: assert(!gate5
				|| (((a_d5 || dis5_0) && (b_d4 || dis5_1) && (c_d3 || dis5_2) && (d_d2 || dis5_3))
					|| ((a_d5 || dis5_0)
						&& (b_d4 || dis5_1)
						&& (c_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((a_d5 || dis5_0)
						&& (b_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((a_d5 || dis5_0)
						&& (b_d3 || dis5_2)
						&& (c_d1 || dis5_4)
						&& (d || dis5_5))));
		named_ref: assert(!gate5
				|| (((a_d5 || dis5_0) && (b_d4 || dis5_1) && (c_d3 || dis5_2) && (d_d2 || dis5_3))
					|| ((a_d5 || dis5_0)
						&& (b_d4 || dis5_1)
						&& (c_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((a_d5 || dis5_0)
						&& (b_d3 || dis5_2)
						&& (c_d2 || dis5_3)
						&& (d_d1 || dis5_4))
					|| ((a_d5 || dis5_0)
						&& (b_d3 || dis5_2)
						&& (c_d1 || dis5_4)
						&& (d || dis5_5))));
		repeat_range_ref: assert(!gate6
				|| (((a_d6 || dis6_0)
						&& (b_d5 || dis6_1)
						&& (a_d4 || dis6_2)
						&& (b_d3 || dis6_3)
						&& (c_d2 || dis6_4))
					|| ((a_d6 || dis6_0)
						&& (b_d5 || dis6_1)
						&& (a_d4 || dis6_2)
						&& (b_d2 || dis6_4)
						&& (c_d1 || dis6_5))
					|| ((a_d6 || dis6_0)
						&& (b_d4 || dis6_2)
						&& (a_d3 || dis6_3)
						&& (b_d2 || dis6_4)
						&& (c_d1 || dis6_5))
					|| ((a_d6 || dis6_0)
						&& (b_d4 || dis6_2)
						&& (a_d3 || dis6_3)
						&& (b_d1 || dis6_5)
						&& (c || dis6_6))));
		empty_first_ref: assert(!gate3
				|| (((a_d3 || dis3_0)
						&& ((c_d2 || dis3_1) || ((b_d2 || dis3_1) && (c_d1 || dis3_2))))
					|| ((a_d3 || dis3_0)
						&& ((c_d1 || dis3_2) || ((b_d1 || dis3_2) && (c || dis3_3))))));
		cover_parenthesized_ref: cover(gate4
				&& (((a_d4 && !dis4_0) && (b_d3 && !dis4_1) && (c_d2 && !dis4_2))
					|| ((a_d4 && !dis4_0) && (b_d3 && !dis4_1) && (c_d1 && !dis4_3))
					|| ((a_d4 && !dis4_0) && (b_d2 && !dis4_2) && (c_d1 && !dis4_3))
					|| ((a_d4 && !dis4_0) && (b_d2 && !dis4_2) && (c && !dis4_4))));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		gate4 <= gate3;
		gate5 <= gate4;
		gate6 <= gate5;
		gate7 <= gate6;
		r_d1 <= r;
		r_d2 <= r_d1;
		r_d3 <= r_d2;
		r_d4 <= r_d3;
		r_d5 <= r_d4;
		r_d6 <= r_d5;
		r_d7 <= r_d6;
		a_d1 <= a;
		a_d2 <= a_d1;
		a_d3 <= a_d2;
		a_d4 <= a_d3;
		a_d5 <= a_d4;
		a_d6 <= a_d5;
		a_d7 <= a_d6;
		b_d1 <= b;
		b_d2 <= b_d1;
		b_d3 <= b_d2;
		b_d4 <= b_d3;
		b_d5 <= b_d4;
		b_d6 <= b_d5;
		c_d1 <= c;
		c_d2 <= c_d1;
		c_d3 <= c_d2;
		c_d4 <= c_d3;
		c_d5 <= c_d4;
		d_d1 <= d;
		d_d2 <= d_d1;
		d_d3 <= d_d2;
		d_d4 <= d_d3;
		d_d5 <= d_d4;
	end

	throughout_range_test: assert property(@(posedge clk_i) disable iff (r)
			c throughout (a ##[1:3] b));
	first_match_range_test: assert property(@(posedge clk_i) disable iff (r)
			first_match(a ##[1:2] b ##[1:2] c) ##1 d);
	and_range_test: assert property(@(posedge clk_i) disable iff (r)
			((a ##[1:2] b) and (c ##[0:2] d)) ##1 c);
	window_test: assert property(@(posedge clk_i) disable iff (r)
			a ##[1:2] b ##[0:2] c[*0:1] ##1 d);
	shared_consequent_test: assert property(@(posedge clk_i) disable iff (r)
			a ##[1:3] b |-> ##[1:2] c ##[0:2] d);
	eventually_range_test: assert property(@(posedge clk_i) disable iff (r)
			a |-> s_eventually [1:2] (b ##[1:2] c));
	parenthesized_test: assert property(@(posedge clk_i) disable iff (r)
			a ##[1:2] (b ##[1:2] c) ##1 d);
	named_test: assert property(@(posedge clk_i) disable iff (r) a ##[1:2] pair(b, c) ##1 d);
	repeat_range_test: assert property(@(posedge clk_i) disable iff (r) (a ##[1:2] b)[*2] ##1 c);
	empty_first_test: assert property(@(posedge clk_i) disable iff (r) a ##[1:2] (b[*0:1] ##1 c));
	cover_parenthesized_test: cover property(@(posedge clk_i) disable iff (r)
			a ##[1:2] (b ##[1:2] c));
endmodule
