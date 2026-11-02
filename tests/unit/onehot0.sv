module onehot0_test #(parameter int WIDTH = 8) (input logic [WIDTH-1:0] val);
    function automatic bit reference_onehot0(logic [WIDTH-1:0] v);
        int reference_count = 0;
        for (int i = 0; i < WIDTH; i++)
            reference_count += v[i];
        if (reference_count == 1) return 1;
        else if (reference_count == 0) return 1;
        else return 0;
    endfunction

    always_comb if (^val !== 'x) begin
        assert($onehot0(val) == reference_onehot0(val));
    end
endmodule

module onehot0_1bit(input logic val);
    onehot0_test #(.WIDTH(1)) t(.val(val));
endmodule

module onehot0_8bit(input logic [7:0] val);
    onehot0_test #(.WIDTH(8)) t(.val(val));
endmodule

module onehot0_32bit(input logic [31:0] val);
    onehot0_test #(.WIDTH(32)) t(.val(val));
endmodule

module onehot0_constants;
    always_comb begin
        assert($onehot0(1'b1));
        assert($onehot0(1'b0));
        assert($onehot0(8'b00000000));
        assert($onehot0(8'b00000001));
        assert($onehot0(8'b10000000));
        assert(!$onehot0(8'b10000001));
        assert(!$onehot0(8'b11111111));
        assert(!$onehot0(5'b10101));
    end
endmodule
