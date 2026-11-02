// An empty match of a sequence used as a property is no success. Such sequences are
// degenerate, but slang lets them pass as long as it's warning about no possible match.
// With a path that can't match being the longest one the other matches report late.
module empty_property(input clk_i, input logic a, input logic b, input logic c, input logic r);
	reg gate1 = 0;
	reg gate2 = 0;
	reg a_d1;
	reg b_d1;
	reg r_d1;

	property implication;
		a |-> b;
	endproperty

	always @(posedge clk_i) begin
		toplevel_ref: assert(0);
		overlap_ref: assert(!a);
		overlap_only_empty_ref: assert(!a);
		overlap_other_match_ref: assert(!gate1 || !a_d1 || b_d1);
		nonoverlap_ref: assert(!gate1 || !a_d1);
		negated_ref: assert(1);
		negated_other_match_ref: assert(!gate1 || !b_d1);
		nexttime_ref: assert(!gate1);
		snexttime_other_match_ref: assert(!gate2 || b_d1);
		eventually_ref: assert(!gate1);
		and_left_ref: assert(0);
		and_right_ref: assert(0);
		and_instance_ref: assert(0);
		or_ref: assert(!a || b);
		disable_ref: assert(r);
		disable_only_empty_ref: assert(r);
		disable_other_match_ref: assert(!gate1 || r_d1 || b_d1);

		gate1 <= 1;
		gate2 <= gate1;
		a_d1 <= a;
		b_d1 <= b;
		r_d1 <= r;
	end

	toplevel_test: assert property(@(posedge clk_i) 1'b0[*0:1]);
	overlap_test: assert property(@(posedge clk_i) a |-> 1'b0[*0:1]);
	overlap_only_empty_test: assert property(@(posedge clk_i) a |-> 1'b0[*0]);
	overlap_other_match_test: assert property(@(posedge clk_i) a |-> (1'b0[*0:1] ##1 b[*0:1]));
	nonoverlap_test: assert property(@(posedge clk_i) a |=> 1'b0[*0:1]);
	negated_test: assert property(@(posedge clk_i) not 1'b0[*0:1]);
	negated_other_match_test: assert property(@(posedge clk_i) not (1'b0[*0:1] ##1 b[*0:1]));
	nexttime_test: assert property(@(posedge clk_i) nexttime 1'b0[*0:1]);
	snexttime_other_match_test: assert property(@(posedge clk_i) s_nexttime (1'b0[*0:1] ##1 b[*0:1]));
	eventually_test: assert property(@(posedge clk_i) eventually [0:1] 1'b0[*0:1]);
	and_left_test: assert property(@(posedge clk_i) 1'b0[*0:1] and (a |-> b));
	and_right_test: assert property(@(posedge clk_i) (a |-> b) and 1'b0[*0:1]);
	and_instance_test: assert property(@(posedge clk_i) implication and 1'b0[*0:1]);
	or_test: assert property(@(posedge clk_i) (a |-> b) or 1'b0[*0:1]);
	disable_test: assert property(@(posedge clk_i) disable iff (r) 1'b0[*0:1]);
	disable_only_empty_test: assert property(@(posedge clk_i) disable iff (r) (a intersect (b ##1 c))[*0:1]);
	disable_other_match_test: assert property(@(posedge clk_i) disable iff (r) (1'b0[*0:1] ##1 b[*0:1]));
endmodule
