module alert_pattern_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;
	wire alert_req = age < 4;
	wire alert_ack = age >= 4;
	assert property (@(posedge clk)
		alert_req |-> strong(##[1:$] alert_ack));
	cover property (@(posedge clk) alert_req);
endmodule

module alert_pattern_fail ((* gclk *) input logic clk);
	wire alert_req = 1'b1;
	wire alert_ack = 1'b0;
	assert property (@(posedge clk)
		alert_req |-> strong(##[1:$] alert_ack));
endmodule

module esc_pattern_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;
	wire esc_req = age < 2;
	assert property (@(posedge clk)
		esc_req |-> strong(##[1:$] !esc_req[*3]));
	cover property (@(posedge clk) esc_req);
endmodule

module esc_pattern_fail ((* gclk *) input logic clk);
	wire esc_req = 1'b1;
	assert property (@(posedge clk)
		esc_req |-> strong(##[1:$] !esc_req[*3]));
endmodule

module sha3_pattern_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;
	wire process = age == 0;
	wire keccak_run = age >= 3;
	assert property (@(posedge clk)
		process |-> strong(##[2:$] keccak_run));
	cover property (@(posedge clk) process);
endmodule

module sha3_pattern_fail ((* gclk *) input logic clk);
	wire process = 1'b1;
	wire keccak_run = 1'b0;
	assert property (@(posedge clk)
		process |-> strong(##[2:$] keccak_run));
endmodule

module tlul_pattern_pass ((* gclk *) input logic clk);
	logic [2:0] age = 0;
	always_ff @(posedge clk)
		if (age != 7)
			age <= age + 1'b1;
	wire request_error = age == 0;
	wire d_error = age >= 2;
	assert property (@(posedge clk)
		request_error |=> s_eventually d_error);
	cover property (@(posedge clk) request_error);
endmodule

module tlul_pattern_fail ((* gclk *) input logic clk);
	wire request_error = 1'b1;
	wire d_error = 1'b0;
	assert property (@(posedge clk)
		request_error |=> s_eventually d_error);
endmodule
