#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INST 100
#define REG_COUNT 8

typedef struct {
    char opcode[10];
    int rd, rs1, rs2;
    int imm; // For mov immediate
    int isImmediate;
} Instruction;

int registers[REG_COUNT] = {0};

// Execute one instruction
void execute(Instruction inst) {
    if (strcmp(inst.opcode, "mov") == 0 && inst.isImmediate) {
        registers[inst.rd] = inst.imm;
    }
    else if (strcmp(inst.opcode, "add") == 0) {
        registers[inst.rd] = registers[inst.rs1] + registers[inst.rs2];
    }
    else if (strcmp(inst.opcode, "sub") == 0) {
        registers[inst.rd] = registers[inst.rs1] - registers[inst.rs2];
    }
    else if (strcmp(inst.opcode, "mul") == 0) {
        registers[inst.rd] = registers[inst.rs1] * registers[inst.rs2];
    }
}

// Detect RAW hazard
int hasHazard(Instruction prev, Instruction curr) {
    if (strcmp(curr.opcode, "mov") == 0) return 0; // MOV immediate has no hazard
    return (curr.rs1 == prev.rd || curr.rs2 == prev.rd);
}

int main() {
    FILE *fin, *fout;
    Instruction inst[MAX_INST];
    int count = 0, cycles = 0, stalls = 0;

    // 1. Read instructions
    fin = fopen("instructions.txt", "r");
    if (!fin) {
        printf("Error: Cannot open instructions.txt\n");
        return 1;
    }

    char line[50];
    while (fgets(line, sizeof(line), fin)) {
        Instruction temp;
        temp.isImmediate = 0;
        temp.rs1 = temp.rs2 = temp.imm = 0;

        if (sscanf(line, "mov R%d , %d", &temp.rd, &temp.imm) == 2 ||
            sscanf(line, "mov R%d, %d", &temp.rd, &temp.imm) == 2) {
            strcpy(temp.opcode, "mov");
            temp.isImmediate = 1;
        }
        else if (sscanf(line, "add R%d , R%d , R%d", &temp.rd, &temp.rs1, &temp.rs2) == 3 ||
                 sscanf(line, "add R%d, R%d, R%d", &temp.rd, &temp.rs1, &temp.rs2) == 3) {
            strcpy(temp.opcode, "add");
        }
        else if (sscanf(line, "sub R%d , R%d , R%d", &temp.rd, &temp.rs1, &temp.rs2) == 3 ||
                 sscanf(line, "sub R%d, R%d, R%d", &temp.rd, &temp.rs1, &temp.rs2) == 3) {
            strcpy(temp.opcode, "sub");
        }
        else if (sscanf(line, "mul R%d , R%d , R%d", &temp.rd, &temp.rs1, &temp.rs2) == 3 ||
                 sscanf(line, "mul R%d, R%d, R%d", &temp.rd, &temp.rs1, &temp.rs2) == 3) {
            strcpy(temp.opcode, "mul");
        }
        inst[count++] = temp;
    }
    fclose(fin);

    // 2. Pipeline execution with hazard stalls
    for (int i = 0; i < count; i++) {
        if (i > 0 && hasHazard(inst[i-1], inst[i])) {
            stalls += 2;
            cycles += 2;
        }
        execute(inst[i]);
        cycles++;
    }

    // Add pipeline fill/drain cycles
    cycles += 4;

    // 3. Write results to file
    fout = fopen("output.txt", "w");
    if (!fout) {
        printf("Error: Cannot open output.txt\n");
        return 1;
    }

    fprintf(fout, "Final Register Values:\n");
    for (int i = 0; i < REG_COUNT; i++) {
        fprintf(fout, "R%d = %d\n", i, registers[i]);
    }

    fprintf(fout, "\nTotal Instructions = %d\n", count);
    fprintf(fout, "Stalls Inserted    = %d\n", stalls);
    fprintf(fout, "Total Cycles       = %d\n", cycles);

    fclose(fout);

    printf("Pipeline simulation complete. Check output.txt\n");
    return 0;
}
