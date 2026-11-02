// The properties that lower to monitor obligations rather than by their paths
// take the procedural condition as the trigger of the monitors: it decides if an
// attempt starts when the statement is reached, the attempt then runs to
// completion regardless (IEEE Std 1800-2017 section 16.14.6).
module procedural_guard_obligations(input logic clk_i, input logic c, input logic e,
									input logic a, input logic b);
	reg c_d1 = 0;
	reg c_e_d1 = 0;
	reg a_d1 = 0;
	reg n_two_d1 = 0;
	reg [1:0] n = 0;

	always @(posedge clk_i) begin
		// The one attempt starts at n == 1 and fails at n == 2, with the condition low by then
		dropped_ref: assert(n != 2);
		if (n == 1)
			dropped_test: assert property (strong((n == 1) ##1 (n == 3)));

		// The one attempt starts at n == 2 and fails a tick later, the match starting
		// at n == 1 doesn't count
		stale_ref: assert(!n_two_d1);
		if (n == 2)
			stale_test: assert property (strong((n == 1) ##1 (n == 2)));

		strong_ref: assert(!c_d1 || (a_d1 && b));
		if (c)
			strong_test: assert property (strong(a ##1 b));

		weak_ref: assert(!c_d1 || (a_d1 && b));
		if (c)
			weak_test: assert property (weak(a ##1 b));

		if_ref: assert(!c_e_d1 || (a_d1 && b));
		if (c)
			if_test: assert property (if (e) a ##1 b);

		assume_ref: assume(!c_d1 || (a_d1 && b));
		if (c)
			assume_test: assume property (strong(a ##1 b));

		cover_ref: cover(c_d1 && a_d1 && b);
		if (c)
			cover_test: cover property (strong(a ##1 b));

		// No possible match, so every attempt fails on its first tick
		no_match_ref: assert(!c);
		if (c)
			no_match_test: assert property (strong((a ##1 b) intersect (b ##2 a)));

		c_d1 <= c;
		c_e_d1 <= c && e;
		a_d1 <= a;
		n_two_d1 <= n == 2;
		if (n != 3)
			n <= n + 1;
	end
endmodule
