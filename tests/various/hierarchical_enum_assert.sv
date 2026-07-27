module hierarchical_enum_child (
	input logic clk_i,
	input logic select_i
);
	typedef enum logic [1:0] {
		Idle,
		Busy
	} state_e;

	state_e state_q;

	always_ff @(posedge clk_i)
		state_q <= select_i ? Busy : Idle;
endmodule

module hierarchical_enum_assert (
	input logic clk_i,
	input logic select_i
);
	hierarchical_enum_child u_child (
		.clk_i,
		.select_i
	);

	EnumLiteral_A: assert property (@(posedge clk_i)
		u_child.state_q inside {u_child.Idle, u_child.Busy});
endmodule
