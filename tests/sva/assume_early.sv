// An assumption rules out an attempt on the first tick its property can't hold any
// more, whatever comes next, as well as on its last tick. A trace that ends in between
// would be left unconstrained otherwise. The references spell out on which ticks an
// attempt queued `k` ticks ago gets rejected, from the values it sampled so far.
module assume_early(input clk_i, input logic a, input logic b, input logic c, input logic d,
					input logic e, input logic r, input logic en,
					input logic start, input logic busy, input logic done);
	reg gate1 = 0, gate2 = 0, gate3 = 0;
	reg en_d1 = 0;
	reg a_d1, a_d2, a_d3;
	reg b_d1, b_d2, b_d3;
	reg c_d1, c_d2;
	reg d_d1, d_d2;
	reg r_d1, r_d2;
	reg start_d1, start_d2, start_d3;
	reg busy_d1, busy_d2, busy_d3;

	always @(posedge clk_i) begin
		// Without `busy` on the tick of `start` the attempt is doomed right away
		busy_ref: assume(!(start && !busy)
				&& !(gate1 && start_d1 && !(busy_d1 && busy))
				&& !(gate2 && start_d2 && !(busy_d2 && busy_d1 && busy))
				&& !(gate3 && start_d3 && !(busy_d3 && busy_d2 && busy_d1 && done)));

		chain_ref: assume(!(a && !b)
				&& !(gate1 && a_d1 && !(b_d1 && c))
				&& !(gate2 && a_d2 && !(b_d2 && c_d1 && d)));

		// Nothing to go by before the consequent starts
		nonoverlap_ref: assume(!(gate1 && a_d1 && !b)
				&& !(gate2 && a_d2 && !(b_d1 && b))
				&& !(gate3 && a_d3 && !(b_d2 && b_d1 && c)));

		// Both operands of `and` are needed, either one failing rejects
		and_ref: assume(!(a && !(b && d))
				&& !(gate1 && a_d1 && !(b_d1 && c && d_d1))
				&& !(gate2 && a_d2 && !(b_d2 && c_d1 && d_d2 && e)));

		// A failure settled before the disable stands, and is rejected on its tick
		// unless the disable came first. With the disable on the tick after a missing
		// `c`, the attempt holds after all, as `c` isn't needed then.
		disable_ref: assume(!(!r && a && !b)
				&& !(gate1 && !r_d1 && a_d1 && (!b_d1 || (!r && !c)))
				&& !(gate2 && !(r_d2 || !a_d2 || (b_d2 && (r_d1 || (c_d1 && (r || d)))))));

		// The antecedent has to match first
		antecedent_ref: assume(!(gate1 && a_d1 && b && !c)
				&& !(gate2 && a_d2 && b_d1 && !(c_d1 && d)));

		proc_ref: assume(!(en && a && !b)
				&& !(en_d1 && a_d1 && !(b_d1 && c)));

		// A later tick of the range can still have `b`
		range_ref: assume(!(gate3 && a_d3 && !(b_d2 || b_d1 || b)));

		// Rejected only once `b` stops before a `c`
		repeat_ref: assume(!(a && !b)
				&& !(gate1 && a_d1 && !(b_d1 && (c || b)))
				&& !(gate2 && a_d2 && !(b_d2 && (c_d1 || (b_d1 && (c || b)))))
				&& !(gate3 && a_d3 && !(b_d3 && (c_d2 || (b_d2 && (c_d1 || (b_d1 && c)))))));

		throughout_ref: assume(!(a && !(c && b))
				&& !(gate1 && a_d1 && !(c_d1 && c && b_d1))
				&& !(gate2 && a_d2 && !(c_d2 && c_d1 && b_d2 && (d_d1 || (c && d)))));

		first_match_ref: assume(!(a && !b)
				&& !(gate1 && a_d1 && !b_d1)
				&& !(gate2 && a_d2 && !(b_d2 && ((c_d1 && d) || (!c_d1 && c))))
				&& !(gate3 && a_d3 && !(b_d3 && ((c_d2 && d_d1) || (!c_d2 && c_d1 && d)))));

		// A negated sequence can't fail before the sequence matches
		not_sequence_ref: assume(!(gate1 && a_d1 && b));

		// The negated implication fails without `a`, but with `a` it waits for `c`
		not_implication_ref: assume(a && !(gate1 && !(a_d1 && !(b_d1 && c))));

		// A consequent with no match rejects on the tick of the antecedent, before the
		// tick the attempt ends on
		no_match_ref: assume(!a && !(gate1 && a_d1));
		only_empty_ref: assume(!a && !(gate1 && a_d1));
		constant_ref: assume(!a && !(gate2 && a_d2));

		gate1 <= 1;
		gate2 <= gate1;
		gate3 <= gate2;
		en_d1 <= en;
		a_d1 <= a;
		a_d2 <= a_d1;
		a_d3 <= a_d2;
		b_d1 <= b;
		b_d2 <= b_d1;
		b_d3 <= b_d2;
		c_d1 <= c;
		c_d2 <= c_d1;
		d_d1 <= d;
		d_d2 <= d_d1;
		r_d1 <= r;
		r_d2 <= r_d1;
		start_d1 <= start;
		start_d2 <= start_d1;
		start_d3 <= start_d2;
		busy_d1 <= busy;
		busy_d2 <= busy_d1;
		busy_d3 <= busy_d2;

		if (en)
			proc_test: assume property (a |-> b ##1 c);
	end

	busy_test: assume property (@(posedge clk_i) start |-> busy[*3] ##1 done);
	chain_test: assume property (@(posedge clk_i) a |-> b ##1 c ##1 d);
	nonoverlap_test: assume property (@(posedge clk_i) a |=> b[*2] ##1 c);
	and_test: assume property (@(posedge clk_i) a |-> (b ##1 c) and (d ##2 e));
	disable_test: assume property (@(posedge clk_i) disable iff (r) a |-> b ##1 c ##1 d);
	antecedent_test: assume property (@(posedge clk_i) a ##1 b |-> c ##1 d);
	range_test: assume property (@(posedge clk_i) a |-> ##[1:3] b);
	repeat_test: assume property (@(posedge clk_i) a |-> b[*1:3] ##1 c);
	throughout_test: assume property (@(posedge clk_i) a |-> c throughout (b ##[1:2] d));
	first_match_test: assume property (@(posedge clk_i) a |-> first_match(b ##[1:2] c) ##1 d);
	not_sequence_test: assume property (@(posedge clk_i) not (a ##1 b));
	not_implication_test: assume property (@(posedge clk_i) not (a |-> b ##1 c));
	no_match_test: assume property (@(posedge clk_i) a |=> 1'b0[*0:1]);
	only_empty_test: assume property (@(posedge clk_i) a ##1 1'b1 |-> 1'b0[*0]);
	constant_test: assume property (@(posedge clk_i) a |-> ##2 1'b0);
endmodule
