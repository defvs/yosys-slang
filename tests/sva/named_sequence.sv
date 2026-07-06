module named_sequence(input clk_i, input logic start, input logic a, input logic b,
					  input logic done);
	sequence handshake_s;
		a ##1 b;
	endsequence

	reg gate1 = 0;
	reg gate2 = 0;
	reg start_d1;
	reg start_d2;
	reg a_d1;

	always @(posedge clk_i) begin
		named_ref: assert(!gate1 || (a_d1 && b));
		compose_ref: assert(!gate2 || !start_d2 || (a_d1 && b && done));

		gate1 <= 1;
		gate2 <= gate1;
		start_d1 <= start;
		start_d2 <= start_d1;
		a_d1 <= a;
	end

	named_test: assert property(@(posedge clk_i) handshake_s);
	compose_test: assert property(@(posedge clk_i) start |=> handshake_s ##0 done);
endmodule
