// Ranged operands composed with each other, each against the attempt spelled out tick
// by tick. These take the running prefixes of throughout, first_match and `and`, the
// windows of paths a concatenation is placed after, a consequent shared by the ticks
// it is placed on, and a concatenation placed after many paths at once, or not when
// its first element can be empty.
module composed_ranges(input clk_i, input logic a, input logic b, input logic c, input logic d);
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
	reg a_d1, a_d2, a_d3, a_d4, a_d5, a_d6, a_d7;
	reg b_d1, b_d2, b_d3, b_d4, b_d5, b_d6;
	reg c_d1, c_d2, c_d3, c_d4, c_d5;
	reg d_d1, d_d2, d_d3, d_d4, d_d5;

	// Matches ending on a tick of the attempt, or a consequent from one
	wire first_match_range_end2 = a_d5 && b_d4 && c_d3;
	wire first_match_range_end3 = (a_d5 && b_d4 && c_d2) || (a_d5 && b_d3 && c_d2);
	wire first_match_range_end4 = a_d5 && b_d3 && c_d1;
	wire and_range_end1 = ((a_d3 && b_d2) && (c_d3 && d_d3)) || ((a_d3 && b_d2) && (c_d3 && d_d2));
	wire and_range_end2 = ((a_d3 && b_d2) && (c_d3 && d_d1))
			|| ((a_d3 && b_d1) && (c_d3 && d_d3))
			|| ((a_d3 && b_d1) && (c_d3 && d_d2))
			|| ((a_d3 && b_d1) && (c_d3 && d_d1));
	wire shared_consequent_from1 = (c_d5 && (d_d5 || d_d4 || d_d3))
			|| (c_d4 && (d_d4 || d_d3 || d_d2));
	wire shared_consequent_from2 = (c_d4 && (d_d4 || d_d3 || d_d2))
			|| (c_d3 && (d_d3 || d_d2 || d_d1));
	wire shared_consequent_from3 = (c_d3 && (d_d3 || d_d2 || d_d1))
			|| (c_d2 && (d_d2 || d_d1 || d));

	always @(posedge clk_i) begin
		throughout_range_ref: assert(!gate3
				|| ((a_d3 && b_d2 && c_d3 && c_d2)
					|| (a_d3 && b_d1 && c_d3 && c_d2 && c_d1)
					|| (a_d3 && b && c_d3 && c_d2 && c_d1 && c)));
		first_match_range_ref: assert(!gate5
				|| ((first_match_range_end2 && d_d2)
					|| ((first_match_range_end3 && !first_match_range_end2) && d_d1)
					|| ((first_match_range_end4
							&& !(first_match_range_end2 || first_match_range_end3))
						&& d)));
		and_range_ref: assert(!gate3 || ((and_range_end1 && c_d1) || (and_range_end2 && c)));
		window_ref: assert(!gate5
				|| ((a_d5
						&& b_d4
						&& (d_d3 || d_d2 || (c_d4 && d_d3) || (c_d3 && d_d2) || (c_d2 && d_d1)))
					|| (a_d5
						&& b_d3
						&& (d_d2 || d_d1 || (c_d3 && d_d2) || (c_d2 && d_d1) || (c_d1 && d)))));
		shared_consequent_ref: assert(!gate7
				|| ((!(a_d7 && b_d6) || shared_consequent_from1)
					&& (!(a_d7 && b_d5) || shared_consequent_from2)
					&& (!(a_d7 && b_d4) || shared_consequent_from3)));
		eventually_range_ref: assert(!gate4
				|| (!a_d4 || (b_d3 && (c_d2 || c_d1)) || (b_d2 && (c_d1 || c))));
		parenthesized_ref: assert(!gate5
				|| ((a_d5 && b_d4 && c_d3 && d_d2)
					|| (a_d5 && b_d4 && c_d2 && d_d1)
					|| (a_d5 && b_d3 && c_d2 && d_d1)
					|| (a_d5 && b_d3 && c_d1 && d)));
		named_ref: assert(!gate5
				|| ((a_d5 && b_d4 && c_d3 && d_d2)
					|| (a_d5 && b_d4 && c_d2 && d_d1)
					|| (a_d5 && b_d3 && c_d2 && d_d1)
					|| (a_d5 && b_d3 && c_d1 && d)));
		repeat_range_ref: assert(!gate6
				|| ((a_d6 && b_d5 && a_d4 && b_d3 && c_d2)
					|| (a_d6 && b_d5 && a_d4 && b_d2 && c_d1)
					|| (a_d6 && b_d4 && a_d3 && b_d2 && c_d1)
					|| (a_d6 && b_d4 && a_d3 && b_d1 && c)));
		empty_first_ref: assert(!gate3
				|| ((a_d3 && (c_d2 || (b_d2 && c_d1))) || (a_d3 && (c_d1 || (b_d1 && c)))));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		gate4 <= gate3;
		gate5 <= gate4;
		gate6 <= gate5;
		gate7 <= gate6;
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

	throughout_range_test: assert property(@(posedge clk_i) c throughout (a ##[1:3] b));
	first_match_range_test: assert property(@(posedge clk_i)
			first_match(a ##[1:2] b ##[1:2] c) ##1 d);
	and_range_test: assert property(@(posedge clk_i) ((a ##[1:2] b) and (c ##[0:2] d)) ##1 c);
	window_test: assert property(@(posedge clk_i) a ##[1:2] b ##[0:2] c[*0:1] ##1 d);
	shared_consequent_test: assert property(@(posedge clk_i) a ##[1:3] b |-> ##[1:2] c ##[0:2] d);
	eventually_range_test: assert property(@(posedge clk_i) a |-> s_eventually [1:2] (b ##[1:2] c));
	parenthesized_test: assert property(@(posedge clk_i) a ##[1:2] (b ##[1:2] c) ##1 d);
	named_test: assert property(@(posedge clk_i) a ##[1:2] pair(b, c) ##1 d);
	repeat_range_test: assert property(@(posedge clk_i) (a ##[1:2] b)[*2] ##1 c);
	empty_first_test: assert property(@(posedge clk_i) a ##[1:2] (b[*0:1] ##1 c));
endmodule
