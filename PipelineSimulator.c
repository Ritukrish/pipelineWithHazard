#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#define NUM_REGS 16
#define LINE_LEN 128
#define MAX_INST 512
#define REG_UNUSED (-1)
#define MEM_SIZE_WORDS 1024   // memory size (words)
#define WORD_SIZE_BYTES 4
#define MEMORY_SIZE 4096
#define REGISTER_MEMORY_BASE 1000   // starting address for registers in memory

int memory[MEMORY_SIZE]; // global memory
int R[NUM_REGS];  // Register file


/**
 * @brief Validate register index
 * @param r Register index (0..NUM_REGS-1, or -1 for unused)
 * @return true if valid, false otherwise
 */
static inline bool reg_valid(int r) {
    return r == REG_UNUSED || (r >= 0 && r < NUM_REGS);
}

// ---------- ISA ----------
typedef enum { OP_NOOP, OP_MOV, OP_ADD, OP_SUB, OP_MUL, OP_LOAD, OP_STORE } OpCode;

typedef struct {
    OpCode op;
    int rd, rs1, rs2;   // -1 if not used
    int imm;            // used for MOV and offset for loads/stores
    int valid;          // 1 if this instruction slot contains a real inst
    char text[LINE_LEN];
} Instruction;

// For tracing where an operand came from
typedef enum { SRC_NONE, SRC_REG, SRC_MEM, SRC_WB } FwdSrc;

typedef struct {
    Instruction inst;
    int alu_result;     // result computed in EX stage (or MOV imm) or address for loads/stores
    int val_rs1;        // resolved operand 1 (after forwarding)
    int val_rs2;        // resolved operand 2 (after forwarding)
    FwdSrc src_rs1;     // SRC_REG/SRC_MEM/SRC_WB/SRC_NONE
    FwdSrc src_rs2;     // SRC_REG/SRC_MEM/SRC_WB/SRC_NONE
} StageLatch;

// ---------- CPU container (no globals) ----------
typedef struct {
    int R[NUM_REGS];               // Register file
    Instruction program[MAX_INST]; // Instruction memory
    int inst_count;                // Number of instructions loaded
    int PC;                        // Program Counter

    // Simple memory (word-addressable). Addresses are byte addresses; we index by word (address/4).
    int memory[MEM_SIZE_WORDS];

    // Pipeline latches
    StageLatch pipeline_IF_ID, pipeline_ID_EX, pipeline_EX_MEM, pipeline_MEM_WB;
} CPU;

// ---------- Helpers ----------
const char* opcode_name(OpCode op) {
    switch(op) {
        case OP_MOV: return "MOV";
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";
        case OP_LOAD: return "LOAD";
        case OP_STORE: return "STORE";
        case OP_NOOP: return "NOP";
        default: return "UNK";
    }
}

/**
 * @brief Construct a NOP instruction
 * @return Instruction representing a no-op
 */
Instruction make_nop() {
    Instruction i;
    i.op = OP_NOOP;
    i.rd = i.rs1 = i.rs2 = REG_UNUSED;
    i.imm = 0;
    i.valid = 0;
    strcpy(i.text, "NOP");
    return i;
}
Instruction make_invalid_instruction(const char *reason) {
    Instruction ins = make_nop(); // create a NOP as base
    ins.valid = 0;
    snprintf(ins.text, LINE_LEN, "ERROR: %s", reason);
    return ins;
}

StageLatch make_nop_latch() {
    StageLatch s;
    s.inst = make_nop();
    s.alu_result = 0;
    s.val_rs1 = s.val_rs2 = 0;
    s.src_rs1 = s.src_rs2 = SRC_NONE;
    return s;
}

void init_pipeline(CPU* cpu) {
    cpu->pipeline_IF_ID = make_nop_latch();
    cpu->pipeline_ID_EX = make_nop_latch();
    cpu->pipeline_EX_MEM = make_nop_latch();
    cpu->pipeline_MEM_WB = make_nop_latch();
}

/**
 * @brief Parse a line of assembly into an Instruction
 * @param line Input string (one assembly instruction)
 * @return Parsed Instruction (valid=0 if error, with error text)
 */
// ---------- Modular Parsing ----------

/**
 * @brief Parse MOV instruction
 */
Instruction parse_mov(char *rd_str, char *imm_str) {
    Instruction ins = make_nop();

    if (!rd_str || sscanf(rd_str, "R%d", &ins.rd) != 1 || ins.rd < 0 || ins.rd >= NUM_REGS)
        return make_invalid_instruction("Invalid destination register in MOV");

    if (!imm_str || sscanf(imm_str, "%d", &ins.imm) != 1)
        return make_invalid_instruction("Invalid immediate in MOV");

    ins.op = OP_MOV;
    ins.rs1 = ins.rs2 = REG_UNUSED;
    ins.valid = 1;
    return ins;
}

/**
 * @brief Parse R-type instruction (ADD, SUB, MUL)
 */
Instruction parse_rtype(OpCode op, char *rd_str, char *rs1_str, char *rs2_str) {
    Instruction ins = make_nop();

    if (!rd_str  || sscanf(rd_str, "R%d", &ins.rd)  != 1 || ins.rd  < 0 || ins.rd  >= NUM_REGS)
        return make_invalid_instruction("Invalid destination register");

    if (!rs1_str || sscanf(rs1_str, "R%d", &ins.rs1) != 1 || ins.rs1 < 0 || ins.rs1 >= NUM_REGS)
        return make_invalid_instruction("Invalid source register 1");

    if (!rs2_str || sscanf(rs2_str, "R%d", &ins.rs2) != 1 || ins.rs2 < 0 || ins.rs2 >= NUM_REGS)
        return make_invalid_instruction("Invalid source register 2");

    ins.op = op;
    ins.imm = 0;
    ins.valid = 1;
    return ins;
}

/**
 * @brief Parse offset(Rx) pattern into offset and base register.
 * Example: "8(R0)" => offset=8, base=0
 * Returns 1 on success, 0 on parse failure.
 */
int parse_offset_reg(const char *s, int *out_offset, int *out_reg) {
    if (!s) return 0;
    // allow optional + or - on offset
    int off=0, r=-1;
    if (sscanf(s, "%d(R%d)", &off, &r) == 2) {
        *out_offset = off;
        *out_reg = r;
        return 1;
    }
    // try if spaces or differently formatted - fail otherwise
    return 0;
}

/**
 * @brief Parse LOAD instruction: load Rdst, OFFSET(Rbase)
 */
Instruction parse_load(char *rd_str, char *addr_str) {
    Instruction ins = make_nop();
    if (!rd_str || sscanf(rd_str, "R%d", &ins.rd) != 1 || ins.rd < 0 || ins.rd >= NUM_REGS)
        return make_invalid_instruction("Invalid destination register in LOAD");

    int base = -1, off = 0;
    if (!addr_str || !parse_offset_reg(addr_str, &off, &base) || base < 0 || base >= NUM_REGS)
        return make_invalid_instruction("Invalid address in LOAD");

    ins.op = OP_LOAD;
    ins.rs1 = base;    // base register
    ins.rs2 = REG_UNUSED;
    ins.imm = off;     // byte offset
    ins.valid = 1;
    return ins;
}

/**
 * @brief Parse STORE instruction: store Rsrc, OFFSET(Rbase)
 */
Instruction parse_store(char *rs_str, char *addr_str) {
    Instruction ins = make_nop();
    if (!rs_str || sscanf(rs_str, "R%d", &ins.rs1) != 1 || ins.rs1 < 0 || ins.rs1 >= NUM_REGS)
        return make_invalid_instruction("Invalid source register in STORE");

    int base = -1, off = 0;
    if (!addr_str || !parse_offset_reg(addr_str, &off, &base) || base < 0 || base >= NUM_REGS)
        return make_invalid_instruction("Invalid address in STORE");

    ins.op = OP_STORE;
    ins.rd = REG_UNUSED;
    ins.rs2 = base;  // base register in rs2
    ins.imm = off;
    ins.valid = 1;
    return ins;
}

/**
 * @brief Dispatch parsing based on opcode
 */
Instruction parse_line(char *line) {
    char temp_line[LINE_LEN];
    strcpy(temp_line, line);

    char *opcode_str = strtok(temp_line, " ,\t\n");
    if (!opcode_str)
        return make_invalid_instruction("Missing opcode");

    Instruction ins = make_nop();

    if (strcasecmp(opcode_str, "mov") == 0) {
        // MOV R1, 10
        char *rd_str = strtok(NULL, " ,\t\n");
        char *imm_str = strtok(NULL, " ,\t\n");
        ins = parse_mov(rd_str, imm_str);
    }
    else if (strcasecmp(opcode_str, "add") == 0 ||
             strcasecmp(opcode_str, "sub") == 0 ||
             strcasecmp(opcode_str, "mul") == 0) {

        OpCode op = (strcasecmp(opcode_str, "add") == 0) ? OP_ADD :
                    (strcasecmp(opcode_str, "sub") == 0) ? OP_SUB : OP_MUL;

        // ADD R1, R2, R3
        char *rd_str  = strtok(NULL, " ,\t\n");
        char *rs1_str = strtok(NULL, " ,\t\n");
        char *rs2_str = strtok(NULL, " ,\t\n");
        ins = parse_rtype(op, rd_str, rs1_str, rs2_str);
    }
    else if (strcasecmp(opcode_str, "load") == 0) {
        // LOAD R5, 8(R0)
        char *rd_str = strtok(NULL, " ,\t\n");
        char *addr_str = strtok(NULL, " ,\t\n");
        ins = parse_load(rd_str, addr_str);
    }
    else if (strcasecmp(opcode_str, "store") == 0) {
        // STORE R3, 8(R0)
        char *rs_str = strtok(NULL, " ,\t\n");
        char *addr_str = strtok(NULL, " ,\t\n");
        ins = parse_store(rs_str, addr_str);
    }
    else {
        return make_invalid_instruction("Unknown opcode");
    }

    // trim trailing newline from original line
    char tline[LINE_LEN];
    strncpy(tline, line, LINE_LEN-1); tline[LINE_LEN-1] = '\0';
    // remove trailing newline
    size_t L = strlen(tline);
    while (L>0 && (tline[L-1]=='\n' || tline[L-1]=='\r')) { tline[L-1]=0; --L; }
    strncpy(ins.text, tline, LINE_LEN-1);
    ins.text[LINE_LEN-1]=0;

    return ins;
}

// ---------- Modular ALU ----------
/**
 * @brief Perform ALU operation
 * @param op   The operation code
 * @param a    Operand 1
 * @param b    Operand 2
 * @param imm  Immediate value (used for MOV and load/store offset)
 * @return Computed result
 */
int alu_execute(OpCode op, int a, int b, int imm) {
    switch (op) {
        case OP_MOV: return imm;
        case OP_ADD: return a + b;
        case OP_SUB: return a - b;
        case OP_MUL: return a * b;
        case OP_LOAD:
        case OP_STORE:
            // For loads/stores, EX stage computes effective address (byte address).
            return a + imm;
        case OP_NOOP: return 0;
        default: return 0;
    }
}

/**
 * @brief Load program into CPU instruction memory
 * @param cpu CPU state pointer
 * @param filename File containing assembly instructions
 * @return 0 on success, -1 if file could not be opened
 */
int program_load(CPU* cpu, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;
    char line[LINE_LEN];
    cpu->inst_count = 0;
    int lineno = 0;
    while (fgets(line, sizeof(line), f) && cpu->inst_count < MAX_INST) {
        lineno++;
        Instruction ins = parse_line(line);
        if (ins.valid) {
            cpu->program[cpu->inst_count++] = ins;
        } else {
            fprintf(stderr, "Parse error at line %d: %s -- '%s'\n", lineno, ins.text, line);
        }
    }
    fclose(f);
    return 0;
}

bool pipeline_is_empty(const CPU* cpu) {
    return !cpu->pipeline_IF_ID.inst.valid && !cpu->pipeline_ID_EX.inst.valid &&
           !cpu->pipeline_EX_MEM.inst.valid && !cpu->pipeline_MEM_WB.inst.valid;
}
/**
 * @brief Instruction Fetch (IF) stage
 * @param cpu CPU pointer
 * @param fetched_inst Output pointer to hold fetched instruction
 */
// ---------- IF ----------
void fetch_stage(CPU* cpu, Instruction* fetched_inst) {
    assert(cpu->PC >= 0 && cpu->PC <= cpu->inst_count);  // ✅ PC must be in range

    if (cpu->PC < cpu->inst_count) {
        *fetched_inst = cpu->program[cpu->PC];
    } else {
        *fetched_inst = make_nop();
    }
}

// ---------- Forwarding helper ----------
typedef struct {
    int value;
    FwdSrc src;
} Resolved;

/**
 * @brief Resolve an operand value using forwarding rules.
 * Note: LOAD values are only available after MEM stage (i.e. from MEM/WB),
 * so we do NOT forward loaded data from EX/MEM (EX/MEM.alu_result for a LOAD is an address).
 */
Resolved resolve_operand(const CPU* cpu, int reg) {
    Resolved r; r.value = 0; r.src = SRC_NONE;
    if (reg == -1) return r;

    // If EX/MEM has an instruction that wrote this reg and it is not a LOAD
    if (cpu->pipeline_EX_MEM.inst.valid && cpu->pipeline_EX_MEM.inst.rd == reg && cpu->pipeline_EX_MEM.inst.rd != REG_UNUSED) {
        // Only forward from EX/MEM when the producing instruction is NOT a LOAD.
        if (cpu->pipeline_EX_MEM.inst.op != OP_LOAD) {
            r.value = cpu->pipeline_EX_MEM.alu_result;
            r.src = SRC_MEM;
            return r;
        }
    }
    // Then check MEM/WB (final result available for writes and loads)
    if (cpu->pipeline_MEM_WB.inst.valid && cpu->pipeline_MEM_WB.inst.rd == reg && cpu->pipeline_MEM_WB.inst.rd != REG_UNUSED) {
        r.value = cpu->pipeline_MEM_WB.alu_result;
        r.src = SRC_WB;
        return r;
    }
    // Otherwise read register file
    r.value = cpu->R[reg];
    r.src = SRC_REG;
    return r;
}

// ---------- ID (pure) ----------
typedef struct {
    StageLatch next;
    bool stall;
    const char* stall_reason;
} DecodeResult;
/**
 * @brief Instruction Decode (ID) stage
 * @param cpu CPU state
 * @param pipeline_IF_ID Current IF/ID latch
 * @param pipeline_ID_EX Current ID/EX latch
 * @return DecodeResult (next ID/EX latch + stall info)
 */
DecodeResult decode_stage(const CPU* cpu, StageLatch pipeline_IF_ID, StageLatch pipeline_ID_EX) {
    DecodeResult res;
    res.next = pipeline_IF_ID; // pass-through for this simple ISA
    res.stall = false;
    res.stall_reason = NULL;

    // Load-use hazard detection:
  // STORE → LOAD hazard detection
if (pipeline_ID_EX.inst.valid && pipeline_ID_EX.inst.op == OP_STORE &&
    pipeline_IF_ID.inst.valid && pipeline_IF_ID.inst.op == OP_LOAD) {
    
    int store_base = pipeline_ID_EX.inst.rs2;   // STORE base register
    int load_base = pipeline_IF_ID.inst.rs1;    // LOAD base register

    if (store_base == load_base && pipeline_ID_EX.inst.imm == pipeline_IF_ID.inst.imm) {
        res.stall = true;
        res.stall_reason = "STORE→LOAD hazard (same address)";
    }
}


    return res;
}

// ---------- EX (pure) ----------
typedef struct {
    StageLatch next;     // the latch for EX/MEM
    bool branch_taken;   // true if branch was taken (unused here)
    int target_pc;       // new PC if branch
    bool valid;          // whether this result is valid
} ExecResult;

/**
 * @brief Execute one instruction in EX stage
 * @param cpu CPU state
 * @param pipeline_ID_EX Current ID/EX latch
 * @return ExecResult (EX/MEM latch + ALU result + branch info)
 */
ExecResult execute_stage(const CPU* cpu, StageLatch pipeline_ID_EX) {
    ExecResult r;
    r.next = pipeline_ID_EX;
    r.branch_taken = false;
    r.target_pc = -1;
    r.valid = pipeline_ID_EX.inst.valid;

    if (!pipeline_ID_EX.inst.valid || pipeline_ID_EX.inst.op == OP_NOOP) {
        r.next.val_rs1 = r.next.val_rs2 = 0;
        r.next.src_rs1 = r.next.src_rs2 = SRC_NONE;
        r.next.alu_result = 0;
        return r;
    }

    // Defensive: register validity
    assert(reg_valid(pipeline_ID_EX.inst.rd));
    assert(reg_valid(pipeline_ID_EX.inst.rs1));
    assert(reg_valid(pipeline_ID_EX.inst.rs2));

    // Resolve operands with forwarding
    Resolved rs1 = resolve_operand(cpu, pipeline_ID_EX.inst.rs1);
    Resolved rs2 = resolve_operand(cpu, pipeline_ID_EX.inst.rs2);

    r.next.val_rs1 = rs1.value;
    r.next.val_rs2 = rs2.value;
    r.next.src_rs1 = rs1.src;
    r.next.src_rs2 = rs2.src;

    // Use modular ALU: for LOAD/STORE, compute effective byte address in EX stage.
    r.next.alu_result = alu_execute(pipeline_ID_EX.inst.op, rs1.value, rs2.value, pipeline_ID_EX.inst.imm);

    // For STORE, also ensure r.next.val_rs1 carries the store data (rs1 was used as data register)
    // In our parse, for STORE we used rs1 as source-data and rs2 as base; so val_rs1 already has data.
    return r;
}

// ---------- MEM ----------
typedef struct {
    StageLatch next;
} MemResult;

/**
 * @brief Memory stage (pass-through for this ISA)
 * @param pipeline_EX_MEM Current EX/MEM latch
 * @return MemResult (MEM/WB latch)
 */
MemResult memory_stage(CPU* cpu, StageLatch pipeline_EX_MEM) {
    MemResult r;
    r.next = pipeline_EX_MEM;  // default pass-through

    if (!pipeline_EX_MEM.inst.valid || pipeline_EX_MEM.inst.op == OP_NOOP) {
        return r;
    }

if (pipeline_EX_MEM.inst.op == OP_STORE) {
    int base_register = pipeline_EX_MEM.inst.rs2;  // R0
    int offset = pipeline_EX_MEM.inst.imm;         // 8
    int source_register = pipeline_EX_MEM.inst.rs1; // R3 (value to store)

    // Step 1: find where R0 itself lives in memory
    int address_of_R0 = get_register_address(base_register);

    // Step 2: calculate effective address
    int effective_address = address_of_R0 + offset;

    // Step 3: store R3's value into memory at that address
    memory[effective_address] = R[source_register];

    printf("[MEM] STORE: R%d(%d) -> Memory[%d] (base addr=%d + offset=%d)\n",
           source_register,
           R[source_register],
           effective_address,
           address_of_R0,
           offset);
}



else if (pipeline_EX_MEM.inst.op == OP_LOAD) {
    int base_register = pipeline_EX_MEM.inst.rs1;  // R0
    int offset = pipeline_EX_MEM.inst.imm;         // 8
    int dest_register = pipeline_EX_MEM.inst.rd;   // R5

    // Step 1: find where R0 itself lives in memory
    int address_of_R0 = get_register_address(base_register);

    // Step 2: calculate effective address
    int effective_address = address_of_R0 + offset;

    // Step 3: load value from memory
    R[dest_register] = memory[effective_address];

    printf("[MEM] LOAD: R%d <- Memory[%d] (value=%d, base addr=%d + offset=%d)\n",
           dest_register,
           effective_address,
           memory[effective_address],
           address_of_R0,
           offset);
}


 else {
        // For ALU ops / MOV, pass through the ALU result (already in alu_result)
        r.next.alu_result = pipeline_EX_MEM.alu_result;
    }

    return r;
}

/**
 * @brief Write-back (WB) stage
 * @param cpu CPU state pointer
 */
void wb_stage(CPU* cpu) {
    const Instruction* w = &cpu->pipeline_MEM_WB.inst;
    if (w->valid && w->op != OP_NOOP && w->rd != REG_UNUSED) {
        assert(reg_valid(w->rd));
        cpu->R[w->rd] = cpu->pipeline_MEM_WB.alu_result;
    }
}

// ---------- Pipeline advancement ----------
/**
 * @brief Advance all pipeline latches by one cycle
 * @param cpu CPU state
 * @param ex_res Result of EX stage
 * @param mem_res Result of MEM stage
 * @param fetched_inst Instruction fetched in IF
 * @param dec_res Decode stage result (including stall info)
 */
/**
 * @brief Advance all pipeline latches by one cycle
 * @param cpu CPU state
 * @param ex_res Result of EX stage
 * @param mem_res Result of MEM stage
 * @param fetched_inst Instruction fetched in IF
 * @param dec_res Decode stage result (including stall info)
 */
void advance_pipeline(CPU* cpu,
                      ExecResult ex_res,
                      MemResult mem_res,
                      Instruction fetched_inst,
                      DecodeResult dec_res) {
    // Defensive assertion: PC must always be within valid range
    assert(cpu->PC >= 0 && cpu->PC <= cpu->inst_count);

    // Commit WB (already done inside wb_stage)
    // MEM → WB
    cpu->pipeline_MEM_WB = mem_res.next;

    // EX → MEM
    cpu->pipeline_EX_MEM = ex_res.next;

    // ID → EX
    if (dec_res.stall)
        cpu->pipeline_ID_EX = make_nop_latch();
    else
        cpu->pipeline_ID_EX = cpu->pipeline_IF_ID;

    // IF → ID
    if (!dec_res.stall) {
        cpu->pipeline_IF_ID.inst = fetched_inst;

        // Centralized PC increment
        if (cpu->PC < cpu->inst_count) {
            cpu->PC++;
        }
    } else {
        // stalled: keep the same IF/ID (we do not advance PC; fetched_inst should be discarded)
    }
}

// ---------- Pretty printing ----------
static const char* src_name(FwdSrc s) {
    switch (s) {
        case SRC_NONE: return "-";
        case SRC_REG:  return "RF";
        case SRC_MEM:  return "MEM";
        case SRC_WB:   return "WB";
        default:       return "?";
    }
}

void print_stage_inst(const char *name, const StageLatch *s) {
    if (!s->inst.valid || s->inst.op == OP_NOOP) {
        printf("%-6s: %-20s ", name, "NOP");
        return;
    }
    printf("%-6s: %-20s", name, s->inst.text);
}
/**
 * @brief Print pipeline and register state for the given cycle
 * @param cpu CPU state
 * @param cycle Current cycle number
 * @param stalled Whether the decode stage stalled
 * @param stall_reason String explaining stall reason (optional)
 */
void print_cycle_state(const CPU* cpu, int cycle, bool stalled, const char* stall_reason) {
    printf("\n================ Cycle %d ================ Pc : %d\n", cycle, cpu->PC);

    if (cpu->PC < cpu->inst_count)
        printf("IF    : Fetching '%s'%s\n", cpu->program[cpu->PC].text, stalled ? " (stall->refetch)" : "");
    else
        printf("IF    : Done\n");

    if (stalled) {
        printf("ID    : %-20s (Stalled%s%s)\n",
               cpu->pipeline_IF_ID.inst.valid ? cpu->pipeline_IF_ID.inst.text : "NOP",
               stall_reason ? " — " : "",
               stall_reason ? stall_reason : "");
    } else {
        print_stage_inst("ID", &cpu->pipeline_IF_ID); printf("\n");
    }

    if (!cpu->pipeline_ID_EX.inst.valid || cpu->pipeline_ID_EX.inst.op == OP_NOOP) {
        printf("EX    : NOP\n");
    } else if (cpu->pipeline_ID_EX.inst.op == OP_MOV) {
        printf("EX    : %-20s (imm=%d and result=%d)\n",
               cpu->pipeline_ID_EX.inst.text, cpu->pipeline_ID_EX.inst.imm, cpu->pipeline_ID_EX.alu_result);
    } else if (cpu->pipeline_ID_EX.inst.op == OP_LOAD || cpu->pipeline_ID_EX.inst.op == OP_STORE) {
        // show address computation and forwarded operand info
        if (cpu->pipeline_ID_EX.inst.op == OP_LOAD) {
            printf("EX    : %-20s (base R%d=%d[%s], offset=%d; addr=%d)\n",
                   cpu->pipeline_ID_EX.inst.text,
                   cpu->pipeline_ID_EX.inst.rs1, cpu->pipeline_ID_EX.val_rs1, src_name(cpu->pipeline_ID_EX.src_rs1),
                   cpu->pipeline_ID_EX.inst.imm,
                   cpu->pipeline_ID_EX.alu_result);
        } else {
            // STORE: val_rs1 is data, rs2 is base
            printf("EX    : %-20s (data R%d=%d[%s], base R%d=%d[%s], offset=%d; addr=%d)\n",
                   cpu->pipeline_ID_EX.inst.text,
                   cpu->pipeline_ID_EX.inst.rs1, cpu->pipeline_ID_EX.val_rs1, src_name(cpu->pipeline_ID_EX.src_rs1),
                   cpu->pipeline_ID_EX.inst.rs2, cpu->pipeline_ID_EX.val_rs2, src_name(cpu->pipeline_ID_EX.src_rs2),
                   cpu->pipeline_ID_EX.inst.imm,
                   cpu->pipeline_ID_EX.alu_result);
        }
    } else {
        printf("EX    : %-20s (R%d=%d[%s], R%d=%d[%s]; result=%d)\n",
               cpu->pipeline_ID_EX.inst.text,
               cpu->pipeline_ID_EX.inst.rs1, cpu->pipeline_ID_EX.val_rs1, src_name(cpu->pipeline_ID_EX.src_rs1),
               cpu->pipeline_ID_EX.inst.rs2, cpu->pipeline_ID_EX.val_rs2, src_name(cpu->pipeline_ID_EX.src_rs2),
               cpu->pipeline_ID_EX.alu_result);
    }

    print_stage_inst("MEM", &cpu->pipeline_EX_MEM); printf("\n");

    if (cpu->pipeline_MEM_WB.inst.valid && cpu->pipeline_MEM_WB.inst.rd != REG_UNUSED && cpu->pipeline_MEM_WB.inst.op != OP_NOOP) {
        printf("WB    : %-20s (write R%d=%d)\n",
               cpu->pipeline_MEM_WB.inst.text,
               cpu->pipeline_MEM_WB.inst.rd,
               cpu->pipeline_MEM_WB.alu_result);
    } else {
        print_stage_inst("WB", &cpu->pipeline_MEM_WB); printf("\n");
    }

    // Registers
    printf("\nRegisters: ");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, cpu->R[i]);
        if ((i + 1) % 8 == 0) printf("\n           ");
    }
    printf("\n");
}

// ---------- main ----------
/**
 * @brief Main entry point: load program, run pipeline simulation
 * @return 0 on success, 1 if program load failed
 */
int main() {
    CPU cpu;
    memset(&cpu, 0, sizeof(CPU));
    for (int i = 0; i < NUM_REGS; ++i) cpu.R[i] = 0;
    // Initialize memory to zero
    for (int i = 0; i < MEM_SIZE_WORDS; ++i) cpu.memory[i] = 0;

    cpu.PC = 0;

    if (program_load(&cpu, "inst.txt") != 0) {
        fprintf(stderr, "Could not open inst.txt. Please create it.\n");
        return 1;
    }

    init_pipeline(&cpu);
    int cycle = 1;

    // Prime pipeline_IF_ID with first fetch so the first cycle shows ID properly
    Instruction first;
    fetch_stage(&cpu, &first);        // Fetch first instruction
    cpu.pipeline_IF_ID.inst = first;  // Load into IF/ID latch
    cpu.PC++;                         // ✅ Increment PC once here

    while (cpu.PC < cpu.inst_count || !pipeline_is_empty(&cpu)) {
        // ---- Phase 1: compute ----
        wb_stage(&cpu);
        MemResult mem_res = memory_stage(&cpu, cpu.pipeline_EX_MEM);
        ExecResult ex_res = execute_stage(&cpu, cpu.pipeline_ID_EX);

        DecodeResult dec_res = decode_stage(&cpu, cpu.pipeline_IF_ID, cpu.pipeline_ID_EX);
        Instruction fetched_inst;
        fetch_stage(&cpu, &fetched_inst);

        // ---- Phase 2: print ----
        // Use the execute result just for printing the EX line
        StageLatch saved_pipeline_ID_EX = cpu.pipeline_ID_EX;
        cpu.pipeline_ID_EX = ex_res.next;

        print_cycle_state(&cpu, cycle, dec_res.stall, dec_res.stall_reason);

        // Restore the original latched view before we advance
        cpu.pipeline_ID_EX = saved_pipeline_ID_EX;

        // ---- Phase 3: latch update ----
        advance_pipeline(&cpu, ex_res, mem_res, fetched_inst, dec_res);

        cycle++;
    }

    // Final summary
    printf("\n=============== FINAL REGISTER STATE ===============\n");
    for (int i = 0; i < NUM_REGS; ++i) {
        printf("R%-2d=%-5d ", i, cpu.R[i]);
        if ((i + 1) % 8 == 0) printf("\n");
    }


    printf("\nTotal cycles: %d\n", cycle - 1);

    return 0;
}
