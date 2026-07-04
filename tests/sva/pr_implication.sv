module pr_implication(input clk_i, input logic x, input logic y, input logic z);
	reg gate = 0;
	reg past_y;

	always @(posedge clk_i) begin
		overlap_ref: assert(!x || y);
		nonoverlap_ref: assert(!gate || !past_y || z);

		gate <= 1;
		past_y <= y;
	end

	overlap_test: assert property(@(posedge clk_i) x |-> y);
	nonoverlap_test: assert property(@(posedge clk_i) y |=> z);
endmodule
