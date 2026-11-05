// The properties lower_property() takes apart keep to Annex F under a disable as well.
// Their finite parts take the letters of the disable as a sequence does, so a failure
// settled ahead of the disable stands, and an accept_on goes the same way for them. A
// cover gets a success of the T letters of an accept_on, while the letters of its disable
// iff are the dual of T.
module disable_obligations(input clk_i, input logic r, input logic x, input logic a,
						   input logic b, input logic c, input logic d);
	reg gate1 = 0;
	reg gate2 = 0;
	reg r_d1, r_d2;
	reg x_d1;
	reg a_d1, a_d2;
	reg b_d1, b_d2;
	reg c_d1;

	always @(posedge clk_i) begin
		strong_ref: assert(!gate2 || r_d2 || (a_d2 && (r_d1 || (b_d1 && (r || c)))));
		accept_ref: assert(!gate2 || r_d2 || (a_d2 && (r_d1 || (b_d1 && (r || c)))));
		range_ref: assert(!gate2 || r_d2 || (a_d2 && (r_d1 || (b_d2 && c_d1) || (b_d1 && (r || c)))));
		// The assumption rules out an attempt without `a` on its first tick as of that
		// tick, as no disable later on can cancel that
		assume_ref: assume((r || a) && (!gate1 || r_d1 || (a_d1 && (r || b))));

		// The condition sees the dual letter, as the antecedent of an implication does
		if_ref: assert(!gate1 || !c_d1 || r_d1 || (a_d1 && (r || b)));
		implies_ref: assert(!gate1 || !c_d1 || r_d1 || (a_d1 && (r || b)));
		iff_ref: assert(r || a == b);

		// The antecedent ends with `b` on the tick before, its consequent fails there
		// or on this tick
		unbounded_ref: assert(!gate2 || !(a_d2 && !r_d2 && b_d1 && !r_d1) || (c_d1 && (r || d)));
		unbounded_next_ref: assert(!gate2 || !(a_d2 && !r_d2 && b_d1 && !r_d1) || r || c);

		cover_ref: cover(gate1 && a_d1 && !r_d1 && (b_d1 || (b && !r)));
		cover_accept_ref: cover(gate1 && (a_d1 || x_d1) && (x_d1 || x || b));
		// The accept settles the attempt, a disable on a later tick has no bearing on it
		cover_both_ref: cover(gate1 && !r_d1 && (x_d1 || !r) && (a_d1 || x_d1) && (x_d1 || x || b));
		cover_not_ref: cover(gate1 && !r_d1 && (x_d1 || (r ? !a_d1 : x || !(a_d1 && b))));
		cover_false_ref: cover(x);
		cover_iff_ref: cover(x || a == b);
		cover_iff_disable_ref: cover(!r && a == b);

		gate1 <= 1;
		gate2 <= gate1;
		r_d1 <= r;
		r_d2 <= r_d1;
		x_d1 <= x;
		a_d1 <= a;
		a_d2 <= a_d1;
		b_d1 <= b;
		b_d2 <= b_d1;
		c_d1 <= c;
	end

	strong_test: assert property(@(posedge clk_i) disable iff (r) strong(a ##1 b ##1 c));
	accept_test: assert property(@(posedge clk_i) accept_on (r) a ##1 b ##1 c);
	range_test: assert property(@(posedge clk_i) disable iff (r) strong(a ##[0:1] b ##1 c));
	assume_test: assume property(@(posedge clk_i) disable iff (r) strong(a ##1 b));
	if_test: assert property(@(posedge clk_i) disable iff (r) if (c) strong(a ##1 b));
	implies_test: assert property(@(posedge clk_i) disable iff (r) c implies strong(a ##1 b));
	iff_test: assert property(@(posedge clk_i) disable iff (r) a iff b);
	unbounded_test: assert property(@(posedge clk_i) disable iff (r) a[*1:$] ##1 b |-> c ##1 d);
	unbounded_next_test: assert property(@(posedge clk_i) disable iff (r) a[*1:$] ##1 b |=> c);

	cover_test: cover property(@(posedge clk_i) disable iff (r) strong(a ##[0:1] b));
	cover_accept_test: cover property(@(posedge clk_i) accept_on (x) strong(a ##1 b));
	cover_both_test: cover property(@(posedge clk_i) disable iff (r) accept_on (x) strong(a ##1 b));
	cover_not_test: cover property(@(posedge clk_i) disable iff (r) accept_on (x) not (a ##1 b));
	cover_false_test: cover property(@(posedge clk_i) accept_on (x) 1'b0);
	cover_iff_test: cover property(@(posedge clk_i) accept_on (x) (a iff b));
	cover_iff_disable_test: cover property(@(posedge clk_i) disable iff (r) a iff b);
endmodule
