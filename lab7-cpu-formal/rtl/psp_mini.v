// psp_mini.v -- multi-cycle implementation of the psp-mini ISA.
//
// This is the design under verification (DUV).  Treat it like third-party IP:
// you are allowed to read it, but the point of the lab is to find its problems
// with a spec + miter + bounded model checking, not by code review.
//
// Micro-architecture: one instruction at a time, four states per instruction
//
//     FETCH  -> latch the instruction word from the instruction port
//     DECODE -> classify the instruction, read the register file, pick the immediate
//     EXEC   -> ALU / branch-target / CSR-read, compute the next pc
//     WB     -> commit: write rd / memory / CSRs, take traps, update pc and privilege
//
// so every instruction takes exactly 4 cycles and `commit` pulses in WB.  All
// architectural state changes happen at the clock edge that ends the WB cycle.
//
// Build-time switches (Verilog defines):
//   PSP_GLITCH      plant defect #1: the ALU adder is wrong for certain operands
//   PSP_BACKDOOR    plant defect #2: an undocumented instruction
//   PSP_MUTANT=n    plant a known, simple bug n (1..14) -- used to sanity-check
//                   a verification environment; see README "Mutants"
// The magic constants of the two defects live in defects.vh, which is generated
// per SEED by tools/gen_defects.py.
`include "psp_defs.vh"
`include "defects.vh"

`ifndef PSP_MUTANT
`define PSP_MUTANT 0
`endif

module psp_mini (
    input  wire                      clk,
    input  wire                      rst,
    // instruction port: combinational read, sampled while imem_en is high
    output wire                      imem_en,
    output wire [31:0]               imem_addr,
    input  wire [31:0]               imem_rdata,
    // commit (retirement) port: one pulse per finished instruction
    output wire                      commit,
    output wire [31:0]               commit_pc,
    output wire [31:0]               commit_insn,
    // architectural state, exported for verification and debugging only
    output wire [31:0]               dbg_pc,
    output wire                      dbg_priv,
    output wire [32*`NREGS-1:0]      dbg_regs,     // x0 in bits [31:0], x1 in [63:32], ...
    output wire [31:0]               dbg_mtvec,
    output wire [31:0]               dbg_mepc,
    output wire [3:0]                dbg_mcause,
    output wire                      dbg_mpp,
    output wire [32*`DMEM_WORDS-1:0] dbg_dmem      // word 0 in bits [31:0], ...
);
    localparam integer MUT = `PSP_MUTANT;

    // ------------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------------
    localparam [1:0] S_FETCH = 2'd0, S_DECODE = 2'd1, S_EXEC = 2'd2, S_WB = 2'd3;

    // instruction classes produced by DECODE
    localparam [3:0] K_ILL  = 4'd0,  K_LUI  = 4'd1,  K_JAL   = 4'd2,  K_BEQ   = 4'd3,
                     K_LW   = 4'd4,  K_SW   = 4'd5,  K_ADDI  = 4'd6,  K_SRLI  = 4'd7,
                     K_ADD  = 4'd8,  K_XOR  = 4'd9,  K_ECALL = 4'd10, K_MRET  = 4'd11,
                     K_CSRRW = 4'd12, K_BD  = 4'd13;

    // architectural state
    reg [31:0] pc;
    reg        priv;
    reg [31:0] mtvec, mepc;
    reg [3:0]  mcause;
    reg        mpp;
    reg [31:0] rf   [0:`NREGS-1];
    reg [31:0] dmem [0:`DMEM_WORDS-1];

    // micro-architectural state
    reg [1:0]  state;
    reg [31:0] ir;          // instruction register (FETCH)
    reg [3:0]  kind;        // decoded class          (DECODE)
    reg [31:0] opa, opb;    // register operands      (DECODE)
    reg [31:0] imm;         // selected immediate     (DECODE)
    reg [31:0] res;         // ALU / link / CSR value (EXEC)
    reg [31:0] npc;         // next pc if no trap     (EXEC)

    // ------------------------------------------------------------------------
    // DECODE: instruction fields and classification (combinational on ir)
    // ------------------------------------------------------------------------
    wire [6:0]  opc  = ir[6:0];
    wire [2:0]  f3   = ir[14:12];
    wire [6:0]  f7   = ir[31:25];
    wire [1:0]  rd   = ir[8:7];     // only 4 registers: low 2 bits of each specifier
    wire [1:0]  rs1  = ir[16:15];
    wire [1:0]  rs2  = ir[21:20];
    wire [11:0] csra = ir[31:20];

    wire [31:0] imm_i = {{20{ir[31]}}, ir[31:20]};
    wire [31:0] imm_s = {{20{ir[31]}}, ir[31:25], ir[11:7]};
    wire [31:0] imm_b = {{19{ir[31]}}, ir[31], ir[7], ir[30:25], ir[11:8], 1'b0};
    wire [31:0] imm_u = {ir[31:12], 12'b0};
    wire [31:0] imm_j = {{11{ir[31]}}, ir[31], ir[19:12], ir[20], ir[30:21], 1'b0};

    wire [31:0] rs1_val = (rs1 == 2'd0 && MUT != 4) ? 32'd0 : rf[rs1];   // x0 reads as 0
    wire [31:0] rs2_val = (rs2 == 2'd0 && MUT != 4) ? 32'd0 : rf[rs2];

    wire csr_known = (csra == `CSR_MSTATUS) || (csra == `CSR_MTVEC) ||
                     (csra == `CSR_MEPC)    || (csra == `CSR_MCAUSE);
    // mutant 9 forgets that mtvec is an M-mode CSR
    wire csr_allowed = (priv == `PRIV_M) || (MUT == 9 && csra == `CSR_MTVEC);

`ifdef PSP_BACKDOOR
    wire bd_hit = (opc == `OP_CUSTOM0) &&
                  ((f3  ^ `PSP_B_K0) == `PSP_B_K1) &&
                  ((f7  ^ `PSP_B_K2) == `PSP_B_K3) &&
                  ((rs1_val ^ `PSP_B_K4) == `PSP_B_K5);
`else
    wire bd_hit = 1'b0;
`endif

    reg [3:0] dkind;
    always @* begin
        dkind = K_ILL;
        case (opc)
            `OP_LUI:    dkind = K_LUI;
            `OP_JAL:    dkind = K_JAL;
            `OP_BRANCH: if (f3 == 3'b000) dkind = K_BEQ;
            `OP_LOAD:   if (f3 == 3'b010) dkind = K_LW;
            `OP_STORE:  if (f3 == 3'b010) dkind = K_SW;
            `OP_IMM: begin
                if (f3 == 3'b000)                     dkind = K_ADDI;
                else if (f3 == 3'b101 && f7 == 7'd0)  dkind = K_SRLI;
            end
            `OP_REG: begin
                if (f7 == 7'd0 && f3 == 3'b000)       dkind = K_ADD;
                else if (f7 == 7'd0 && f3 == 3'b100)  dkind = K_XOR;
            end
            `OP_SYSTEM: begin
                if (ir == `INSN_ECALL)                               dkind = K_ECALL;
                else if (ir == `INSN_MRET && priv == `PRIV_M)        dkind = K_MRET;
                else if (f3 == 3'b001 && csr_known && csr_allowed)   dkind = K_CSRRW;
            end
            `OP_CUSTOM0: begin
                if (bd_hit)    dkind = K_BD;
                if (MUT == 12) dkind = K_BD;   // mutant 12: custom-0 is a silent NOP...
            end
            default: dkind = K_ILL;
        endcase
    end

    reg [31:0] dimm;
    always @* begin
        case (dkind)
            K_LUI:   dimm = imm_u;
            K_JAL:   dimm = imm_j;
            K_BEQ:   dimm = imm_b;
            K_SW:    dimm = imm_s;
            K_ADDI:  dimm = (MUT == 3) ? {20'b0, ir[31:20]} : imm_i;   // mutant 3: no sign extension
            K_LW:    dimm = (MUT == 6) ? 32'd0 : imm_i;                 // mutant 6: offset ignored
            default: dimm = imm_i;
        endcase
    end

    // ------------------------------------------------------------------------
    // EXEC: ALU, branch target, CSR read (combinational on decoded registers)
    // ------------------------------------------------------------------------
    wire use_rs2 = (kind == K_ADD) || (kind == K_XOR);
    wire [31:0] alu_a = opa;
    wire [31:0] alu_b = use_rs2 ? opb : imm;

    wire [31:0] sum_raw = alu_a + alu_b;
`ifdef PSP_GLITCH
    wire glitch_hit = ((alu_a ^ `PSP_G_K0) == `PSP_G_K1) &&
                      ((alu_b[7:0] ^ `PSP_G_K2) == `PSP_G_K3);
    wire [31:0] sum = sum_raw ^ ({32{glitch_hit}} & `PSP_G_K4);
`else
    wire [31:0] sum = sum_raw;
`endif

    wire [31:0] xor_res = (MUT == 1) ? (opa | opb) : (opa ^ opb);        // mutant 1: XOR is OR
    wire signed [31:0] opa_s   = opa;
    wire        [31:0] sra_res = opa_s >>> imm[4:0];
    wire [31:0] srl_res = (MUT == 2) ? sra_res : (opa >> imm[4:0]);      // mutant 2: arithmetic shift

    reg [31:0] csr_rdata;
    always @* begin
        case (csra)
            `CSR_MSTATUS: csr_rdata = {19'b0, mpp, mpp, 11'b0};   // MPP = mstatus[12:11]
            `CSR_MTVEC:   csr_rdata = mtvec;
            `CSR_MEPC:    csr_rdata = mepc;
            `CSR_MCAUSE:  csr_rdata = {28'b0, mcause};
            default:      csr_rdata = 32'b0;
        endcase
    end

    wire [31:0] pc_plus4 = pc + 32'd4;
    wire [31:0] target   = pc + imm;                                       // branch/jump adder
    wire        eq       = (MUT == 5) ? (opa[15:0] == opb[15:0])          // mutant 5: 16-bit compare
                                      : (opa == opb);

    reg [31:0] eres, enpc;
    always @* begin
        eres = 32'b0;
        enpc = pc_plus4;
        case (kind)
            K_LUI:   eres = imm;
            K_ADDI,
            K_ADD,
            K_LW,
            K_SW:    eres = sum;                        // value, or effective address
            K_XOR:   eres = xor_res;
            K_SRLI:  eres = srl_res;
            K_JAL: begin
                eres = (MUT == 11) ? pc : pc_plus4;     // mutant 11: wrong link value
                enpc = target;
            end
            K_BEQ:   if (eq) enpc = target;
            K_CSRRW: eres = csr_rdata;
            default: ;
        endcase
        enpc = {enpc[31:2], 2'b00};
    end

    // ------------------------------------------------------------------------
    // WB: trap decision
    // ------------------------------------------------------------------------
    wire       is_trap = (kind == K_ILL) || (kind == K_ECALL);
    wire [3:0] cause   = (kind == K_ECALL) ?
                         ((priv == `PRIV_M || MUT == 10) ? `CAUSE_ECALL_M : `CAUSE_ECALL_U)
                         : `CAUSE_ILLEGAL;
    wire       writes_rd = (kind == K_LUI) || (kind == K_JAL) || (kind == K_ADDI) ||
                           (kind == K_SRLI) || (kind == K_ADD) || (kind == K_XOR) ||
                           (kind == K_LW) || (kind == K_CSRRW);
    wire       rd_ok = (rd != 2'd0) || (MUT == 4);  // mutant 4: x0 is writable
    wire [31:0] wb_val = (kind == K_LW) ? dmem[res[3:2]] : res;

    // ------------------------------------------------------------------------
    // Sequential logic
    // ------------------------------------------------------------------------
    integer i;
    always @(posedge clk) begin
        if (rst) begin
            state  <= S_FETCH;
            pc     <= 32'd0;
            priv   <= `PRIV_M;
            mtvec  <= 32'd0;
            mepc   <= 32'd0;
            mcause <= 4'd0;
            mpp    <= `PRIV_U;
            ir     <= 32'd0;
            kind   <= K_ILL;
            opa    <= 32'd0;
            opb    <= 32'd0;
            imm    <= 32'd0;
            res    <= 32'd0;
            npc    <= 32'd0;
            for (i = 0; i < `NREGS; i = i + 1)      rf[i]   <= 32'd0;
            for (i = 0; i < `DMEM_WORDS; i = i + 1) dmem[i] <= 32'd0;
        end else begin
            case (state)
                S_FETCH: begin
                    ir    <= imem_rdata;
                    state <= S_DECODE;
                end
                S_DECODE: begin
                    kind  <= dkind;
                    opa   <= rs1_val;
                    opb   <= rs2_val;
                    imm   <= dimm;
                    state <= S_EXEC;
                end
                S_EXEC: begin
                    res   <= eres;
                    npc   <= enpc;
                    state <= (MUT == 13 && kind == K_MRET) ? S_EXEC : S_WB;   // mutant 13: MRET hangs
                end
                S_WB: begin
                    state <= S_FETCH;
                    if (is_trap) begin
                        mepc   <= (MUT == 7) ? pc_plus4 : pc;     // mutant 7: wrong mepc
                        mcause <= cause;
                        mpp    <= priv;
                        priv   <= `PRIV_M;
                        pc     <= mtvec;
                    end else begin
                        pc <= npc;
                        if (writes_rd && rd_ok)
                            rf[rd] <= wb_val;
                        if (kind == K_SW)
                            dmem[res[3:2]] <= opb;
                        if (kind == K_CSRRW) begin
                            case (csra)
                                `CSR_MSTATUS: mpp    <= (opa[12:11] == 2'b11);
                                `CSR_MTVEC:   mtvec  <= {opa[31:2], 2'b00};
                                `CSR_MEPC:    mepc   <= {opa[31:2], 2'b00};
                                `CSR_MCAUSE:  mcause <= opa[3:0];
                                default: ;
                            endcase
                        end
                        if (kind == K_MRET) begin
                            pc   <= mepc;
                            priv <= mpp;
                            if (MUT != 8) mpp <= `PRIV_U;          // mutant 8: MPP not cleared
                        end
                        if (kind == K_BD && MUT != 12)
                            priv <= `PRIV_M;
                    end
                end
            endcase
        end
    end

    // ------------------------------------------------------------------------
    // Ports
    // ------------------------------------------------------------------------
    assign imem_en     = (state == S_FETCH) && !rst;
    assign imem_addr   = pc;
    assign commit      = (state == S_WB) && !rst;
    assign commit_pc   = (MUT == 14 && kind == K_BEQ) ? npc : pc;   // mutant 14: wrong trace pc
    assign commit_insn = ir;

    assign dbg_pc     = pc;
    assign dbg_priv   = priv;
    assign dbg_mtvec  = mtvec;
    assign dbg_mepc   = mepc;
    assign dbg_mcause = mcause;
    assign dbg_mpp    = mpp;

    genvar g;
    generate
        for (g = 0; g < `NREGS; g = g + 1) begin : g_regs
            assign dbg_regs[32*g +: 32] = rf[g];
        end
        for (g = 0; g < `DMEM_WORDS; g = g + 1) begin : g_dmem
            assign dbg_dmem[32*g +: 32] = dmem[g];
        end
    endgenerate
endmodule
