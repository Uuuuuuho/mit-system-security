// spec_core.v -- single-cycle ISA specification of psp-mini   (STUDENT FILE)
//
// The spec executes one whole instruction in the cycle in which `step` is high:
// all of its effects are computed combinationally from (current state, insn)
// in the always @* block below and become visible after the clock edge.
//
// Write it like the ISA manual (README section "The psp-mini ISA"), NOT like
// rtl/psp_mini.v: recognise each instruction by its complete encoding and spell
// out its effect.  ADDI is done for you.
//
//   Part 1: LUI, ADD, XOR, SRLI
//   Part 3: JAL, BEQ, LW, SW, ECALL, MRET, CSRRW, illegal instructions + traps
//
// The reset/sync/step state update at the bottom is complete; you only need to
// fill in the next-state function.
`include "psp_defs.vh"

module spec_core (
    input  wire                      clk,
    input  wire                      rst,
    input  wire                      step,      // execute `insn` in this cycle
    input  wire [31:0]               insn,      // the instruction at address spec_pc
    // re-anchoring port: when `sync` is high, overwrite the whole architectural
    // state with the sync_* values (the miter uses this, see miter.v)
    input  wire                      sync,
    input  wire [31:0]               sync_pc,
    input  wire                      sync_priv,
    input  wire [32*`NREGS-1:0]      sync_regs,
    input  wire [31:0]               sync_mtvec,
    input  wire [31:0]               sync_mepc,
    input  wire [3:0]                sync_mcause,
    input  wire                      sync_mpp,
    input  wire [32*`DMEM_WORDS-1:0] sync_dmem,
    output wire [31:0]               spec_pc,
    // architectural state (same layout as psp_mini's dbg_* ports)
    output wire                      dbg_priv,
    output wire [32*`NREGS-1:0]      dbg_regs,
    output wire [31:0]               dbg_mtvec,
    output wire [31:0]               dbg_mepc,
    output wire [3:0]                dbg_mcause,
    output wire                      dbg_mpp,
    output wire [32*`DMEM_WORDS-1:0] dbg_dmem
);
    // ---------------------------------------------------------------- state --
    reg [31:0] pc;
    reg        priv;
    reg [31:0] mtvec, mepc;
    reg [3:0]  mcause;
    reg        mpp;
    reg [31:0] x   [0:`NREGS-1];
    reg [31:0] mem [0:`DMEM_WORDS-1];

    // --------------------------------------------------------------- fields --
    wire [6:0]  opcode = insn[6:0];
    wire [2:0]  funct3 = insn[14:12];
    wire [6:0]  funct7 = insn[31:25];
    wire [1:0]  rd     = insn[8:7];      // 4 registers: low 2 bits only
    wire [1:0]  rs1    = insn[16:15];
    wire [1:0]  rs2    = insn[21:20];
    wire [4:0]  shamt  = insn[24:20];
    wire [11:0] csr    = insn[31:20];

    wire [31:0] imm_i = {{20{insn[31]}}, insn[31:20]};
    wire [31:0] imm_s = {{20{insn[31]}}, insn[31:25], insn[11:7]};
    wire [31:0] imm_b = {{19{insn[31]}}, insn[31], insn[7], insn[30:25], insn[11:8], 1'b0};
    wire [31:0] imm_u = {insn[31:12], 12'b0};
    wire [31:0] imm_j = {{11{insn[31]}}, insn[31], insn[19:12], insn[20], insn[30:21], 1'b0};

    // register reads: x0 always reads as zero
    wire [31:0] v1 = (rs1 == 2'd0) ? 32'd0 : x[rs1];
    wire [31:0] v2 = (rs2 == 2'd0) ? 32'd0 : x[rs2];

    // CSR read value.  TODO (Part 3): mark the four implemented CSRs as existing
    // and return their values (mstatus: MPP in bits [12:11], everything else 0;
    // mcause: zero-extended).
    reg        csr_exists;
    reg [31:0] csr_val;
    always @* begin
        csr_exists = 1'b0;
        csr_val    = 32'd0;
        case (csr)
            default: ;
        endcase
    end

    // ------------------------------------------------- next-state function --
    reg [31:0] n_pc, n_mtvec, n_mepc;
    reg [3:0]  n_mcause;
    reg        n_priv, n_mpp;
    reg        we;          // write x[rd] <= wval   (writes to x0 are dropped below)
    reg [31:0] wval;
    reg        st;          // write mem[addr[3:2]] <= v2
    reg [31:0] addr;
    reg        illegal, ecall;

    always @* begin
        // defaults: fall through to the next instruction, change nothing else
        n_pc     = pc + 32'd4;
        n_priv   = priv;
        n_mtvec  = mtvec;
        n_mepc   = mepc;
        n_mcause = mcause;
        n_mpp    = mpp;
        we       = 1'b0;
        wval     = 32'd0;
        st       = 1'b0;
        addr     = 32'd0;
        illegal  = 1'b0;
        ecall    = 1'b0;

        case (opcode)
            `OP_IMM: begin
                if (funct3 == 3'b000) begin                    // ADDI (done)
                    we = 1'b1; wval = v1 + imm_i;
                end
                // TODO (Part 1): SRLI
                // TODO (Part 3): every other OP-IMM encoding is illegal
            end
            // TODO (Part 1): LUI, ADD, XOR
            // TODO (Part 3): JAL, BEQ, LW, SW, ECALL, MRET, CSRRW, and make
            //                everything that is not in the ISA illegal
            default: ;
        endcase

        // TODO (Part 3): a trap (illegal instruction or ECALL) overrides all of
        // the above: no register or memory write; mepc = pc, mcause = cause,
        // mpp = priv, priv = M, next pc = mtvec.

        n_pc = {n_pc[31:2], 2'b00};     // targets are always word aligned
    end

    // --------------------------------------------------------- state update --
    integer i;
    always @(posedge clk) begin
        if (rst) begin
            pc     <= 32'd0;
            priv   <= `PRIV_M;
            mtvec  <= 32'd0;
            mepc   <= 32'd0;
            mcause <= 4'd0;
            mpp    <= `PRIV_U;
            for (i = 0; i < `NREGS; i = i + 1)      x[i]   <= 32'd0;
            for (i = 0; i < `DMEM_WORDS; i = i + 1) mem[i] <= 32'd0;
        end else if (sync) begin
            pc     <= sync_pc;
            priv   <= sync_priv;
            mtvec  <= sync_mtvec;
            mepc   <= sync_mepc;
            mcause <= sync_mcause;
            mpp    <= sync_mpp;
            for (i = 0; i < `NREGS; i = i + 1)      x[i]   <= sync_regs[32*i +: 32];
            for (i = 0; i < `DMEM_WORDS; i = i + 1) mem[i] <= sync_dmem[32*i +: 32];
        end else if (step) begin
            pc     <= n_pc;
            priv   <= n_priv;
            mtvec  <= n_mtvec;
            mepc   <= n_mepc;
            mcause <= n_mcause;
            mpp    <= n_mpp;
            if (we && rd != 2'd0) x[rd] <= wval;
            if (st) mem[addr[3:2]] <= v2;
        end
    end

    // ---------------------------------------------------------------- ports --
    assign spec_pc    = pc;
    assign dbg_priv   = priv;
    assign dbg_mtvec  = mtvec;
    assign dbg_mepc   = mepc;
    assign dbg_mcause = mcause;
    assign dbg_mpp    = mpp;

    genvar g;
    generate
        for (g = 0; g < `NREGS; g = g + 1) begin : g_regs
            assign dbg_regs[32*g +: 32] = (g == 0) ? 32'd0 : x[g];
        end
        for (g = 0; g < `DMEM_WORDS; g = g + 1) begin : g_mem
            assign dbg_dmem[32*g +: 32] = mem[g];
        end
    endgenerate
endmodule
