module parameter_array_assert #(
	parameter logic [1:0] States [2] = '{2'b01, 2'b10}
) (
	input logic clk,
	input logic [1:0] state
);
	ArrayParameter_A: assert property (@(posedge clk)
		state inside {States[0], States[1]});

	for (genvar n = 0; n < 2; n++) begin : gen_state
		GenvarParameter_A: assert property (@(posedge clk)
			state == States[n] |=> state == States[n]);
	end
endmodule
