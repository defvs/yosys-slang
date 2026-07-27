module enum_next(
	input logic [1:0] in,
	output logic [1:0] next1,
	output logic [1:0] next2,
	output logic [1:0] prev1,
	output logic [1:0] prev2
);
	typedef enum logic [1:0] {
		A = 2'b00,
		B = 2'b10,
		C = 2'b11
	} state_e;

	state_e state;
	always_comb begin
		state = state_e'(in);
		next1 = state.next();
		next2 = state.next(2);
		prev1 = state.prev();
		prev2 = state.prev(2);
	end
endmodule
