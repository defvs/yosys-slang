module liveness_composition (
	input logic clk,
	input logic reset,
	input logic select_a,
	input logic a,
	input logic b,
	input logic hold
);
	both_progress: assert property (@(posedge clk)
		(s_eventually a) and (s_eventually b));

	selected_progress: assert property (@(posedge clk)
		if (select_a) s_eventually a else s_eventually b);

	accepted_on_reset: assert property (@(posedge clk)
		accept_on (reset) s_eventually a);

	rejected_on_reset: assert property (@(posedge clk)
		reject_on (reset) s_eventually a);

	always_hold: assert property (@(posedge clk) always hold);

	weak_until_b: assert property (@(posedge clk) hold until b);

	strong_until_with_b: assert property (@(posedge clk)
		hold s_until_with b);

	implied_progress: assert property (@(posedge clk)
		hold implies s_eventually a);

	equivalent_now: assert property (@(posedge clk)
		a iff b);

	case_progress: assert property (@(posedge clk)
		case (select_a)
			1'b1: s_eventually a;
			default: s_eventually b;
		endcase);

	followed_progress: assert property (@(posedge clk)
		hold #-# s_eventually a);

	not_always_progress: assert property (@(posedge clk)
		not (always (!a)));

	not_eventual_hold: assert property (@(posedge clk)
		not (s_eventually (!hold)));
endmodule
