module sampled_blocking(input logic clk_i, input logic [3:0] v);
	logic y = 1'b0;
	logic gate = 1'b0;

	always @(posedge clk_i) begin
		// A blocking assignment doesn't change the sampled value of a variable
		y = 1'b1;
		blocking_ref: assert(gate);
		blocking_test: assert property(y);

		for (int i = 3; i < 4; i++) begin
			loop_ref: assert(v[3]);
			loop_test: assert property(v[i]);
		end

		gate <= 1'b1;
	end
endmodule
