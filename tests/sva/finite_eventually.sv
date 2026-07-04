module finite_eventually(input clk_i, input logic x, input logic y);
	reg gate1 = 0;
	reg gate2 = 0;
	reg x_d1;
	reg x_d2;
	reg y_d1;
	reg y_d2;

	always @(posedge clk_i) begin
		eventually_ref: assert(!gate2 || !x_d2 || y_d2 || y_d1 || y);

		gate1 <= 1;
		gate2 <= gate1;
		x_d1 <= x;
		x_d2 <= x_d1;
		y_d1 <= y;
		y_d2 <= y_d1;
	end

	eventually_test: assert property(@(posedge clk_i) x |-> eventually [0:2] y);
endmodule
