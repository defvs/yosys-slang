module finite_throughout(input clk_i, input logic x, input logic y, input logic z);
	reg gate1 = 0;
	reg gate2 = 0;
	reg x_d1;
	reg x_d2;
	reg y_d2;

	always @(posedge clk_i) begin
		throughout_ref: assert(!gate2 || (x_d2 && x_d1 && x && y_d2 && z));

		gate1 <= 1;
		gate2 <= gate1;
		x_d1 <= x;
		x_d2 <= x_d1;
		y_d2 <= y;
	end

	throughout_test: assert property(@(posedge clk_i) x throughout (y ##2 z));
endmodule
