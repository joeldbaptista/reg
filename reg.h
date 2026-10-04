/* reg - shared instruction set definitions for the assembler and VM */
#include <stdint.h>

#define NREG      16     /* r0..r15 */
#define MAXINSTR 4096    /* program-size cap, in instructions */
#define WORDSZ     8     /* bytes per word (64-bit) */
#define REG_MAGIC "REG1" /* 4-byte on-disk file magic, no NUL */

typedef enum {
	R0,    /* return value */
	R1,
	R2,
	R3,
	R4,
	R5,    /* first argument */
	R6,    /* base pointer */
	R7,    /* stack pointer */
	R8,
	R9,
	R10,
	R11,
	R12,
	R13,
	R14,
	R15,
} Reg;

#define SP R7    /* register push/pop/call/return use as stack pointer */

typedef enum {
	OP_PUT,
	OP_MOV,
	OP_LOAD,
	OP_STORE,
	OP_PUSH,
	OP_POP,
	OP_INC,
	OP_DEC,
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_REM,
	OP_CMP,
	OP_NOT,
	OP_AND,
	OP_OR,
	OP_JMP,
	OP_CALL,
	OP_RET,
	OP_JE,
	OP_JL,
	OP_JG,
	OP_JGE,
	OP_JLE,
	OP_JNE,
	OP_HALT,    /* must stay last: the VM rejects any opcode above it */
} Op;

typedef struct instr Instr;
struct instr {
	Op      op;
	int     r1;     /* first register operand, unused by ops that don't take one */
	int     r2;     /* second register operand (destination of a 2-reg op) */
	int64_t imm;    /* immediate operand, or resolved jump/call target address */
	int     immsrc; /* 1 if the source operand is imm instead of r1 */
};

/*
 * On-disk format: one Header followed by Header.ninstr EncInstr
 * records. Every field is already naturally aligned, so there is no
 * implicit compiler padding and the struct can be read/written with
 * a single fread/fwrite, as long as the assembler and VM are built
 * with the same compiler/ABI (guaranteed here by the shared Makefile).
 */
typedef struct {
	char    magic[4];  /* REG_MAGIC */
	int32_t ninstr;    /* number of EncInstr records following */
	int32_t entry;     /* instruction index where execution begins */
	int32_t ndata;     /* number of initial data words following the instructions */
} Header;

typedef struct {
	uint8_t op;        /* Op enum value */
	uint8_t r1;        /* first register operand, 0 if unused */
	uint8_t r2;        /* second register operand, 0 if unused */
	uint8_t immsrc;    /* 1 if the source operand is imm instead of r1 */
	uint8_t pad[4];    /* reserved, always zero */
	int64_t imm;       /* immediate, or resolved jump/call address */
} EncInstr;
