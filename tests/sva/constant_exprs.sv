package constant_exprs_pkg;
	typedef enum logic [1:0] { PKG_IDLE = 2'd1, PKG_BUSY = 2'd2 } pkg_state_e;
endpackage

module constant_exprs #(
	parameter int ARR [2] = '{5, 9},
	parameter real RATE = 2.5,
	parameter string NAME = "abc"
) (input logic clk_i, input logic [1:0] st, input logic [3:0] v, input logic [7:0] x);
	typedef enum logic [1:0] { IDLE = 2'd1, BUSY = 2'd2, DONE = 2'd3 } state_e;
	state_e s;

	function automatic int fact(int n);
		return n <= 1 ? 1 : n * fact(n - 1);
	endfunction

	function automatic logic is_idle(logic [1:0] state);
		return state == IDLE;
	endfunction

	function automatic logic [7:0] code(logic [1:0] state);
		case (state)
			BUSY: return 8'd7;
			default: return 8'd1;
		endcase
	endfunction

	logic valid = 1'b0;
	logic busy_q = 1'b0;
	logic [1:0] st_q;

	always @(posedge clk_i) begin
		enum_ref: assert(st == 2'd1);
		enum_pkg_ref: assert(st == 2'd2);
		enum_first_ref: assert(st == 2'd1);
		enum_num_ref: assert(x == 8'd3);
		enum_temporal_ref: assert(!busy_q || st == 2'd3);
		enum_function_ref: assert(st == 2'd1);
		enum_case_ref: assert(x == (st == 2'd2 ? 8'd7 : 8'd1));
		enum_past_ref: assert(!valid || st_q == 2'd2);

		bits_ref: assert(v[3]);
		size_ref: assert(x == 8'd4);
		high_ref: assert(x == 8'd3);
		low_ref: assert(x == 8'd0);
		left_ref: assert(x == 8'd3);
		right_ref: assert(x == 8'd0);
		increment_ref: assert(x == 8'd1);
		dimensions_ref: assert(x == 8'd1);
		unpacked_dimensions_ref: assert(x == 8'd0);

		array_ref: assert(x == 8'd9);
		array_sum_ref: assert(x == 8'd14);
		real_ref: assert(x[0]);
		real_cast_ref: assert(x == 8'd3);
		real_var_ref: assert(x[0]);
		string_ref: assert(x[0]);
		string_len_ref: assert(x == 8'd3);
		string_char_ref: assert(x == 8'h61);
		type_ref: assert(x[0]);

		function_ref: assert(x == 8'd6);

		valid <= 1'b1;
		busy_q <= st == 2'd2;
		st_q <= st;
	end

	enum_test: assert property(@(posedge clk_i) st == IDLE);
	enum_pkg_test: assert property(@(posedge clk_i) st == constant_exprs_pkg::PKG_BUSY);
	enum_first_test: assert property(@(posedge clk_i) st == s.first());
	enum_num_test: assert property(@(posedge clk_i) x == s.num());
	enum_temporal_test: assert property(@(posedge clk_i) (st == BUSY) |=> (st == DONE));
	enum_function_test: assert property(@(posedge clk_i) is_idle(st));
	enum_case_test: assert property(@(posedge clk_i) x == code(st));
	enum_past_test: assert property(@(posedge clk_i) disable iff (!valid) $past(st) == BUSY);

	bits_test: assert property(@(posedge clk_i) v[$bits(v) - 1]);
	size_test: assert property(@(posedge clk_i) x == $size(v));
	high_test: assert property(@(posedge clk_i) x == $high(v));
	low_test: assert property(@(posedge clk_i) x == $low(v));
	left_test: assert property(@(posedge clk_i) x == $left(v));
	right_test: assert property(@(posedge clk_i) x == $right(v));
	increment_test: assert property(@(posedge clk_i) x == $increment(v));
	dimensions_test: assert property(@(posedge clk_i) x == $dimensions(v));
	unpacked_dimensions_test: assert property(@(posedge clk_i) x == $unpacked_dimensions(v));

	array_test: assert property(@(posedge clk_i) x == ARR[1]);
	array_sum_test: assert property(@(posedge clk_i) x == ARR.sum());
	real_test: assert property(@(posedge clk_i) RATE > 1.5 && x[0]);
	real_cast_test: assert property(@(posedge clk_i) x == int'(RATE));
	real_var_test: assert property(@(posedge clk_i) real'(ARR[0]) > 1.5 && x[0]);
	string_test: assert property(@(posedge clk_i) NAME == "abc" && x[0]);
	string_len_test: assert property(@(posedge clk_i) x == NAME.len());
	string_char_test: assert property(@(posedge clk_i) x == NAME[0]);
	type_test: assert property(@(posedge clk_i) type(x) == type(v) || x[0]);

	function_test: assert property(@(posedge clk_i) x == fact(3));

	for (genvar i = 0; i < 2; i++) begin : gen
		always @(posedge clk_i) begin
			select_ref: assert(v[i]);
			value_ref: assert(x == i);
			temporal_ref: assert(!valid || v[i]);
		end

		select_test: assert property(@(posedge clk_i) v[i]);
		value_test: assert property(@(posedge clk_i) x == i);
		temporal_test: assert property(@(posedge clk_i) 1'b1 ##1 v[i]);
	end
endmodule
