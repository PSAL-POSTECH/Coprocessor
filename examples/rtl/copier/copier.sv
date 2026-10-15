// An asynchronous memory copier behind the standard vcix RTL ports: commands queue DEPTH deep and return at once;
// op 0 sets the source, op 1 copies BYTES bytes from it to the address in cmd_x and moves the source past them (copies
// in a row stream one buffer), any other op (wait) does nothing.
// It reads and writes 8 bytes a request on the mem port, one request at a time; busy while a command is left.
module copier #(
  parameter int DEPTH = 4,
  parameter int BYTES = 256
) (
  input  logic        clk,
  input  logic        rst,
  input  logic        cmd_valid,
  output logic        cmd_ready,
  input  logic [1:0]  cmd_op,
  input  logic [63:0] cmd_x,
  output logic        mem_valid,
  input  logic        mem_ready,
  output logic        mem_we,
  output logic [63:0] mem_addr,
  output logic [63:0] mem_wdata,
  input  logic [63:0] mem_rdata,
  output logic        busy
);
  localparam logic [1:0] SRC = 0, COPY = 1;
  logic [1:0]  q_op [DEPTH];
  logic [63:0] q_x [DEPTH];
  logic [7:0]  q_count;
  logic [63:0] src, from, to, data;
  logic [15:0] left;
  logic        active, writing;

  always_comb begin
    cmd_ready = int'(q_count) < DEPTH;
    mem_valid = active;
    mem_we = writing;
    mem_addr = writing ? to : from;
    mem_wdata = data;
    busy = q_count != 0 || active;
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      q_count <= 0; active <= 0; writing <= 0; src <= 0;
    end else begin
      begin : queue
        logic [7:0] n;
        logic       take;
        n = q_count;
        take = !active && q_count != 0;
        if (take) begin
          for (int k = 0; k < DEPTH - 1; k++) begin q_op[k] <= q_op[k+1]; q_x[k] <= q_x[k+1]; end
          n = n - 1;
          if (q_op[0] == SRC) src <= q_x[0];
          if (q_op[0] == COPY) begin
            active <= 1; writing <= 0; from <= src; to <= q_x[0]; left <= 16'(BYTES / 8);
            src <= src + 64'(BYTES);
          end
        end
        if (cmd_valid && cmd_ready) begin
          q_op[int'(n)] <= cmd_op; q_x[int'(n)] <= cmd_x;
          n = n + 1;
        end
        q_count <= n;
      end
      if (active && mem_ready) begin
        if (!writing) begin
          data <= mem_rdata; writing <= 1;
        end else begin
          writing <= 0; from <= from + 8; to <= to + 8; left <= left - 1;
          if (left == 1) active <= 0;
        end
      end
    end
  end
endmodule
