module finite_intersect(input clk_i, input logic x, input logic y, input logic z);
	reg gate = 0;
	reg x_d1;
	reg z_d1;

	always @(posedge clk_i) begin
		intersect_ref: assert(!gate || (x_d1 && z_d1 && y));

		gate <= 1;
		x_d1 <= x;
		z_d1 <= z;
	end

	intersect_test: assert property(@(posedge clk_i) (x ##1 y) intersect (z ##1 y));
endmodule
