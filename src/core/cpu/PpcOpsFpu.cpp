/*
 * BeBox emulator - PowerPC floating point instructions.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Arithmetic is done with host IEEE doubles. The host exception flags
 * (MXCSR on x86) give overflow, underflow and inexact; invalid operation
 * and zero divide are classified explicitly because the PowerPC reports
 * their causes separately. NaN results follow the PowerPC precedence
 * (frA, frB, frC) instead of the host's. The common case of the
 * arithmetic instructions takes a fast path (ArithFast()) that reads the
 * host's flags at most once.
 */
#include "PpcOps.h"

#include <cmath>

#if defined(__x86_64__) || defined(__i386__)
#	include <xmmintrin.h>
#	define BEBOX_HAVE_MXCSR 1
#else
#	include <cfenv>
#endif

namespace bebox {

namespace {

enum : u32 {
	FX = 1u << 31, FEX = 1u << 30, VX = 1u << 29, OX = 1u << 28,
	UX = 1u << 27, ZX = 1u << 26, XX = 1u << 25, VXSNAN = 1u << 24,
	VXISI = 1u << 23, VXIDI = 1u << 22, VXZDZ = 1u << 21, VXIMZ = 1u << 20,
	VXVC = 1u << 19, FR = 1u << 18, FI = 1u << 17,
	FPRF_MASK = 0x1fu << 12, FPCC_MASK = 0xfu << 12,
	VXSOFT = 1u << 10, VXSQRT = 1u << 9, VXCVI = 1u << 8,
	VE = 1u << 7, OE = 1u << 6, UE = 1u << 5, ZE = 1u << 4, XE = 1u << 3,
	NI = 1u << 2, RN_MASK = 3u
};

const u32 kVxAll = VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT
	| VXSQRT | VXCVI;
const u32 kExceptionBits = OX | UX | ZX | XX | kVxAll;

const u64 kDefaultQNaN = 0x7ff8000000000000ull;

#define G c.gpr
#define F c.fpr

inline double
ToDouble(u64 bits)
{
	double value;
	memcpy(&value, &bits, 8);
	return value;
}


inline u64
ToBits(double value)
{
	u64 bits;
	memcpy(&bits, &value, 8);
	return bits;
}


inline bool IsNaN(u64 b) { return (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
inline bool IsSNaN(u64 b) { return IsNaN(b) && (b & 0x0008000000000000ull) == 0; }
inline bool IsInf(u64 b) { return (b & 0x7fffffffffffffffull) == 0x7ff0000000000000ull; }
inline bool IsZero(u64 b) { return (b & 0x7fffffffffffffffull) == 0; }
inline bool Sign(u64 b) { return (b >> 63) != 0; }
inline u64 Quiet(u64 b) { return b | 0x0008000000000000ull; }

// Host exception flags.
enum { kHostInvalid = 1, kHostDenormal = 2, kHostZeroDivide = 4,
	kHostOverflow = 8, kHostUnderflow = 16, kHostInexact = 32 };

inline void
ClearHostFlags()
{
#ifdef BEBOX_HAVE_MXCSR
	_mm_setcsr(_mm_getcsr() & ~0x3fu);
#else
	feclearexcept(FE_ALL_EXCEPT);
#endif
}


// Holds a value in a register at this point. The compiler does not know
// that host arithmetic sets the exception flags: pinning its operands
// after ClearHostFlags() and its result before HostFlags() keeps it in
// between.
template<typename T>
inline void
Pin(T& value)
{
#if defined(__x86_64__)
	__asm__ volatile("" : "+x"(value));
#else
	__asm__ volatile("" : "+m"(value));
#endif
}


inline u32
HostFlags()
{
#ifdef BEBOX_HAVE_MXCSR
	return _mm_getcsr() & 0x3f;
#else
	u32 flags = 0;
	if (fetestexcept(FE_INVALID)) flags |= kHostInvalid;
	if (fetestexcept(FE_DIVBYZERO)) flags |= kHostZeroDivide;
	if (fetestexcept(FE_OVERFLOW)) flags |= kHostOverflow;
	if (fetestexcept(FE_UNDERFLOW)) flags |= kHostUnderflow;
	if (fetestexcept(FE_INEXACT)) flags |= kHostInexact;
	return flags;
#endif
}


u64
SingleToDouble(u32 word)
{
	u32 exponent = (word >> 23) & 0xff;
	if (exponent == 0xff) {
		// infinity or NaN: keep the payload bit exact
		return ((u64)(word >> 31) << 63) | (0x7ffull << 52)
			| ((u64)(word & 0x7fffff) << 29);
	}
	float value;
	memcpy(&value, &word, 4);
	return ToBits((double)value);	// exact for all other values
}


// The PowerPC "store floating-point single" conversion (no rounding).
u32
DoubleToSingleStore(u64 bits)
{
	u32 exponent = (bits >> 52) & 0x7ff;
	if (exponent > 896 || (bits & 0x7fffffffffffffffull) == 0) {
		return (u32)((bits >> 32) & 0xc0000000u)
			| (u32)((bits >> 29) & 0x3fffffffu);
	}
	u32 sign = (u32)(bits >> 63) << 31;
	if (exponent >= 874) {
		int e = (int)exponent - 1023;
		u64 fraction = (1ull << 52) | (bits & 0xfffffffffffffull);
		while (e < -126) {
			fraction >>= 1;
			e++;
		}
		return sign | (u32)((fraction >> 29) & 0x7fffff);
	}
	return sign;
}


u32
Fprf(u64 b)
{
	bool negative = Sign(b);
	if (IsNaN(b))
		return 0x11;
	if (IsInf(b))
		return negative ? 0x09 : 0x05;
	if (IsZero(b))
		return negative ? 0x12 : 0x02;
	if (((b >> 52) & 0x7ff) == 0)
		return negative ? 0x18 : 0x14;	// denormalized
	return negative ? 0x08 : 0x04;
}


inline bool
CheckFpAvailable(PpcCpu& c)
{
	if ((c.GetMsr() & MSR_FP) == 0) {
		c.RaiseFpUnavailable();
		return false;
	}
	return true;
}


// Sets exception bits, FX for bits that were not set before, and the
// summary bits (which every change of the FPSCR keeps up to date: with no
// new bit they stay).
inline void
SetExceptions(PpcCpu& c, u32 bits)
{
	bits &= kExceptionBits;
	if ((bits & ~c.fFpscr) == 0)
		return;
	c.fFpscr |= FX | bits;
	PpcOps::UpdateFpscrSummary(c);
}


inline void
SetCr1(PpcCpu& c)
{
	PpcSetCrField(c, 1, c.fFpscr >> 28);
}


// Raises the program exception for enabled floating point exceptions.
inline bool
CheckEnabled(PpcCpu& c)
{
	if ((c.fFpscr & FEX) != 0 && (c.GetMsr() & (MSR_FE0 | MSR_FE1)) != 0) {
		c.RaiseProgram(PROGRAM_FP);
		return false;
	}
	return true;
}


enum ArithOp {
	kAdd, kSub, kMul, kDiv, kMadd, kMsub, kNmadd, kNmsub
};


// The bits of a finite double's significand from the highest one set to
// the lowest (0 for zero): two whose sum is at most 53 multiply exactly
// (single precision values have at most 24).
inline int
SignificantBits(u64 bits)
{
	u64 significand = bits & 0xfffffffffffffull;
	if ((bits & 0x7ff0000000000000ull) != 0)
		significand |= 1ull << 52;
	if (significand == 0)
		return 0;
	return 64 - __builtin_clzll(significand) - __builtin_ctzll(significand);
}


// The fused multiply-add for single precision: x * z + y rounded to odd
// (toward zero, the last bit set when that was inexact), so that rounding
// it to single is the one rounding the PowerPC does (53 bits are more than
// 24 + 1), in every rounding mode. An exact zero gets its sign from the
// current rounding mode. The host's flags must be clear; they stay set.
double
FmaToOdd(double x, double z, double y)
{
#ifdef BEBOX_HAVE_MXCSR
	u32 csr = _mm_getcsr();
	_mm_setcsr(csr | (3u << 13));		// toward zero
#else
	int rounding = fegetround();
	fesetround(FE_TOWARDZERO);
#endif
	Pin(x);
	Pin(z);
	Pin(y);
	double r = std::fma(x, z, y);
	Pin(r);
#ifdef BEBOX_HAVE_MXCSR
	u32 after = _mm_getcsr();
	_mm_setcsr((after & ~(3u << 13)) | (csr & (3u << 13)));
	bool inexact = (after & kHostInexact) != 0;
#else
	bool inexact = fetestexcept(FE_INEXACT) != 0;
	fesetround(rounding);
#endif
	if (inexact) {
		if (std::isfinite(r))
			r = ToDouble(ToBits(r) | 1);
	} else if (r == 0) {
		Pin(x);
		Pin(z);
		Pin(y);
		r = std::fma(x, z, y);
		Pin(r);
	}
	return r;
}


// x * z - r exactly, r being the rounded product (Dekker's product: no
// fma, which hosts without the instruction do in software; normal
// operands below 2^995, a product away from underflow; built with
// -ffp-contract=off).
inline double
ProductError(double x, double z, double r)
{
	const double kSplit = 134217729.0;	// 2^27 + 1
	double cx = kSplit * x;
	double xh = cx - (cx - x);
	double xl = x - xh;
	double cz = kSplit * z;
	double zh = cz - (cz - z);
	double zl = z - zh;
	return ((xh * zh - r) + xh * zl + xl * zh) + xl * zl;
}


// The common case of the arithmetic instructions: round to nearest, no
// exception enabled, IEEE mode, finite operands, a divisor that is not
// zero, results away from overflow and underflow. The rounding error of
// add, sub, mul and div is computed exactly (2Sum, Dekker's product), so
// they leave the host's exception flags alone; so do the multiply-adds
// whose product is exact (single precision operands: a plain multiply,
// then an add), the others read them once. Rounding to single precision
// is checked by converting back. Returns false for the general path;
// where both apply, the results and FPSCR are the same bit for bit
// (cpu-unit-tests compares them).
bool
ArithFast(PpcCpu& c, u32 i, ArithOp op, bool single)
{
	u32& fpscr = c.fFpscr;
	if ((fpscr & (VE | OE | UE | ZE | XE | NI | RN_MASK)) != 0)
		return false;
	const u64 kExponent = 0x7ff0000000000000ull;
	u64 a = F[PPC_RA(i)];
	u64 b = F[PPC_RB(i)];
	u64 cc = F[PPC_RC(i)];
	bool usesB = op != kMul;
	bool usesC = op == kMul || op >= kMadd;
	if ((a & kExponent) == kExponent
		|| (usesB && (b & kExponent) == kExponent)
		|| (usesC && (cc & kExponent) == kExponent))
		return false;
	double x = ToDouble(a);
	double y = ToDouble(b);
	double z = ToDouble(cc);
	double r;
	bool inexact = false;
	bool roundedUp = false;
	switch (op) {
		case kAdd:
		case kSub:
		{
			double yy = op == kSub ? -y : y;
			r = x + yy;
			if (!std::isfinite(r))
				return false;
			// sums are exact when tiny: no underflow
			double bv = r - x;
			double error = (x - (r - bv)) + (yy - bv);
			inexact = error != 0;
			roundedUp = inexact && ((error < 0) != (r < 0));
			break;
		}
		case kMul:
		{
			r = x * z;
			if (IsZero(a) || IsZero(cc))
				break;
			// the error is a double here
			if (!(std::fabs(r) >= 0x1p-960) || !std::isfinite(r))
				return false;
			if (SignificantBits(a) + SignificantBits(cc) <= 53)
				break;
			if ((a & kExponent) == 0 || (cc & kExponent) == 0
				|| !(std::fabs(x) < 0x1p995 && std::fabs(z) < 0x1p995))
				return false;
			double error = ProductError(x, z, r);
			inexact = error != 0;
			roundedUp = inexact && ((error < 0) != (r < 0));
			break;
		}
		case kDiv:
		{
			if (IsZero(b))
				return false;
			r = x / y;
			if (IsZero(a))
				break;
			// the remainder is a double here, Dekker's product does not
			// overflow
			double ax = std::fabs(x);
			double ay = std::fabs(y);
			double ar = std::fabs(r);
			if (!(ax >= 0x1p-900 && ax <= 0x1p900 && ay >= 0x1p-900
					&& ay <= 0x1p900 && ar >= 0x1p-900 && ar <= 0x1p900))
				return false;
			// x - p is exact (p is close to x), and so is the remainder
			double p = r * y;
			double remainder = (x - p) - ProductError(r, y, p);
			double error = remainder / y;
			inexact = error != 0;
			roundedUp = inexact && ((error < 0) != (r < 0));
			break;
		}
		default:
		{
			// (the general path does not tell FR for these either)
			double yy = op == kMadd || op == kNmadd ? y : -y;
			double p = x * z;
			if (IsZero(a) || IsZero(cc)
				|| (SignificantBits(a) + SignificantBits(cc) <= 53
					&& std::fabs(p) >= 0x1p-960)) {
				// an exact product: one rounding, in the add (sums are
				// exact when tiny)
				r = p + yy;
				if (!std::isfinite(r))
					return false;
				double bv = r - p;
				double error = (p - (r - bv)) + (yy - bv);
				inexact = error != 0;
				if (single && inexact && (ToBits(r) & 1) == 0) {
					// rounded to odd (FmaToOdd()): the neighbour on the
					// exact value's side
					u64 bits = ToBits(r);
					if ((error < 0) == (r < 0))
						bits++;
					else
						bits--;
					r = ToDouble(bits);
				}
				break;
			}
			if (single)
				return false;	// rounded to odd by the general path
			ClearHostFlags();
			Pin(x);
			Pin(z);
			Pin(yy);
			r = std::fma(x, z, yy);
			Pin(r);
			u32 hostFlags = HostFlags();
			if ((hostFlags & (kHostOverflow | kHostUnderflow)) != 0)
				return false;
			inexact = (hostFlags & kHostInexact) != 0;
			break;
		}
	}
	bool underflow = false;
	if (single) {
		float f = (float)r;
		if (std::isinf(f))
			return false;
		double back = (double)f;
		if (back != r) {
			inexact = true;
			roundedUp = std::fabs(back) > std::fabs(r);
			// tiny before rounding (the host tells after rounding)
			underflow = std::fabs(r) < 1.17549435082228750797e-38;
		}
		r = back;
	} else if (inexact && roundedUp && std::fabs(r) == 2.2250738585072014e-308)
		underflow = true;
	if (op == kNmadd || op == kNmsub)
		r = -r;

	u64 result = ToBits(r);
	F[PPC_RD(i)] = result;
	fpscr &= ~(FR | FI | FPRF_MASK);
	fpscr |= Fprf(result) << 12;
	u32 exceptions = 0;
	if (inexact) {
		fpscr |= roundedUp ? FI | FR : FI;
		exceptions |= XX;
	}
	if (underflow)
		exceptions |= UX;
	SetExceptions(c, exceptions);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
	return true;
}


// Common path of the arithmetic instructions. a, b, c are the operands as
// named by the instruction (frA, frB, frC); unused ones are 0.
void
Arith(PpcCpu& c, u32 i, ArithOp op, bool single)
{
	if (!CheckFpAvailable(c))
		return;
	if (PpcOps::sFastArithmetic && ArithFast(c, i, op, single))
		return;
	int d = PPC_RD(i);
	u64 a = F[PPC_RA(i)];
	u64 b = F[PPC_RB(i)];
	u64 cc = F[PPC_RC(i)];
	bool usesA = true;
	bool usesB = op != kMul;
	bool usesC = op == kMul || op >= kMadd;

	// Invalid operation and zero divide classification.
	u32 invalid = 0;
	if ((usesA && IsSNaN(a)) || (usesB && IsSNaN(b)) || (usesC && IsSNaN(cc)))
		invalid |= VXSNAN;
	bool anyNaN = (usesA && IsNaN(a)) || (usesB && IsNaN(b))
		|| (usesC && IsNaN(cc));
	bool zeroDivide = false;
	if (!anyNaN) {
		switch (op) {
			case kAdd:
			case kSub:
				if (IsInf(a) && IsInf(b)
					&& (Sign(a) != Sign(b)) == (op == kAdd))
					invalid |= VXISI;
				break;
			case kMul:
				if ((IsInf(a) && IsZero(cc)) || (IsZero(a) && IsInf(cc)))
					invalid |= VXIMZ;
				break;
			case kDiv:
				if (IsInf(a) && IsInf(b))
					invalid |= VXIDI;
				else if (IsZero(a) && IsZero(b))
					invalid |= VXZDZ;
				else if (IsZero(b))
					zeroDivide = true;
				break;
			default:
			{
				if ((IsInf(a) && IsZero(cc)) || (IsZero(a) && IsInf(cc))) {
					invalid |= VXIMZ;
				} else if ((IsInf(a) || IsInf(cc)) && IsInf(b)) {
					bool productNegative = Sign(a) != Sign(cc);
					bool addendNegative = Sign(b)
						!= (op == kMsub || op == kNmsub);
					if (productNegative != addendNegative)
						invalid |= VXISI;
				}
				break;
			}
		}
	}

	u32& fpscr = c.fFpscr;
	if ((invalid != 0 && (fpscr & VE) != 0)
		|| (zeroDivide && (fpscr & ZE) != 0)) {
		// Enabled exception: the target register is not altered.
		fpscr &= ~(FR | FI);
		SetExceptions(c, invalid | (zeroDivide ? (u32)ZX : 0u));
		if (PPC_RCBIT(i))
			SetCr1(c);
		CheckEnabled(c);
		return;
	}

	u64 result;
	u32 flags = 0;
	bool inexactKnown = false;
	if (anyNaN) {
		u64 nan = usesA && IsNaN(a) ? a : usesB && IsNaN(b) ? b : cc;
		result = Quiet(nan);
		if (single)
			result = SingleToDouble(DoubleToSingleStore(result));
		fpscr &= ~(FR | FI);
	} else if (invalid != 0) {
		result = kDefaultQNaN;
		fpscr &= ~(FR | FI);
	} else {
		double x = ToDouble(a);
		double y = ToDouble(b);
		double z = ToDouble(cc);
		double r;
		ClearHostFlags();
		Pin(x);
		Pin(y);
		Pin(z);
		switch (op) {
			case kAdd: r = x + y; break;
			case kSub: r = x - y; break;
			case kMul: r = x * z; break;
			case kDiv: r = x / y; break;
			// fnmadd and fnmsub round the un-negated result; the negation
			// is applied after rounding (also to single precision).
			case kMadd: case kNmadd:
				r = single ? FmaToOdd(x, z, y) : std::fma(x, z, y);
				break;
			default:
				r = single ? FmaToOdd(x, z, -y) : std::fma(x, z, -y);
				break;
		}
		Pin(r);
		// Rounding direction of the double result (fraction rounded).
		bool roundedUp = false;
		u32 hostFlags = HostFlags();
		if ((hostFlags & kHostInexact) != 0 && std::isfinite(r)) {
			double error = 0;
			switch (op) {
				case kAdd:
				case kSub:
				{
					double yy = op == kSub ? -y : y;
					double bv = r - x;
					error = (x - (r - bv)) + (yy - bv);
					break;
				}
				case kMul: error = std::fma(x, z, -r); break;
				case kDiv: error = -std::fma(r, y, -x) / y; break;
				default: break;
			}
			// error = exact - rounded; magnitude increased if the error
			// has the opposite sign of the result.
			roundedUp = error != 0 && ((error < 0) != (r < 0));
		}
		if (single) {
			ClearHostFlags();
			Pin(r);
			float f = (float)r;
			Pin(f);
			u32 singleFlags = HostFlags();
			if ((singleFlags & kHostInexact) != 0 && std::isfinite(f)) {
				double back = (double)f;
				roundedUp = std::fabs(back) > std::fabs(r);
				// The PowerPC detects tininess before rounding, the host
				// after rounding.
				if (r != 0 && std::fabs(r) < 1.17549435082228750797e-38)
					singleFlags |= kHostUnderflow;
			}
			hostFlags |= singleFlags;
			r = (double)f;
		}
		if (!single && (hostFlags & kHostInexact) != 0 && roundedUp
			&& std::fabs(r) == 2.2250738585072014e-308)
			hostFlags |= kHostUnderflow;	// tiny before rounding
		if (op == kNmadd || op == kNmsub)
			r = -r;
		flags = hostFlags;
		inexactKnown = true;
		result = ToBits(r);
		if ((fpscr & NI) != 0 && ((result >> 52) & 0x7ff) == 0
			&& !IsZero(result))
			result &= 0x8000000000000000ull;
		fpscr &= ~(FR | FI);
		if ((flags & kHostInexact) != 0) {
			fpscr |= FI;
			if (roundedUp)
				fpscr |= FR;
		}
	}

	u32 exceptions = invalid;
	if (zeroDivide)
		exceptions |= ZX;
	if (inexactKnown) {
		if ((flags & kHostOverflow) != 0)
			exceptions |= OX | XX;
		if ((flags & kHostUnderflow) != 0)
			exceptions |= UX;
		if ((flags & kHostInexact) != 0)
			exceptions |= XX;
	}
	F[d] = result;
	fpscr = (fpscr & ~FPRF_MASK) | (Fprf(result) << 12);
	SetExceptions(c, exceptions);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
}


void Op_fadd(PpcCpu& c, u32 i) { Arith(c, i, kAdd, false); }
void Op_fsub(PpcCpu& c, u32 i) { Arith(c, i, kSub, false); }
void Op_fmul(PpcCpu& c, u32 i) { Arith(c, i, kMul, false); }
void Op_fdiv(PpcCpu& c, u32 i) { Arith(c, i, kDiv, false); }
void Op_fmadd(PpcCpu& c, u32 i) { Arith(c, i, kMadd, false); }
void Op_fmsub(PpcCpu& c, u32 i) { Arith(c, i, kMsub, false); }
void Op_fnmadd(PpcCpu& c, u32 i) { Arith(c, i, kNmadd, false); }
void Op_fnmsub(PpcCpu& c, u32 i) { Arith(c, i, kNmsub, false); }
void Op_fadds(PpcCpu& c, u32 i) { Arith(c, i, kAdd, true); }
void Op_fsubs(PpcCpu& c, u32 i) { Arith(c, i, kSub, true); }
void Op_fmuls(PpcCpu& c, u32 i) { Arith(c, i, kMul, true); }
void Op_fdivs(PpcCpu& c, u32 i) { Arith(c, i, kDiv, true); }
void Op_fmadds(PpcCpu& c, u32 i) { Arith(c, i, kMadd, true); }
void Op_fmsubs(PpcCpu& c, u32 i) { Arith(c, i, kMsub, true); }
void Op_fnmadds(PpcCpu& c, u32 i) { Arith(c, i, kNmadd, true); }
void Op_fnmsubs(PpcCpu& c, u32 i) { Arith(c, i, kNmsub, true); }


// Rounds a double result to single precision with full FPSCR handling.
void
RoundToSingle(PpcCpu& c, u32 i, u64 b, bool estimate)
{
	u32& fpscr = c.fFpscr;
	int d = PPC_RD(i);
	u32 exceptions = 0;
	u64 result;
	if (IsNaN(b)) {
		if (IsSNaN(b)) {
			exceptions |= VXSNAN;
			if ((fpscr & VE) != 0) {
				fpscr &= ~(FR | FI);
				SetExceptions(c, exceptions);
				if (PPC_RCBIT(i))
					SetCr1(c);
				CheckEnabled(c);
				return;
			}
		}
		result = SingleToDouble(DoubleToSingleStore(Quiet(b)));
		fpscr &= ~(FR | FI);
	} else {
		double r = ToDouble(b);
		ClearHostFlags();
		Pin(r);
		float f = (float)r;
		Pin(f);
		u32 flags = HostFlags();
		result = ToBits((double)f);
		fpscr &= ~(FR | FI);
		if ((flags & kHostInexact) != 0 && r != 0
			&& std::fabs(r) < 1.17549435082228750797e-38)
			flags |= kHostUnderflow;	// tiny before rounding
		if ((flags & kHostInexact) != 0 && !estimate) {
			fpscr |= FI;
			if (std::isfinite(f) && std::fabs((double)f) > std::fabs(r))
				fpscr |= FR;
			exceptions |= XX;
		}
		if ((flags & kHostOverflow) != 0)
			exceptions |= OX | XX;
		if ((flags & kHostUnderflow) != 0)
			exceptions |= UX;
	}
	F[d] = result;
	fpscr = (fpscr & ~FPRF_MASK) | (Fprf(result) << 12);
	SetExceptions(c, exceptions);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
}


void
Op_frsp(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	RoundToSingle(c, i, F[PPC_RB(i)], false);
}


void
Op_fres(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u64 b = F[PPC_RB(i)];
	if (IsZero(b)) {
		u32& fpscr = c.fFpscr;
		if ((fpscr & ZE) != 0) {
			SetExceptions(c, ZX);
			if (PPC_RCBIT(i))
				SetCr1(c);
			CheckEnabled(c);
			return;
		}
		F[PPC_RD(i)] = (b & 0x8000000000000000ull) | 0x7ff0000000000000ull;
		fpscr = (fpscr & ~(FPRF_MASK | FR | FI))
			| (Fprf(F[PPC_RD(i)]) << 12);
		SetExceptions(c, ZX);
		if (PPC_RCBIT(i))
			SetCr1(c);
		CheckEnabled(c);
		return;
	}
	if (IsNaN(b)) {
		RoundToSingle(c, i, b, true);
		return;
	}
	RoundToSingle(c, i, ToBits(1.0 / ToDouble(b)), true);
}


void
Op_frsqrte(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u64 b = F[PPC_RB(i)];
	u32& fpscr = c.fFpscr;
	int d = PPC_RD(i);
	u32 exceptions = 0;
	u64 result;
	if (IsNaN(b)) {
		if (IsSNaN(b))
			exceptions |= VXSNAN;
		result = Quiet(b);
	} else if (IsZero(b)) {
		exceptions |= ZX;
		result = (b & 0x8000000000000000ull) | 0x7ff0000000000000ull;
	} else if (Sign(b)) {
		exceptions |= VXSQRT;
		result = kDefaultQNaN;
	} else {
		result = ToBits(1.0 / std::sqrt(ToDouble(b)));
	}
	if (((exceptions & kVxAll) != 0 && (fpscr & VE) != 0)
		|| ((exceptions & ZX) != 0 && (fpscr & ZE) != 0)) {
		SetExceptions(c, exceptions);
		if (PPC_RCBIT(i))
			SetCr1(c);
		CheckEnabled(c);
		return;
	}
	F[d] = result;
	fpscr = (fpscr & ~(FPRF_MASK | FR | FI)) | (Fprf(result) << 12);
	SetExceptions(c, exceptions);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
}


void
Op_fsel(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u64 a = F[PPC_RA(i)];
	bool selectC = !IsNaN(a) && (IsZero(a) || !Sign(a));
	F[PPC_RD(i)] = selectC ? F[PPC_RC(i)] : F[PPC_RB(i)];
	if (PPC_RCBIT(i))
		SetCr1(c);
}


void
Op_fsqrt_illegal(PpcCpu& c, u32 i)
{
	// fsqrt and fsqrts are not implemented by the 603.
	PpcOps::Illegal(c, i);
}


// #pragma mark - moves


template<int kOp>
void
Op_fmove(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u64 b = F[PPC_RB(i)];
	switch (kOp) {
		case 0: break;									// fmr
		case 1: b ^= 0x8000000000000000ull; break;		// fneg
		case 2: b &= ~0x8000000000000000ull; break;		// fabs
		default: b |= 0x8000000000000000ull; break;		// fnabs
	}
	F[PPC_RD(i)] = b;
	if (PPC_RCBIT(i))
		SetCr1(c);
}


// #pragma mark - conversion


void
ConvertToInteger(PpcCpu& c, u32 i, bool truncate)
{
	if (!CheckFpAvailable(c))
		return;
	u64 b = F[PPC_RB(i)];
	u32& fpscr = c.fFpscr;
	u32 exceptions = 0;
	u32 value;
	bool inexact = false;
	bool roundedUp = false;
	if (IsNaN(b)) {
		exceptions |= VXCVI | (IsSNaN(b) ? (u32)VXSNAN : 0u);
		value = 0x80000000;
	} else {
		double x = ToDouble(b);
		double rounded;
		if (truncate) {
			rounded = std::trunc(x);
		} else {
			switch (fpscr & RN_MASK) {
				case 0: rounded = std::nearbyint(x); break;	// host = nearest
				case 1: rounded = std::trunc(x); break;
				case 2: rounded = std::ceil(x); break;
				default: rounded = std::floor(x); break;
			}
			if ((fpscr & RN_MASK) == 0) {
				// nearbyint follows the host rounding mode, which is the
				// guest's; make sure it is round to nearest even here.
				double floorValue = std::floor(x);
				double diff = x - floorValue;
				if (diff > 0.5)
					rounded = floorValue + 1;
				else if (diff < 0.5)
					rounded = floorValue;
				else
					rounded = std::fmod(floorValue, 2.0) == 0
						? floorValue : floorValue + 1;
			}
		}
		if (rounded > 2147483647.0) {
			exceptions |= VXCVI;
			value = 0x7fffffff;
		} else if (rounded < -2147483648.0) {
			exceptions |= VXCVI;
			value = 0x80000000;
		} else {
			value = (u32)(s32)rounded;
			inexact = rounded != x;
			roundedUp = std::fabs(rounded) > std::fabs(x);
		}
	}
	if ((exceptions & kVxAll) != 0 && (fpscr & VE) != 0) {
		fpscr &= ~(FR | FI);
		SetExceptions(c, exceptions);
		if (PPC_RCBIT(i))
			SetCr1(c);
		CheckEnabled(c);
		return;
	}
	fpscr &= ~(FR | FI);
	if (inexact) {
		fpscr |= FI;
		if (roundedUp)
			fpscr |= FR;
		exceptions |= XX;
	}
	F[PPC_RD(i)] = 0xfff8000000000000ull | value;
	SetExceptions(c, exceptions);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
}


void Op_fctiw(PpcCpu& c, u32 i) { ConvertToInteger(c, i, false); }
void Op_fctiwz(PpcCpu& c, u32 i) { ConvertToInteger(c, i, true); }


// #pragma mark - compare


void
Compare(PpcCpu& c, u32 i, bool ordered)
{
	if (!CheckFpAvailable(c))
		return;
	u64 a = F[PPC_RA(i)];
	u64 b = F[PPC_RB(i)];
	u32 field;
	u32 exceptions = 0;
	if (IsNaN(a) || IsNaN(b)) {
		field = 1;	// unordered
		bool snan = IsSNaN(a) || IsSNaN(b);
		if (snan)
			exceptions |= VXSNAN;
		if (ordered) {
			if (!snan || (c.fFpscr & VE) == 0)
				exceptions |= VXVC;
		}
	} else {
		double x = ToDouble(a);
		double y = ToDouble(b);
		field = x < y ? 8 : x > y ? 4 : 2;
	}
	c.fFpscr = (c.fFpscr & ~FPCC_MASK) | (field << 12);
	PpcSetCrField(c, (i >> 23) & 7, field);
	SetExceptions(c, exceptions);
	CheckEnabled(c);
}


void Op_fcmpu(PpcCpu& c, u32 i) { Compare(c, i, false); }
void Op_fcmpo(PpcCpu& c, u32 i) { Compare(c, i, true); }


// #pragma mark - FPSCR


void
FpscrChanged(PpcCpu& c)
{
	PpcOps::UpdateFpscrSummary(c);
	PpcOps::SetHostRounding(c.fFpscr);
}


void
Op_mffs(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	F[PPC_RD(i)] = 0xfff8000000000000ull | c.fFpscr;
	if (PPC_RCBIT(i))
		SetCr1(c);
}


// Writes FPSCR bits under 'mask'. FEX and VX are always computed and the
// reserved bit 20 stays clear. mtfsb1 also sets FX when an exception bit
// goes from 0 to 1; mtfsf and mtfsfi only change FX when they write it.
void
WriteFpscr(PpcCpu& c, u32 value, u32 mask, bool fxRule = false)
{
	mask &= ~(FEX | VX | 0x800u);
	u32 old = c.fFpscr;
	u32 updated = (old & ~mask) | (value & mask);
	if (fxRule && ((updated & ~old) & kExceptionBits) != 0)
		updated |= FX;
	c.fFpscr = updated;
	FpscrChanged(c);
}


void
Op_mtfsf(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u32 fm = (i >> 17) & 0xff;
	u32 mask = 0;
	for (int field = 0; field < 8; field++) {
		if ((fm & (0x80 >> field)) != 0)
			mask |= 0xf0000000u >> (4 * field);
	}
	WriteFpscr(c, (u32)F[PPC_RB(i)], mask);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
}


void
Op_mtfsfi(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	int field = (i >> 23) & 7;
	u32 shift = 28 - 4 * field;
	WriteFpscr(c, ((i >> 12) & 0xf) << shift, 0xfu << shift);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
}


void
Op_mtfsb0(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u32 bit = 0x80000000u >> ((i >> 21) & 31);
	WriteFpscr(c, 0, bit);
	if (PPC_RCBIT(i))
		SetCr1(c);
}


void
Op_mtfsb1(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u32 bit = 0x80000000u >> ((i >> 21) & 31);
	WriteFpscr(c, bit, bit, true);
	if (PPC_RCBIT(i))
		SetCr1(c);
	CheckEnabled(c);
}


void
Op_mcrfs(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	int source = (i >> 18) & 7;
	u32 shift = 28 - 4 * source;
	PpcSetCrField(c, (i >> 23) & 7, (c.fFpscr >> shift) & 0xf);
	// The exception bits of the copied field are cleared.
	u32 clear = (0xfu << shift) & (FX | kExceptionBits);
	c.fFpscr &= ~clear;
	FpscrChanged(c);
}


// #pragma mark - loads and stores


inline u32
EaD(PpcCpu& c, u32 i)
{
	return (PPC_RA(i) != 0 ? G[PPC_RA(i)] : 0) + PPC_SIMM(i);
}


inline u32
EaX(PpcCpu& c, u32 i)
{
	return (PPC_RA(i) != 0 ? G[PPC_RA(i)] : 0) + G[PPC_RB(i)];
}


template<bool kIndexed, bool kUpdate, bool kDouble>
void
Op_lf(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u32 address = kIndexed ? EaX(c, i) : EaD(c, i);
	if ((address & 3) != 0) {
		c.RaiseAlignment(address, i);
		return;
	}
	u64 value;
	if (kDouble) {
		if (!c.Read64(address, value))
			return;
	} else {
		u32 word;
		if (!c.Read32(address, word))
			return;
		value = SingleToDouble(word);
	}
	F[PPC_RD(i)] = value;
	if (kUpdate)
		G[PPC_RA(i)] = address;
}


template<bool kIndexed, bool kUpdate, bool kDouble>
void
Op_stf(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u32 address = kIndexed ? EaX(c, i) : EaD(c, i);
	if ((address & 3) != 0) {
		c.RaiseAlignment(address, i);
		return;
	}
	u64 value = F[PPC_RS(i)];
	bool ok = kDouble ? c.Write64(address, value)
		: c.Write32(address, DoubleToSingleStore(value));
	if (ok && kUpdate)
		G[PPC_RA(i)] = address;
}


void
Op_stfiwx(PpcCpu& c, u32 i)
{
	if (!CheckFpAvailable(c))
		return;
	u32 address = EaX(c, i);
	if ((address & 3) != 0) {
		c.RaiseAlignment(address, i);
		return;
	}
	c.Write32(address, (u32)F[PPC_RS(i)]);
}

}	// namespace


bool PpcOps::sFastArithmetic = true;


void
PpcOps::UpdateFpscrSummary(PpcCpu& c)
{
	u32 fpscr = c.fFpscr & ~(VX | FEX);
	if ((fpscr & kVxAll) != 0)
		fpscr |= VX;
	if (((fpscr & VX) && (fpscr & VE)) || ((fpscr & OX) && (fpscr & OE))
		|| ((fpscr & UX) && (fpscr & UE)) || ((fpscr & ZX) && (fpscr & ZE))
		|| ((fpscr & XX) && (fpscr & XE)))
		fpscr |= FEX;
	c.fFpscr = fpscr;
}


void
PpcOps::SetHostRounding(u32 fpscr)
{
#ifdef BEBOX_HAVE_MXCSR
	// PowerPC RN: 0 nearest, 1 zero, 2 +inf, 3 -inf.
	// MXCSR RC:   0 nearest, 1 -inf, 2 +inf, 3 zero.
	static const u32 kRounding[4] = { 0, 3, 2, 1 };
	u32 csr = _mm_getcsr() & ~(3u << 13);
	csr |= kRounding[fpscr & 3] << 13;
	_mm_setcsr(csr);
#else
	static const int kRounding[4] = { FE_TONEAREST, FE_TOWARDZERO,
		FE_UPWARD, FE_DOWNWARD };
	fesetround(kRounding[fpscr & 3]);
#endif
}


void
PpcOps::InitFpuOps()
{
	PpcHandler* p = sPrimary;
	p[48] = Op_lf<false, false, false>;		// lfs
	p[49] = Op_lf<false, true, false>;		// lfsu
	p[50] = Op_lf<false, false, true>;		// lfd
	p[51] = Op_lf<false, true, true>;		// lfdu
	p[52] = Op_stf<false, false, false>;	// stfs
	p[53] = Op_stf<false, true, false>;		// stfsu
	p[54] = Op_stf<false, false, true>;		// stfd
	p[55] = Op_stf<false, true, true>;		// stfdu

	PpcHandler* t = sOp31;
	t[535] = Op_lf<true, false, false>;		// lfsx
	t[567] = Op_lf<true, true, false>;		// lfsux
	t[599] = Op_lf<true, false, true>;		// lfdx
	t[631] = Op_lf<true, true, true>;		// lfdux
	t[663] = Op_stf<true, false, false>;	// stfsx
	t[695] = Op_stf<true, true, false>;		// stfsux
	t[727] = Op_stf<true, false, true>;		// stfdx
	t[759] = Op_stf<true, true, true>;		// stfdux
	t[983] = Op_stfiwx;

	// A form: the extended opcode is in bits 26-30 and frC sits in bits
	// 21-25, so every value of the upper five bits maps to the handler.
	struct { int xo; PpcHandler single; PpcHandler dbl; } aForms[] = {
		{ 18, Op_fdivs, Op_fdiv },
		{ 20, Op_fsubs, Op_fsub },
		{ 21, Op_fadds, Op_fadd },
		{ 22, Op_fsqrt_illegal, Op_fsqrt_illegal },
		{ 25, Op_fmuls, Op_fmul },
		{ 28, Op_fmsubs, Op_fmsub },
		{ 29, Op_fmadds, Op_fmadd },
		{ 30, Op_fnmsubs, Op_fnmsub },
		{ 31, Op_fnmadds, Op_fnmadd },
	};
	for (const auto& form : aForms) {
		for (int high = 0; high < 32; high++) {
			sOp59[(high << 5) | form.xo] = form.single;
			sOp63[(high << 5) | form.xo] = form.dbl;
		}
	}
	for (int high = 0; high < 32; high++) {
		sOp59[(high << 5) | 24] = Op_fres;
		sOp63[(high << 5) | 23] = Op_fsel;
		sOp63[(high << 5) | 26] = Op_frsqrte;
	}

	t = sOp63;
	t[0] = Op_fcmpu;
	t[12] = Op_frsp;
	t[14] = Op_fctiw;
	t[15] = Op_fctiwz;
	t[32] = Op_fcmpo;
	t[38] = Op_mtfsb1;
	t[40] = Op_fmove<1>;	// fneg
	t[64] = Op_mcrfs;
	t[70] = Op_mtfsb0;
	t[72] = Op_fmove<0>;	// fmr
	t[134] = Op_mtfsfi;
	t[136] = Op_fmove<3>;	// fnabs
	t[264] = Op_fmove<2>;	// fabs
	t[583] = Op_mffs;
	t[711] = Op_mtfsf;
}

}	// namespace bebox
