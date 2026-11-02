// Unbounded sequence monitor against hand-written match equations, derived from
// the rules for empty matches in IEEE 1800-2017 16.9.2.1. The never-matching
// `dead` branch only serves to route bounded sequences through the monitor.
module unbounded_empty(input logic clk_i, input logic a, input logic b, input logic c,
					   input logic d, input logic e, input logic z);
	sequence dead;
		1'b0 ##[1:$] 1'b0;
	endsequence

	// Past samples, numbered by age, and whether as many ticks have passed
	reg a1 = 0, a2 = 0, a3 = 0, a4 = 0;
	reg b1 = 0, b2 = 0, b3 = 0;
	reg d1 = 0, d2 = 0;
	reg e1 = 0, e2 = 0, e3 = 0, e4 = 0, e5 = 0;
	reg v1 = 0, v2 = 0, v3 = 0;

	// Monitors for `e ##1` followed by an unbounded sequence
	reg first_wait = 0, second_wait = 0, second_hit = 0, first_hit = 0, quiet = 0;
	reg run = 0, run1 = 0, run2 = 0, seen = 0;
	wire first = first_wait && a;

	always @(posedge clk_i) begin
		{a4, a3, a2, a1} <= {a3, a2, a1, a};
		{b3, b2, b1} <= {b2, b1, b};
		{d2, d1} <= {d1, d};
		{e5, e4, e3, e2, e1} <= {e4, e3, e2, e1, e};
		{v3, v2, v1} <= {v2, v1, 1'b1};

		first_wait <= e || (first_wait && !a);
		first_hit <= first;
		second_wait <= first || (second_wait && !a);
		second_hit <= second_wait && a;
		quiet <= first || (quiet && !a);
		run <= (e1 || run) && a;
		run1 <= e1 && a;
		run2 <= (run1 || run2) && a;
		seen <= seen || e1;
	end

	always @(posedge clk_i) begin
		leading_ref: assert(!(b || (a1 && b)) || z);
		trailing_ref: assert(!(a || (a1 && b)) || z);
		middle_ref: assert(!((a1 && c) || (a2 && b1 && c)) || z);
		both_ref: assert(!(c || (a1 && c) || (b1 && c) || (a2 && b1 && c)) || z);
		two_empty_ref: assert(z);
		three_empty_ref: assert(!((a3 && b) || a2 || (v2 && b) || v1) || z);
		fusion_left_ref: assert(!(a && b) || z);
		fusion_right_ref: assert(!(a && b) || z);
		delay_range_ref: assert(!(b && (a2 || a3 || v1 || v2)) || z);
		alternative_ref: assert(!(c && (e1 || (e2 && a1) || (e2 && b1))) || z);
		nested_ref: assert(!(c && (e1 || (e2 && a1) || (e2 && b1) || (e3 && a2 && b1))) || z);
		repeated_ref: assert(!(b && (e1 || (e2 && a1) || (e3 && a2 && a1))) || z);
		repeated_seq_ref: assert(!(c && ((e2 && a1) || (e3 && a2 && b1) || (e3 && a2 && a1) ||
										 (e4 && a3 && a2 && b1) || (e4 && a3 && b2 && a1) ||
										 (e5 && a4 && b3 && a2 && b1))) || z);
		prefix_ref: assert(!(b && ((v3 && a1) || v2)) || z);
		prefix_range_ref: assert(!(b && v1) || z);
		throughout_ref: assert(!(d && e && d1 && ((a1) || (d2 && a2 && b1))) || z);
		nonoverlap_ref: assert(!(e || (e1 && a1)) || z);
		goto_ref: assert(!(second_hit && b) || z);
		goto_zero_ref: assert(!(b && (e1 || first_hit)) || z);
		nonconsecutive_ref: assert(!(quiet && b) || z);
		nonconsecutive_zero_ref: assert(!(b && (first_wait || quiet)) || z);
		star_ref: assert(!(b && (e1 || run)) || z);
		plus_two_ref: assert(!(b && run2) || z);
		delay_ref: assert(!(a && seen) || z);

		leading_test: assert property(((a[*0:1] ##1 b) or dead) |-> z);
		trailing_test: assert property(((a ##1 b[*0:1]) or dead) |-> z);
		middle_test: assert property(((a ##1 b[*0:1] ##1 c) or dead) |-> z);
		both_test: assert property(((a[*0:1] ##1 b[*0:1] ##1 c) or dead) |-> z);
		two_empty_test: assert property(((a[*0:1] ##2 b[*0:1]) or dead) |-> z);
		three_empty_test: assert property(((a[*0:1] ##3 b[*0:1]) or dead) |-> z);
		fusion_left_test: assert property(((a[*0:1] ##0 b) or dead) |-> z);
		fusion_right_test: assert property(((a ##0 b[*0:1]) or dead) |-> z);
		delay_range_test: assert property(((a[*0:1] ##[2:3] b) or dead) |-> z);
		alternative_test: assert property(((e ##1 (a[*0:1] or b[*0:1]) ##1 c) or dead) |-> z);
		nested_test: assert property(((e ##1 (a[*0:1] ##1 b[*0:1]) ##1 c) or dead) |-> z);
		repeated_test: assert property(((e ##1 (a[*0:1])[*2] ##1 b) or dead) |-> z);
		repeated_seq_test: assert property(((e ##1 (a ##1 b[*0:1])[*1:2] ##1 c) or dead) |-> z);
		prefix_test: assert property(((##2 a[*0:1] ##1 b) or dead) |-> z);
		prefix_range_test: assert property(((##[1:2] a[*0:1] ##1 b) or dead) |-> z);
		throughout_test: assert property(((d throughout (a ##1 b[*0:1] ##1 e)) or dead) |-> z);
		nonoverlap_test: assert property(e |-> ((a[*0:1] or dead) |=> z));
		goto_test: assert property((e ##1 a[->2] ##1 b) |-> z);
		goto_zero_test: assert property((e ##1 a[->0:1] ##1 b) |-> z);
		nonconsecutive_test: assert property((e ##1 a[=1] ##1 b) |-> z);
		nonconsecutive_zero_test: assert property((e ##1 a[=0:1] ##1 b) |-> z);
		star_test: assert property((e ##1 a[*0:$] ##1 b) |-> z);
		plus_two_test: assert property((e ##1 a[*2:$] ##1 b) |-> z);
		delay_test: assert property((e ##[2:$] a) |-> z);
	end
endmodule
