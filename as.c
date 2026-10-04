/* regas - assembler for the reg register machine */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arg.h"
#include "reg.h"
#include "util.h"

#define MAXLINES 4096
#define MAXLINE   256
#define MAXLABEL   64
#define MAXSYM    256
#define MAXTOK     16

typedef enum {
	TOK_IDENT,
	TOK_NUM,
	TOK_OP,
} TokKind;

typedef struct {
	TokKind kind;
	char    s[MAXLABEL];   /* source text of the token */
	int64_t num;           /* value, for TOK_NUM */
} Tok;

typedef enum {
	SYM_CODE,
	SYM_DATA,
} SymKind;

typedef enum {
	SECT_CODE,
	SECT_STATIC,
} Section;

typedef struct {
	char    name[MAXLABEL];
	int64_t addr;
	SymKind kind;
} Sym;

typedef struct {
	char *name;
	Op    op;
} OpName;

static void readlines(const char *inpath);
static void stripcomment(char *s);
static int lex(const char *inpath, int lineno, const char *s, Tok *t);
static int isop(const Tok *t, const char *op);
static int iskw(const Tok *t, const char *kw);
static int regnum(const Tok *t);
static int isreserved(const char *name);
static int expectreg(const char *inpath, int lineno, const Tok *t);
static const char *expectname(const char *inpath, int lineno, const Tok *t);
static int findsym(const char *name, int64_t *addr, SymKind *kind);
static void addsym(const char *inpath, int lineno, const char *name, int64_t addr, SymKind kind);
static int64_t symaddr(const char *inpath, int lineno, const Tok *t, SymKind expect);
static int islabelline(const char *inpath, int lineno, const Tok *t, int n);
static int isdecl(const Tok *t, int n);
static int stmtsize(const Tok *t, int n);
static void emit(Instr *in, Op op, int r1, int r2, int64_t imm, int immsrc);
static int parseif(const char *inpath, int lineno, const Tok *t, int n, Instr *out);
static int parsestmt(const char *inpath, int lineno, const Tok *t, int n, Instr *out);
static void assemble(const char *inpath, const char *outpath);
static void usage(void);

char *argv0;

/* longest first, so "+=" is never read as "+" followed by "=" */
static const char *opers[] = {
	"+=", "-=", "*=", "/=", "%=", "++", "--",
	"==", "!=", "<=", ">=", "&&", "||",
	"=", "<", ">", "!", "*", ":",
};

static const OpName ariths[] = {
	{"+=", OP_ADD}, {"-=", OP_SUB}, {"*=", OP_MUL},
	{"/=", OP_DIV}, {"%=", OP_REM},
};

static const OpName relops[] = {
	{"==", OP_JE}, {"!=", OP_JNE}, {"<", OP_JL},
	{">", OP_JG}, {"<=", OP_JLE}, {">=", OP_JGE},
};

static const char *regnames[NREG] = {
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
};

static const char *keywords[] = {
	"if", "goto", "call", "return", "stop", "push", "pop", "static",
};

static char lines[MAXLINES][MAXLINE];
static int nlines;

static Sym syms[MAXSYM];
static int nsym;

static void
readlines(const char *inpath)
{
	FILE *fp;
	size_t len;

	if (!(fp = fopen(inpath, "r")))
		die("%s:", inpath);

	nlines = 0;
	while (fgets(lines[nlines], MAXLINE, fp)) {
		len = strlen(lines[nlines]);
		if (len > 0 && lines[nlines][len-1] == '\n')
			lines[nlines][len-1] = '\0';
		if (++nlines >= MAXLINES)
			die("%s: too many lines (max %d)", inpath, MAXLINES);
	}

	fclose(fp);
}

static void
stripcomment(char *s)
{
	char *p;

	if ((p = strchr(s, '#')))
		*p = '\0';
}

static int
lex(const char *inpath, int lineno, const char *s, Tok *t)
{
	const char *p;
	char *end;
	size_t i, len;
	int n;

	n = 0;
	p = s;
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\r')
			p++;
		if (*p == '\0')
			break;
		if (n >= MAXTOK)
			die("%s:%d: too many tokens", inpath, lineno);

		if (isdigit((unsigned char)*p) ||
		    (*p == '-' && isdigit((unsigned char)p[1]))) {
			errno = 0;
			t[n].kind = TOK_NUM;
			t[n].num = strtoll(p, &end, 10);
			if (errno == ERANGE || isalnum((unsigned char)*end) ||
			    *end == '_')
				die("%s:%d: invalid number", inpath, lineno);
			len = end - p;
		} else if (isalpha((unsigned char)*p) || *p == '_') {
			t[n].kind = TOK_IDENT;
			len = 0;
			while (isalnum((unsigned char)p[len]) || p[len] == '_')
				len++;
		} else {
			for (i = 0; i < sizeof(opers)/sizeof(opers[0]); i++)
				if (!strncmp(p, opers[i], strlen(opers[i])))
					break;
			if (i == sizeof(opers)/sizeof(opers[0]))
				die("%s:%d: unexpected character '%c'",
				    inpath, lineno, *p);
			t[n].kind = TOK_OP;
			len = strlen(opers[i]);
		}

		if (len >= sizeof(t[n].s))
			die("%s:%d: token too long", inpath, lineno);
		memcpy(t[n].s, p, len);
		t[n].s[len] = '\0';
		p += len;
		n++;
	}

	return n;
}

static int
isop(const Tok *t, const char *op)
{
	return t->kind == TOK_OP && strcmp(t->s, op) == 0;
}

static int
iskw(const Tok *t, const char *kw)
{
	return t->kind == TOK_IDENT && strcmp(t->s, kw) == 0;
}

static int
regnum(const Tok *t)
{
	int i;

	if (t->kind != TOK_IDENT)
		return -1;

	for (i = 0; i < NREG; i++)
		if (strcmp(regnames[i], t->s) == 0)
			return i;

	return -1;
}

static int
isreserved(const char *name)
{
	size_t i;

	for (i = 0; i < NREG; i++)
		if (strcmp(regnames[i], name) == 0)
			return 1;
	for (i = 0; i < sizeof(keywords)/sizeof(keywords[0]); i++)
		if (strcmp(keywords[i], name) == 0)
			return 1;

	return 0;
}

static int
expectreg(const char *inpath, int lineno, const Tok *t)
{
	int r;

	if ((r = regnum(t)) < 0)
		die("%s:%d: expected register, got '%s'", inpath, lineno, t->s);

	return r;
}

static const char *
expectname(const char *inpath, int lineno, const Tok *t)
{
	if (t->kind != TOK_IDENT || isreserved(t->s))
		die("%s:%d: expected label, got '%s'", inpath, lineno, t->s);

	return t->s;
}

static int
findsym(const char *name, int64_t *addr, SymKind *kind)
{
	int i;

	for (i = 0; i < nsym; i++) {
		if (strcmp(syms[i].name, name) == 0) {
			*addr = syms[i].addr;
			*kind = syms[i].kind;
			return 1;
		}
	}

	return 0;
}

static void
addsym(const char *inpath, int lineno, const char *name, int64_t addr, SymKind kind)
{
	int64_t dummyaddr;
	SymKind dummykind;

	if (isreserved(name))
		die("%s:%d: '%s' is a reserved word", inpath, lineno, name);
	if (findsym(name, &dummyaddr, &dummykind))
		die("%s:%d: duplicate label '%s'", inpath, lineno, name);

	if (nsym >= MAXSYM)
		die("%s:%d: too many labels (max %d)", inpath, lineno, MAXSYM);

	strcpy(syms[nsym].name, name);
	syms[nsym].addr = addr;
	syms[nsym].kind = kind;
	nsym++;
}

static int64_t
symaddr(const char *inpath, int lineno, const Tok *t, SymKind expect)
{
	const char *name;
	int64_t addr;
	SymKind kind;

	name = expectname(inpath, lineno, t);
	if (!findsym(name, &addr, &kind))
		die("%s:%d: undefined label '%s'", inpath, lineno, name);
	if (kind != expect)
		die("%s:%d: label '%s' is a %s label, expected %s", inpath, lineno,
		    name, kind == SYM_CODE ? "code" : "static data",
		    expect == SYM_CODE ? "code" : "static data");

	return addr;
}

static int
islabelline(const char *inpath, int lineno, const Tok *t, int n)
{
	if (n < 2 || !isop(&t[1], ":"))
		return 0;
	if (t[0].kind != TOK_IDENT)
		die("%s:%d: invalid label '%s'", inpath, lineno, t[0].s);
	if (n != 2)
		die("%s:%d: label must be alone on its line", inpath, lineno);

	return 1;
}

static int
isdecl(const Tok *t, int n)
{
	return n == 3 && t[0].kind == TOK_IDENT && isop(&t[1], "=") &&
	    t[2].kind == TOK_NUM;
}

/* number of instructions a statement line assembles to */
static int
stmtsize(const Tok *t, int n)
{
	return (n > 0 && iskw(&t[0], "if")) ? 2 : 1;
}

static void
emit(Instr *in, Op op, int r1, int r2, int64_t imm, int immsrc)
{
	in->op = op;
	in->r1 = r1;
	in->r2 = r2;
	in->imm = imm;
	in->immsrc = immsrc;
}

/*
 * if a <rel> b goto L   ->  cmp b, a (flags from a - b); j<rel> L
 * if !a goto L          ->  not a;       jne L
 * if a && b goto L      ->  and a, b;    jne L
 * if a || b goto L      ->  or a, b;     jne L
 */
static int
parseif(const char *inpath, int lineno, const Tok *t, int n, Instr *out)
{
	int64_t target;
	size_t i;
	int a;

	if (n < 4 || !iskw(&t[n-2], "goto"))
		die("%s:%d: expected 'if <condition> goto <label>'",
		    inpath, lineno);
	target = symaddr(inpath, lineno, &t[n-1], SYM_CODE);

	if (n == 5 && isop(&t[1], "!")) {
		emit(&out[0], OP_NOT, expectreg(inpath, lineno, &t[2]), 0, 0, 0);
		emit(&out[1], OP_JNE, 0, 0, target, 0);
		return 2;
	}
	if (n != 6)
		die("%s:%d: invalid condition", inpath, lineno);

	a = expectreg(inpath, lineno, &t[1]);
	if (isop(&t[2], "&&") || isop(&t[2], "||")) {
		emit(&out[0], isop(&t[2], "&&") ? OP_AND : OP_OR, a,
		    expectreg(inpath, lineno, &t[3]), 0, 0);
		emit(&out[1], OP_JNE, 0, 0, target, 0);
		return 2;
	}
	for (i = 0; i < sizeof(relops)/sizeof(relops[0]); i++) {
		if (!isop(&t[2], relops[i].name))
			continue;
		if (t[3].kind == TOK_NUM)
			emit(&out[0], OP_CMP, 0, a, t[3].num, 1);
		else
			emit(&out[0], OP_CMP, expectreg(inpath, lineno, &t[3]),
			    a, 0, 0);
		emit(&out[1], relops[i].op, 0, 0, target, 0);
		return 2;
	}

	die("%s:%d: invalid operator '%s' in condition", inpath, lineno, t[2].s);
	return 0; /* NOTREACHED */
}

/* assembles one statement into out[], returns the instruction count */
static int
parsestmt(const char *inpath, int lineno, const Tok *t, int n, Instr *out)
{
	size_t i;
	int r;

	if (iskw(&t[0], "if"))
		return parseif(inpath, lineno, t, n, out);

	if (iskw(&t[0], "stop") || iskw(&t[0], "return")) {
		if (n != 1)
			die("%s:%d: '%s' takes no operand", inpath, lineno, t[0].s);
		emit(out, iskw(&t[0], "stop") ? OP_HALT : OP_RET, 0, 0, 0, 0);
		return 1;
	}
	if (iskw(&t[0], "goto") || iskw(&t[0], "call")) {
		if (n != 2)
			die("%s:%d: '%s' takes one label", inpath, lineno, t[0].s);
		emit(out, iskw(&t[0], "goto") ? OP_JMP : OP_CALL, 0, 0,
		    symaddr(inpath, lineno, &t[1], SYM_CODE), 0);
		return 1;
	}
	if (iskw(&t[0], "push") || iskw(&t[0], "pop")) {
		if (n != 2)
			die("%s:%d: '%s' takes one register", inpath, lineno,
			    t[0].s);
		emit(out, iskw(&t[0], "push") ? OP_PUSH : OP_POP,
		    expectreg(inpath, lineno, &t[1]), 0, 0, 0);
		return 1;
	}

	/* *x = r */
	if (isop(&t[0], "*")) {
		if (n != 4 || !isop(&t[2], "="))
			die("%s:%d: expected '*<name> = <register>'", inpath, lineno);
		emit(out, OP_STORE, expectreg(inpath, lineno, &t[3]), 0,
		    symaddr(inpath, lineno, &t[1], SYM_DATA), 0);
		return 1;
	}

	/* ++r, --r */
	if (isop(&t[0], "++") || isop(&t[0], "--")) {
		if (n != 2)
			die("%s:%d: '%s' takes one register", inpath, lineno,
			    t[0].s);
		emit(out, isop(&t[0], "++") ? OP_INC : OP_DEC,
		    expectreg(inpath, lineno, &t[1]), 0, 0, 0);
		return 1;
	}

	/* every other statement starts with its destination register */
	if ((r = regnum(&t[0])) < 0)
		die("%s:%d: invalid statement starting with '%s'", inpath, lineno,
		    t[0].s);

	if (n == 2 && (isop(&t[1], "++") || isop(&t[1], "--")))
		die("%s:%d: '%s' must come before the register ('%s%s')",
		    inpath, lineno, t[1].s, t[1].s, t[0].s);
	if (n < 3)
		die("%s:%d: invalid statement", inpath, lineno);

	if (isop(&t[1], "=")) {
		if (n == 4 && isop(&t[2], "*")) {
			emit(out, OP_LOAD, r, 0,
			    symaddr(inpath, lineno, &t[3], SYM_DATA), 0);
			return 1;
		}
		if (n != 3)
			die("%s:%d: invalid assignment", inpath, lineno);
		if (t[2].kind == TOK_NUM) {
			emit(out, OP_PUT, r, 0, t[2].num, 0);
			return 1;
		}
		if (regnum(&t[2]) < 0)
			die("%s:%d: '%s' is not a register (use '*%s' to read "
			    "static data)", inpath, lineno, t[2].s, t[2].s);
		emit(out, OP_MOV, regnum(&t[2]), r, 0, 0);
		return 1;
	}

	for (i = 0; i < sizeof(ariths)/sizeof(ariths[0]); i++) {
		if (!isop(&t[1], ariths[i].name))
			continue;
		if (n != 3)
			die("%s:%d: invalid '%s' statement", inpath, lineno, t[1].s);
		if (t[2].kind != TOK_NUM) {
			emit(out, ariths[i].op, expectreg(inpath, lineno, &t[2]),
			    r, 0, 0);
			return 1;
		}
		if ((ariths[i].op == OP_DIV || ariths[i].op == OP_REM) &&
		    t[2].num == 0)
			die("%s:%d: division by zero", inpath, lineno);
		emit(out, ariths[i].op, 0, r, t[2].num, 1);
		return 1;
	}

	die("%s:%d: invalid statement", inpath, lineno);
	return 0; /* NOTREACHED */
}

static void
assemble(const char *inpath, const char *outpath)
{
	FILE *fp;
	Header hdr;
	Tok t[MAXTOK];
	Instr instrs[MAXINSTR];
	int64_t data[MAXLINES];
	int64_t addr, dataaddr, entry;
	int i, n, ninstr, ndata;
	Section sect;
	SymKind entrykind;
	char buf[MAXLINE];

	readlines(inpath);

	/* pass 1: record code label and static data addresses */
	sect = SECT_CODE;
	addr = 0;
	dataaddr = 0;
	nsym = 0;
	for (i = 0; i < nlines; i++) {
		strcpy(buf, lines[i]);
		stripcomment(buf);
		if (!(n = lex(inpath, i+1, buf, t)))
			continue;

		if (islabelline(inpath, i+1, t, n)) {
			if (strcmp(t[0].s, "static") == 0) {
				sect = SECT_STATIC;
			} else {
				sect = SECT_CODE;
				addsym(inpath, i+1, t[0].s, addr, SYM_CODE);
			}
			continue;
		}

		if (sect == SECT_STATIC) {
			if (!isdecl(t, n))
				die("%s:%d: expected '<name> = <number>' in static "
				    "block", inpath, i+1);
			addsym(inpath, i+1, t[0].s, dataaddr++, SYM_DATA);
			continue;
		}

		addr += stmtsize(t, n);
		if (addr > MAXINSTR)
			die("%s:%d: too many instructions (max %d)", inpath, i+1,
			    MAXINSTR);
	}

	if (!findsym("main", &entry, &entrykind))
		die("%s: missing required label 'main' (entry point)", inpath);
	if (entrykind != SYM_CODE)
		die("%s: 'main' must be a code label", inpath);

	/* pass 2: assemble statements and data, resolving label references */
	sect = SECT_CODE;
	ninstr = 0;
	ndata = 0;
	for (i = 0; i < nlines; i++) {
		strcpy(buf, lines[i]);
		stripcomment(buf);
		if (!(n = lex(inpath, i+1, buf, t)))
			continue;

		if (islabelline(inpath, i+1, t, n)) {
			sect = strcmp(t[0].s, "static") == 0 ?
			    SECT_STATIC : SECT_CODE;
			continue;
		}

		if (sect == SECT_STATIC)
			data[ndata++] = t[2].num;
		else
			ninstr += parsestmt(inpath, i+1, t, n, &instrs[ninstr]);
	}

	if (!(fp = fopen(outpath, "wb")))
		die("%s:", outpath);

	memcpy(hdr.magic, REG_MAGIC, 4);
	hdr.ninstr = ninstr;
	hdr.entry = (int32_t)entry;
	hdr.ndata = ndata;
	if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1)
		die("%s: write failed", outpath);

	for (i = 0; i < ninstr; i++) {
		EncInstr e;

		memset(&e, 0, sizeof(e));
		e.op = (uint8_t)instrs[i].op;
		e.r1 = (uint8_t)instrs[i].r1;
		e.r2 = (uint8_t)instrs[i].r2;
		e.immsrc = (uint8_t)instrs[i].immsrc;
		e.imm = instrs[i].imm;

		if (fwrite(&e, sizeof(e), 1, fp) != 1)
			die("%s: write failed", outpath);
	}

	for (i = 0; i < ndata; i++)
		if (fwrite(&data[i], sizeof(data[i]), 1, fp) != 1)
			die("%s: write failed", outpath);

	fclose(fp);
}

static void
usage(void)
{
	die("usage: %s -i source.reg -o out", argv0);
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

	assemble(inpath, outpath);

	return 0;
}
