module onehot_functions(input logic [2:0] a);
	always_comb begin
		onehot_ref: assert((a == 3'b001) || (a == 3'b010) || (a == 3'b100));
		onehot0_ref: assert((a == 3'b000) || (a == 3'b001) || (a == 3'b010) || (a == 3'b100));
	end

	onehot_test: assert property($onehot(a));
	onehot0_test: assert property($onehot0(a));
endmodule
