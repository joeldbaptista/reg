/* reg - register machine virtual machine */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arg.h"
#include "reg.h"
#include "util.h"

#define STACKSZ  4096   /* words */
#define MEMSZ    4096   /* words */

typedef struct {
	int64_t regs[NREG];   /* indexed by Reg; regs[SP] is the stack pointer */
	int64_t stack[STACKSZ];
	int64_t mem[MEMSZ];    /* static data, addressed by word index */
	int     ndata;          /* number of mem[] words actually populated */
	int     zf, sf, of;
	int     pc;
} Machine;

static int loadprog(const char *inpath, Instr prog[], int max, int *entry, Machine *m);
static void push(Machine *m, int64_t v);
static int64_t pop(Machine *m);
static int64_t srcval(const Machine *m, const Instr *in);
static void execute(const Instr *prog, int nprog, int entry, Machine *m);
static void dumpstate(const char *outpath, const Machine *m);
static void run(const char *inpath, const char *outpath);
static void usage(void);

char *argv0;

static const char *regnames[NREG] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

static int
loadprog(const char *inpath, Instr prog[], int max, int *entry, Machine *m)
{
	FILE *fp;
	Header hdr;
	EncInstr e;
	int i;

	if (!(fp = fopen(inpath, "rb")))
		die("%s:", inpath);

	if (fread(&hdr, sizeof(hdr), 1, fp) != 1)
		die("%s: truncated header", inpath);
	if (memcmp(hdr.magic, REG_MAGIC, 4) != 0)
		die("%s: bad magic (not a reg binary)", inpath);
	if (hdr.ninstr < 0 || hdr.ninstr > max)
		die("%s: invalid instruction count %d", inpath, hdr.ninstr);
	if (hdr.entry < 0 || hdr.entry >= hdr.ninstr)
		die("%s: invalid entry point %d", inpath, hdr.entry);
	if (hdr.ndata < 0 || hdr.ndata > MEMSZ)
		die("%s: invalid data word count %d", inpath, hdr.ndata);

	for (i = 0; i < hdr.ninstr; i++) {
		if (fread(&e, sizeof(e), 1, fp) != 1)
			die("%s: truncated instruction stream", inpath);
		if (e.op > OP_HALT)
			die("%s: invalid opcode %d at instruction %d", inpath, e.op, i);
		if (e.r1 >= NREG || e.r2 >= NREG)
			die("%s: invalid register at instruction %d", inpath, i);
		if (e.immsrc > 1 ||
		    (e.immsrc && (e.op < OP_ADD || e.op > OP_CMP)))
			die("%s: invalid immediate source at instruction %d",
			    inpath, i);
		if ((e.op == OP_LOAD || e.op == OP_STORE) &&
		    (e.imm < 0 || e.imm >= hdr.ndata))
			die("%s: invalid data address %lld at instruction %d",
			    inpath, (long long)e.imm, i);

		prog[i].op = (Op)e.op;
		prog[i].r1 = e.r1;
		prog[i].r2 = e.r2;
		prog[i].imm = e.imm;
		prog[i].immsrc = e.immsrc;
	}

	for (i = 0; i < hdr.ndata; i++)
		if (fread(&m->mem[i], sizeof(m->mem[i]), 1, fp) != 1)
			die("%s: truncated data section", inpath);
	m->ndata = hdr.ndata;

	fclose(fp);

	*entry = hdr.entry;

	return hdr.ninstr;
}

static void
push(Machine *m, int64_t v)
{
	if (m->regs[SP] <= 0)
		die("pc=%d: stack overflow", m->pc);
	m->regs[SP]--;
	m->stack[m->regs[SP]] = v;
}

static int64_t
pop(Machine *m)
{
	int64_t v;

	if (m->regs[SP] >= STACKSZ)
		die("pc=%d: stack underflow", m->pc);
	v = m->stack[m->regs[SP]];
	m->regs[SP]++;

	return v;
}

static int64_t
srcval(const Machine *m, const Instr *in)
{
	return in->immsrc ? in->imm : m->regs[in->r1];
}

static void
execute(const Instr *prog, int nprog, int entry, Machine *m)
{
	Instr in;
	int64_t a, b, r;

	m->pc = entry;
	for (;;) {
		if (m->pc < 0 || m->pc >= nprog)
			die("pc out of range: %d", m->pc);

		in = prog[m->pc];

		if (in.op == OP_HALT)
			break;

		switch (in.op) {
		case OP_PUT:
			m->regs[in.r1] = in.imm;
			m->pc++;
			break;
		case OP_MOV:
			m->regs[in.r2] = m->regs[in.r1];
			m->pc++;
			break;
		case OP_LOAD:
			m->regs[in.r1] = m->mem[in.imm];
			m->pc++;
			break;
		case OP_STORE:
			m->mem[in.imm] = m->regs[in.r1];
			m->pc++;
			break;
		case OP_PUSH:
			push(m, m->regs[in.r1]);
			m->pc++;
			break;
		case OP_POP:
			m->regs[in.r1] = pop(m);
			m->pc++;
			break;
		case OP_INC:
			m->regs[in.r1]++;
			m->pc++;
			break;
		case OP_DEC:
			m->regs[in.r1]--;
			m->pc++;
			break;
		case OP_ADD:
			m->regs[in.r2] += srcval(m, &in);
			m->pc++;
			break;
		case OP_SUB:
			m->regs[in.r2] -= srcval(m, &in);
			m->pc++;
			break;
		case OP_MUL:
			m->regs[in.r2] *= srcval(m, &in);
			m->pc++;
			break;
		case OP_DIV:
			a = srcval(m, &in);
			if (a == 0)
				die("pc=%d: division by zero", m->pc);
			if (a == -1 && m->regs[in.r2] == INT64_MIN)
				die("pc=%d: division overflow", m->pc);
			m->regs[in.r2] /= a;
			m->pc++;
			break;
		case OP_REM:
			a = srcval(m, &in);
			if (a == 0)
				die("pc=%d: division by zero", m->pc);
			if (a == -1 && m->regs[in.r2] == INT64_MIN)
				die("pc=%d: division overflow", m->pc);
			m->regs[in.r2] %= a;
			m->pc++;
			break;
		case OP_CMP:
			a = srcval(m, &in);
			b = m->regs[in.r2];
			r = (int64_t)((uint64_t)b - (uint64_t)a);
			m->zf = (r == 0);
			m->sf = (r < 0);
			m->of = (((b ^ a) & (b ^ r)) < 0);
			m->pc++;
			break;
		case OP_NOT:
			m->zf = (m->regs[in.r1] != 0);
			m->pc++;
			break;
		case OP_AND:
			m->zf = !((m->regs[in.r1] != 0) && (m->regs[in.r2] != 0));
			m->pc++;
			break;
		case OP_OR:
			m->zf = !((m->regs[in.r1] != 0) || (m->regs[in.r2] != 0));
			m->pc++;
			break;
		case OP_JMP:
			m->pc = (int)in.imm;
			break;
		case OP_CALL:
			push(m, m->pc + 1);
			m->pc = (int)in.imm;
			break;
		case OP_RET:
			m->pc = (int)pop(m);
			break;
		case OP_JE:
			m->pc = m->zf ? (int)in.imm : m->pc + 1;
			break;
		case OP_JL:
			m->pc = (m->sf != m->of) ? (int)in.imm : m->pc + 1;
			break;
		case OP_JG:
			m->pc = (!m->zf && m->sf == m->of) ? (int)in.imm : m->pc + 1;
			break;
		case OP_JGE:
			m->pc = (m->sf == m->of) ? (int)in.imm : m->pc + 1;
			break;
		case OP_JLE:
			m->pc = (m->zf || m->sf != m->of) ? (int)in.imm : m->pc + 1;
			break;
		case OP_JNE:
			m->pc = !m->zf ? (int)in.imm : m->pc + 1;
			break;
		default:
			die("pc=%d: invalid opcode %d", m->pc, in.op);
		}
	}
}

static void
dumpstate(const char *outpath, const Machine *m)
{
	FILE *fp;
	int i;

	if (!(fp = fopen(outpath, "w")))
		die("%s:", outpath);

	for (i = 0; i < NREG; i++)
		fprintf(fp, "%s = %lld\n", regnames[i], (long long)m->regs[i]);
	fprintf(fp, "zf = %d\n", m->zf);
	fprintf(fp, "sf = %d\n", m->sf);
	fprintf(fp, "of = %d\n", m->of);

	for (i = 0; i < m->ndata; i++)
		fprintf(fp, "mem[%d] = %lld\n", i, (long long)m->mem[i]);

	fclose(fp);
}

static void
usage(void)
{
	die("usage: %s -i program -o out", argv0);
}

static void
run(const char *inpath, const char *outpath)
{
	Instr prog[MAXINSTR];
	Machine m;
	int nprog, entry;

	memset(&m, 0, sizeof(m));
	m.regs[SP] = STACKSZ;
	nprog = loadprog(inpath, prog, MAXINSTR, &entry, &m);
	execute(prog, nprog, entry, &m);
	dumpstate(outpath, &m);
}

int
main(int argc, char *argv[])
{
	char *inpath, *outpath;

	inpath = NULL;
	outpath = NULL;

	ARGBEGIN {
	case 'i':
		inpath = EARGF(usage());
		break;
	case 'o':
		outpath = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND

	if (!inpath || !outpath)
		usage();

	run(inpath, outpath);

	return 0;
}
