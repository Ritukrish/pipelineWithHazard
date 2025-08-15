#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX 100

// define a stage
struct stage {
    char instr[10];
    char op1[10];
    char op2[10];
    char op3[10];
    int valid;
};

// make 5 stages
struct stage IF, ID, EX, MEM, WB;

// program and registers
char program[MAX][100];
int reg[8];
int pc = 0;
int n = 0;

// get register index like r1 => 0
int getIndex(char *r) {
    if (r[0] == 'r') {
        return atoi(r + 1) - 1;
    }
    return -1; 
}

void doWB(struct stage *s) {
    if (s->valid == 0) return;

    if (strcmp(s->instr, "mov") == 0) {
        int rd = getIndex(s->op1);
        int val = atoi(s->op2);
        reg[rd] = val;
    }
    if (strcmp(s->instr, "add") == 0) {
        int rd = getIndex(s->op1);
        int rs1 = getIndex(s->op2);
        int rs2 = getIndex(s->op3);
        reg[rd] = reg[rs1] + reg[rs2];
    }
    if (strcmp(s->instr, "mul") == 0) {
        int rd = getIndex(s->op1);
        int rs1 = getIndex(s->op2);
        int rs2 = getIndex(s->op3);
        reg[rd] = reg[rs1] * reg[rs2];
    }
}

int main() {

    FILE *fp;
    fp = fopen("instruction.txt", "r");
    if (fp == NULL) {
        printf("file not found\n");
        return 1;
    }

    while (fgets(program[n], 100, fp)) {
        n++;
    }
    fclose(fp);

    // init
    IF.valid = ID.valid = EX.valid = MEM.valid = WB.valid = 0;

    int done = 0;
    int cycle = 1;

    while (!done) {

        printf("\nCycle %d:\n", cycle);

        doWB(&WB);

        WB = MEM;
        MEM = EX;
        EX = ID;
        ID = IF;

        if (pc < n) {
            char i[10], o1[10], o2[10], o3[10];
            int x = sscanf(program[pc], "%s %[^,],%[^,],%s", i, o1, o2, o3);
            IF.valid = 1;
            strcpy(IF.instr, i);
            strcpy(IF.op1, o1);
            if (x >= 3) strcpy(IF.op2, o2); else strcpy(IF.op2, "");
            if (x >= 4) strcpy(IF.op3, o3); else strcpy(IF.op3, "");
            pc++;
        } else {
            IF.valid = 0;
        }

        printf("IF : %s %s %s %s\n", IF.valid ? IF.instr : "-", IF.op1, IF.op2, IF.op3);
        printf("ID : %s %s %s %s\n", ID.valid ? ID.instr : "-", ID.op1, ID.op2, ID.op3);
        printf("EX : %s %s %s %s\n", EX.valid ? EX.instr : "-", EX.op1, EX.op2, EX.op3);
        printf("MEM: %s %s %s %s\n", MEM.valid ? MEM.instr : "-", MEM.op1, MEM.op2, MEM.op3);
        printf("WB : %s %s %s %s\n", WB.valid ? WB.instr : "-", WB.op1, WB.op2, WB.op3);

        if (pc >= n && IF.valid == 0 && ID.valid == 0 && EX.valid == 0 && MEM.valid == 0 && WB.valid == 0) {
            done = 1;
        }

        cycle++;
    }

    printf("\nRegisters:\n");
    for (int i = 0; i < 8; i++) {
        printf("r%d = %d\n", i + 1, reg[i]);
    }

    return 0;
}
