module finite_nexttime(input clk_i, input logic x, input logic y);
	reg gate1 = 0;
	reg gate2 = 0;
	reg x_d1;

	always @(posedge clk_i) begin
		next_ref: assert(!gate1 || x);
		next2_ref: assert(!gate2 || y);
		snext_implication_ref: assert(!gate1 || !x_d1 || y);

		gate1 <= 1;
		gate2 <= gate1;
		x_d1 <= x;
	end

	next_test: assert property(@(posedge clk_i) nexttime x);
	next2_test: assert property(@(posedge clk_i) nexttime [2] y);
	snext_implication_test: assert property(@(posedge clk_i) x |-> s_nexttime [1] y);
endmodule
