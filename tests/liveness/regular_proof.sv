module regular_sequence_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire request = phase == 0;
	wire prefix = request;
	wire progress = phase == 2;

	assert property (@(posedge clk)
		request |-> strong(prefix ##[0:$] progress));
	cover property (@(posedge clk) request);
endmodule

module regular_sequence_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire request = phase == 0;
	wire prefix = request;
	wire progress = 1'b0;

	assert property (@(posedge clk)
		request |-> strong(prefix ##[0:$] progress));
	cover property (@(posedge clk) request);
endmodule

module regular_intersect_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 0;
	assert property (@(posedge clk)
		strong((##[0:$] a) intersect (##[0:$] b)));
	cover property (@(posedge clk) a && b);
endmodule

module regular_intersect_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 1;
	assert property (@(posedge clk)
		strong((##[0:$] a) intersect (##[0:$] b)));
	cover property (@(posedge clk) a);
endmodule

module regular_goto_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase + 1'b1;

	wire a = phase == 0;
	assert property (@(posedge clk) strong(a[->2]));
	cover property (@(posedge clk) a);
endmodule

module regular_goto_fail ((* gclk *) input logic clk);
	wire a = 1'b0;
	assert property (@(posedge clk) strong(a[->2]));
	cover property (@(posedge clk) !a);
endmodule

module regular_within_pass ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase == 2 ? 0 : phase + 1'b1;

	wire a = phase == 0;
	wire b = phase == 1;
	wire c = phase == 2;
	assert property (@(posedge clk)
		strong((a ##1 b) within (##[0:$] c)));
	cover property (@(posedge clk) c);
endmodule

module regular_within_fail ((* gclk *) input logic clk);
	logic [1:0] phase = 0;
	always_ff @(posedge clk)
		phase <= phase == 2 ? 0 : phase + 1'b1;

	wire a = phase == 0;
	wire b = 1'b0;
	wire c = phase == 2;
	assert property (@(posedge clk)
		strong((a ##1 b) within (##[0:$] c)));
	cover property (@(posedge clk) c);
endmodule
