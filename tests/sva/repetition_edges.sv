module repetition_edges(input clk_i, input logic x, input logic y);
	reg gate = 0;
	reg x_d1;

	always @(posedge clk_i) begin
		repeat0_ref: assert(!gate || y);
		repeat01_ref: assert(!gate || y);
		repeat11_ref: assert(!gate || (x_d1 && y));

		gate <= 1;
		x_d1 <= x;
	end

	repeat0_test: assert property(@(posedge clk_i) x[*0] ##1 y);
	repeat01_test: assert property(@(posedge clk_i) x[*0:1] ##1 y);
	repeat11_test: assert property(@(posedge clk_i) x[*1:1] ##1 y);
endmodule
