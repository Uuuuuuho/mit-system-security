// spec_core.v -- REFERENCE SOLUTION (spoilers!): single-cycle ISA specification
// of psp-mini.
//
// The spec executes one whole instruction in the cycle in which `step` is high.
// It is written to read like the ISA manual: every instruction is recognised by
// its complete fixed encoding, and its effect is spelled out in one place.  It
// deliberately shares no code with rtl/psp_mini.v.
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

    // CSR read value (only meaningful when csr_exists)
    reg        csr_exists;
    reg [31:0] csr_val;
    always @* begin
        csr_exists = 1'b1;
        csr_val    = 32'd0;
        case (csr)
            `CSR_MSTATUS: csr_val = {19'd0, {2{mpp}}, 11'd0};
            `CSR_MTVEC:   csr_val = mtvec;
            `CSR_MEPC:    csr_val = mepc;
            `CSR_MCAUSE:  csr_val = {28'd0, mcause};
            default:      csr_exists = 1'b0;
        endcase
    end

    // ------------------------------------------------- next-state function --
    reg [31:0] n_pc, n_mtvec, n_mepc;
    reg [3:0]  n_mcause;
    reg        n_priv, n_mpp;
    reg        we;          // write x[rd] <= wval
    reg [31:0] wval;
    reg        st;          // write mem[st_idx] <= v2
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
            `OP_LUI: begin                                     // LUI
                we = 1'b1; wval = imm_u;
            end
            `OP_JAL: begin                                     // JAL
                we = 1'b1; wval = pc + 32'd4;
                n_pc = pc + imm_j;
            end
            `OP_BRANCH: begin
                if (funct3 == 3'b000) begin                    // BEQ
                    if (v1 == v2) n_pc = pc + imm_b;
                end else illegal = 1'b1;
            end
            `OP_LOAD: begin
                if (funct3 == 3'b010) begin                    // LW
                    addr = v1 + imm_i;
                    we = 1'b1; wval = mem[addr[3:2]];
                end else illegal = 1'b1;
            end
            `OP_STORE: begin
                if (funct3 == 3'b010) begin                    // SW
                    addr = v1 + imm_s;
                    st = 1'b1;
                end else illegal = 1'b1;
            end
            `OP_IMM: begin
                if (funct3 == 3'b000) begin                    // ADDI
                    we = 1'b1; wval = v1 + imm_i;
                end else if (funct3 == 3'b101 && funct7 == 7'b0000000) begin
                    we = 1'b1; wval = v1 >> shamt;             // SRLI
                end else illegal = 1'b1;
            end
            `OP_REG: begin
                if (funct7 == 7'b0000000 && funct3 == 3'b000) begin
                    we = 1'b1; wval = v1 + v2;                 // ADD
                end else if (funct7 == 7'b0000000 && funct3 == 3'b100) begin
                    we = 1'b1; wval = v1 ^ v2;                 // XOR
                end else illegal = 1'b1;
            end
            `OP_SYSTEM: begin
                if (insn == `INSN_ECALL) begin                 // ECALL
                    ecall = 1'b1;
                end else if (insn == `INSN_MRET) begin         // MRET (M-mode only)
                    if (priv == `PRIV_M) begin
                        n_pc   = mepc;
                        n_priv = mpp;
                        n_mpp  = `PRIV_U;
                    end else illegal = 1'b1;
                end else if (funct3 == 3'b001) begin           // CSRRW (M-mode only)
                    if (priv == `PRIV_M && csr_exists) begin
                        we = 1'b1; wval = csr_val;
                        case (csr)
                            `CSR_MSTATUS: n_mpp    = (v1[12:11] == 2'b11);
                            `CSR_MTVEC:   n_mtvec  = {v1[31:2], 2'b00};
                            `CSR_MEPC:    n_mepc   = {v1[31:2], 2'b00};
                            `CSR_MCAUSE:  n_mcause = v1[3:0];
                            default: ;
                        endcase
                    end else illegal = 1'b1;
                end else illegal = 1'b1;
            end
            default: illegal = 1'b1;                           // incl. custom-0
        endcase

        // traps override everything else
        if (illegal || ecall) begin
            we       = 1'b0;
            st       = 1'b0;
            n_mepc   = pc;
            n_mcause = illegal ? `CAUSE_ILLEGAL :
                       (priv == `PRIV_M) ? `CAUSE_ECALL_M : `CAUSE_ECALL_U;
            n_mpp    = priv;
            n_priv   = `PRIV_M;
            n_pc     = mtvec;
        end

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
