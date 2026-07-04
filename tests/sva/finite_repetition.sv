module finite_repetition(input clk_i, input logic x);
	reg gate1 = 0;
	reg gate2 = 0;
	reg x_d1;
	reg x_d2;

	always @(posedge clk_i) begin
		repeat_ref: assert(!gate2 || (x_d2 && x_d1 && x));

		gate1 <= 1;
		gate2 <= gate1;
		x_d1 <= x;
		x_d2 <= x_d1;
	end

	repeat_test: assert property(@(posedge clk_i) x[*3]);
endmodule
