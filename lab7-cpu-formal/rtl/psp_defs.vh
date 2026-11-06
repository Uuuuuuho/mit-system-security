// psp_defs.vh -- constants shared by the psp-mini implementation, the spec
// core, the miter and the simulation testbench.
//
// psp-mini is a deliberately tiny RV32I-subset machine:
//   * 4 general-purpose registers x0..x3 (x0 is hard-wired to zero).  Only bits
//     [1:0] of every 5-bit register specifier are used, so "x5" aliases x1 and
//     "x4" aliases x0.  (Few registers keep the BMC problems small.)
//   * 4 words of data memory.  Only address bits [3:2] select a word; bits
//     [1:0] and [31:4] are ignored (the 16-byte memory repeats everywhere).
//   * Harvard: instructions come from a separate read-only instruction port.
//   * Two privilege levels, U (0) and M (1).  Reset state: pc=0, priv=M,
//     every register/CSR/memory word = 0.
//   * CSRs: mstatus (only MPP is implemented), mtvec, mepc, mcause (4 bits).
//   * Jump/branch targets and mtvec/mepc values have bits [1:0] forced to 00
//     (there is no instruction-address-misaligned exception).
`ifndef PSP_DEFS_VH
`define PSP_DEFS_VH

// ---- major opcodes (insn[6:0]) ---------------------------------------------
`define OP_LUI     7'b0110111
`define OP_JAL     7'b1101111
`define OP_BRANCH  7'b1100011
`define OP_LOAD    7'b0000011
`define OP_STORE   7'b0100011
`define OP_IMM     7'b0010011
`define OP_REG     7'b0110011
`define OP_SYSTEM  7'b1110011
`define OP_CUSTOM0 7'b0001011   // reserved for vendor extensions: must trap

// ---- whole-word encodings ----------------------------------------------------
`define INSN_ECALL 32'h00000073
`define INSN_MRET  32'h30200073
`define INSN_HALT  32'h00000063 // beq x0,x0,0 -- the testbench stops here

// ---- CSR addresses (all are M-mode read/write CSRs) ---------------------------
`define CSR_MSTATUS 12'h300
`define CSR_MTVEC   12'h305
`define CSR_MEPC    12'h341
`define CSR_MCAUSE  12'h342

// ---- trap causes (mcause is 4 bits wide in psp-mini) --------------------------
`define CAUSE_ILLEGAL 4'd2
`define CAUSE_ECALL_U 4'd8
`define CAUSE_ECALL_M 4'd11

// ---- privilege levels ----------------------------------------------------------
`define PRIV_U 1'b0
`define PRIV_M 1'b1

// ---- sizes -------------------------------------------------------------------------
`define NREGS      4
`define DMEM_WORDS 4

`endif
