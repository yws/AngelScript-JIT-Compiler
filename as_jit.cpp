#include "as_jit.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <map>
#include <functional>
#include <cstdint>

#include "../source/as_scriptfunction.h"
#include "../source/as_objecttype.h"
#include "../source/as_callfunc.h"
#include "../source/as_scriptengine.h"
#include "../source/as_scriptobject.h"
#include "../source/as_texts.h"

#include "virtual_asm.h"
using namespace assembler;

CriticalSection jitLock;

#ifdef __amd64__
#define stdcall
#define JIT_64
#endif

#ifdef _M_AMD64
#define JIT_64
#endif

#ifdef JIT_64
#define stdcall
#else
#ifdef _MSC_VER
#define stdcall __stdcall
#else
#define stdcall __attribute__((stdcall))
#endif
#endif

//#define JIT_PRINT_UNHANDLED_CALLS
#ifdef JIT_PRINT_UNHANDLED_CALLS
#include <string>
#include <set>

static std::set<asCScriptFunction*> unhandledCalls;
#endif

const unsigned codePageSize = 65535 * 4;

#define offset0 (asBC_SWORDARG0(pOp)*sizeof(asDWORD))
#define offset1 (asBC_SWORDARG1(pOp)*sizeof(asDWORD))
#define offset2 (asBC_SWORDARG2(pOp)*sizeof(asDWORD))

//#define JIT_DEBUG
#ifdef JIT_DEBUG
static asEBCInstr DBG_CurrentOP;
static asEBCInstr DBG_LastOP;
static void* DBG_Entry = 0;
static void* DBG_Instr = 0;
static void* DBG_LastInstr = 0;
static void* DBG_LastCall = 0;
static void* DBG_FuncEntry = 0;
static asCScriptFunction* DBG_CurrentFunction;
#endif

short offset(asDWORD* op, unsigned n) {
	return *(((short*)op) + (n+1)) * sizeof(asDWORD);
}

//Returns true if the op will clear the temporary var
// Used to determine if we need to perform a full test in a Test-Jump pair
bool clearsTemporary(asEBCInstr op) {
	switch(op) {
		case asBC_TZ:
		case asBC_TNZ:
		case asBC_TS:
		case asBC_TNS:
		case asBC_TP:
		case asBC_TNP:

		case asBC_CMPd:
		case asBC_CMPu:
		case asBC_CMPf:
		case asBC_CMPi:
		case asBC_CMPIi:
		case asBC_CMPIf:
		case asBC_CMPIu:

		case asBC_CmpPtr:

		case asBC_CpyVtoR4:
		case asBC_CpyVtoR8:

		case asBC_CMPi64:
		case asBC_CMPu64:
			return true;
	}

	return false;
}

//Wrappers so we can deal with complex pointers/calling conventions

void stdcall allocScriptObject(asCObjectType* type, asCScriptFunction* constructor, asIScriptEngine* engine, asSVMRegisters* registers);

void* stdcall engineAlloc(asCScriptEngine* engine, asCObjectType* type);

void stdcall engineRelease(asCScriptEngine* engine, void* memory, asCScriptFunction* release);

void stdcall engineDestroyFree(asCScriptEngine* engine, void* memory, asCScriptFunction* destruct);

void stdcall engineFree(asCScriptEngine* engine, void* memory);

void stdcall engineCallMethod(asCScriptEngine* engine, void* object, asCScriptFunction* method);

void stdcall callScriptFunction(asIScriptContext* ctx, asCScriptFunction* func);

asCScriptFunction* stdcall callInterfaceMethod(asIScriptContext* ctx, asCScriptFunction* func);

asCScriptFunction* stdcall callBoundFunction(asIScriptContext* ctx, unsigned short fid);

void stdcall receiveAutoObjectHandle(asIScriptContext* ctx, asCScriptObject* obj);

asCScriptObject* stdcall castObject(asCScriptObject* obj, asCObjectType* to);

bool stdcall doSuspend(asIScriptContext* ctx);

void stdcall returnScriptFunction(asCContext* ctx);

//Wrapper functions to cast between types, or perform math on large types, where doing so is overly complicated in the ASM
#ifdef _MSC_VER
template<class F, class T>
void stdcall directConvert(F* from, T* to) {
	*to = (T)*from;
}
#else
//stdcall doesn't work with templates on GCC
template<class F, class T>
void directConvert(F* from, T* to) {
	*to = (T)*from;
}
#endif

float stdcall fmod_wrapper_f(float* div, float* mod) {
	return fmod(*div, *mod);
}

double stdcall fmod_wrapper(double* div, double* mod) {
	return fmod(*div, *mod);
}

void stdcall i64_add(long long* a, long long* b, long long* r) {
	*r = *a + *b;
}

void stdcall i64_sub(long long* a, long long* b, long long* r) {
	*r = *a - *b;
}

void stdcall i64_mul(long long* a, long long* b, long long* r) {
	*r = *a * *b;
}

void stdcall i64_div(long long* a, long long* b, long long* r) {
	*r = *a / *b;
}

void stdcall i64_mod(long long* a, long long* b, long long* r) {
	*r = *a % *b;
}

void stdcall i64_sll(unsigned long long* a, asDWORD* b, unsigned long long* r) {
	*r = *a << *b;
}

void stdcall i64_srl(unsigned long long* a, asDWORD* b, unsigned long long* r) {
	*r = *a << *b;
}

void stdcall i64_sra(long long* a, asDWORD* b, long long* r) {
	*r = *a << *b;
}

int stdcall cmp_int64(long long* a, long long* b) {
	long long c = *a - *b;
	if( c == 0)
		return 0;
	else if( c < 0)
		return -1;
	else
		return 1;
}

int stdcall cmp_uint64(unsigned long long* a, unsigned long long* b) {
	unsigned long long c = *a - *b;
	if( c == 0)
		return 0;
	else if( c < 0)
		return -1;
	else
		return 1;
}

size_t stdcall div_ull(unsigned long long* div, unsigned long long* by, unsigned long long* result) {
	if(*by == 0)
		return 1;
	*result = *div / *by;
	return 0;
}

size_t stdcall mod_ull(unsigned long long* div, unsigned long long* by, unsigned long long* result) {
	if(*by == 0)
		return 1;
	*result = *div % *by;
	return 0;
}

enum ObjectPosition {
	OP_This,
	OP_First,
	OP_Last,
	OP_None
};

enum EAXContains {
	EAX_Unknown,
	EAX_Stack,
	EAX_Offset,
};

struct SystemCall {
	Processor& cpu;
	FloatingPointUnit& fpu;
	asDWORD* const & pOp;
	unsigned flags;
	std::function<void(JumpType)> returnHandler;

	SystemCall(Processor& CPU, FloatingPointUnit& FPU,
		std::function<void(JumpType)> ConditionalReturn, asDWORD* const & bytecode, unsigned Flags)
		: cpu(CPU), fpu(FPU), returnHandler(ConditionalReturn), pOp(bytecode), flags(Flags) {}

	void callSystemFunction(asCScriptFunction* func, Register* objPointer = 0);

private:
	void call_viaAS(asCScriptFunction* func, Register* objPointer);
	void call_stdcall(asSSystemFunctionInterface* func, asCScriptFunction* sFunc);
	void call_cdecl(asSSystemFunctionInterface* func, asCScriptFunction* sFunc);
	void call_cdecl_obj(asSSystemFunctionInterface* func, asCScriptFunction* sFunc, Register* objPointer, bool last);
	void call_thiscall(asSSystemFunctionInterface* func, asCScriptFunction* sFunc, Register* objPointer);

	void call_64conv(asSSystemFunctionInterface* func, asCScriptFunction* sFunc, Register* objPointer, ObjectPosition pos);
	
	void call_getReturn(asSSystemFunctionInterface* func, asCScriptFunction* sFunc);
	
	//Handles error handling
	void call_entry(asSSystemFunctionInterface* func, asCScriptFunction* sFunc);
	void call_error();
	void call_exit(asSSystemFunctionInterface* func);
};

unsigned toSize(asEBCInstr instr) {
	return asBCTypeSize[asBCInfo[instr].type];
}

asCJITCompiler::asCJITCompiler(unsigned Flags) : flags(Flags), activePage(0) {
}

//Returns the total number of bytes that will be pushed, until the next op that doesn't push
unsigned findTotalPushBatchSize(asDWORD* firstPush, asDWORD* endOfBytecode);

//Offsets on the stack for function local variables
namespace local {
//Used in alloc
const unsigned allocMem = 2 * sizeof(void*);
//Used in function calls
const unsigned pIsSystem = sizeof(void*);
const unsigned retPointer = 0;
//Use in REFCPY
const unsigned object1 = 0;
const unsigned object2 = sizeof(void*);
};

const unsigned functionReserveSpace = 3 * sizeof(void*);

int asCJITCompiler::CompileFunction(asIScriptFunction *function, asJITFunction *output) {
	asUINT   length;
	asDWORD *pOp = function->GetByteCode(&length);

	//No bytecode for this function, don't bother making any jit for it
	if(pOp == 0 || length == 0) {
		output = 0;
		return 1;
	}

	asDWORD *end = pOp + length, *start = pOp;

	volatile byte** jumpTable = new volatile byte*[length];
	memset(jumpTable,0,length * sizeof(void*));
	bool tableInUse = false;

	std::multimap<asDWORD*, void*> futureJumps;

	jitLock.enter();

	//Get the active page, or create a new one if the current one is missing or too small (256 bytes for the entry and a few ops)
	if(activePage == 0 || activePage->final || activePage->getFreeSize() < 256)
		activePage = new CodePage(codePageSize, reinterpret_cast<void*>(&toSize));
	else
		activePage->grab();
	activePage->grab();

	void* curJitFunction = activePage->getFunctionPointer<void*>();
	void* firstJitEntry = 0;
	*output = activePage->getFunctionPointer<asJITFunction>();
	pages.insert(std::pair<asJITFunction,assembler::CodePage*>(*output,activePage));

	//If we are outside of opcodes we can execute, ignore all ops until a new JIT entry is found
	bool waitingForEntry = true;

	//Special case for a common op-pairing (*esi = eax; eax = *esi;)
	int currentEAX = EAX_Unknown, nextEAX = EAX_Unknown;

	//Setup the processor as a 32 bit processor, as most angelscript ops work on integers
	Processor cpu(*activePage, 32);
	byte* byteStart = (byte*)cpu.op;

	FloatingPointUnit fpu(cpu);

	unsigned pBits = sizeof(void*) * 8;

#ifdef JIT_64
	//32-bit registers
	Register eax(cpu,EAX), ebx(cpu,EBX), ecx(cpu,ECX), edx(cpu,EDX), ebp(cpu,EBP,pBits), edi(cpu,R12);
	//8-bit registers
	Register al(cpu,EAX,8), bl(cpu,EBX,8), cl(cpu,ECX,8), dl(cpu,EDX,8);
	//Pointer-sized registers
	Register pax(cpu,EAX,pBits), pbx(cpu,EBX,pBits), pcx(cpu,ECX,pBits), pdx(cpu,EDX,pBits), esp(cpu,ESP,pBits),
		pdi(cpu, R12, pBits), esi(cpu, R13, pBits);
	Register rarg(cpu, R10, pBits);

	//Don't use EDI and ESI, they're used for integer
	//arguments to functions, despite being nonvolatile
#else
	//32-bit registers
	Register eax(cpu,EAX), ebx(cpu,EBX), ecx(cpu,ECX), edx(cpu,EDX), ebp(cpu,EBP,pBits), edi(cpu,EDI);
	//8-bit registers
	Register al(cpu,EAX,8), bl(cpu,EBX,8), cl(cpu,ECX,8), dl(cpu,EDX,8);
	//Pointer-sized registers
	Register pax(cpu,EAX,pBits), pbx(cpu,EBX,pBits), pcx(cpu,ECX,pBits), pdx(cpu,EDX,pBits), esp(cpu,ESP,pBits),
		pdi(cpu, EDI, pBits), esi(cpu, ESI, pBits);
	Register rarg(cpu, EDX, pBits);
#endif

	//JIT FUNCTION ENTRY
	//==================
	//Push unmutable registers (these registers must retain their value after we leave our function)
	cpu.push(esi);
	cpu.push(edi);
	cpu.push(ebx);
	cpu.push(ebp);

	//Reserve two pointers for various things
	esp -= functionReserveSpace;
	cpu.stackDepth += (cpu.pushSize() * 4) + functionReserveSpace;

#ifdef JIT_DEBUG
	pbx = (void*)&DBG_FuncEntry;
	as<void*>(*pbx) = pax;
#endif

	//Function initialization {
#ifdef JIT_64
	ebp = cpu.intArg64(0, 0);
	pax = cpu.intArg64(1, 1);
#else
	ebp = as<void*>(*esp+cpu.stackDepth); //Register pointer
	pax = as<void*>(*esp+cpu.stackDepth+cpu.pushSize()); //Entry jump pointer
#endif

#ifdef JIT_DEBUG
	pbx = (void*)&DBG_CurrentFunction;
	as<void*>(*pbx) = (void*)function;
	pbx = (void*)&DBG_Entry;
	as<void*>(*pbx) = pax;
#endif

	pdi = as<void*>(*ebp+offsetof(asSVMRegisters,stackFramePointer)); //VM Frame pointer
	esi = as<void*>(*ebp+offsetof(asSVMRegisters,stackPointer)); //VM Stack pointer
	pbx = as<void*>(*ebp+offsetof(asSVMRegisters,valueRegister)); //VM Temporary
	//}

	//Jump to the section of the function we'll actually be executing this time
	cpu.jump(pax);

	//Function return {
	volatile byte* ret_pos = cpu.op;
	
	as<void*>(*ebp+offsetof(asSVMRegisters,programPointer)) = rarg; //Set the bytecode pointer based on our exit
	as<void*>(*ebp+offsetof(asSVMRegisters,stackFramePointer)) = pdi; //Return the frame pointer
	as<void*>(*ebp+offsetof(asSVMRegisters,stackPointer)) = esi; //Return the stack pointer
	as<void*>(*ebp+offsetof(asSVMRegisters,valueRegister)) = pbx; //Return the temporary
	
	//Pop reserved pointers and saved pointers
	esp += functionReserveSpace;
	cpu.pop(ebp);
	cpu.pop(ebx);
	cpu.pop(edi);
	cpu.pop(esi);
	cpu.ret();
	//}

	auto Return = [&](bool expected) {
		//Set EDX to the bytecode pointer so the vm can be returned to the correct state
		rarg = (void*)pOp;
		cpu.jump(Jump,ret_pos);
		waitingForEntry = expected;
	};

	auto ReturnCondition = [&](JumpType condition) {
		rarg = (void*)pOp;
		cpu.jump(condition,ret_pos);
	};

	SystemCall sysCall(cpu, fpu, ReturnCondition, pOp, flags);

	volatile byte* script_ret = 0;
	auto ReturnFromScriptCall = [&]() {
		if(script_ret) {
			cpu.jump(Jump,script_ret);
		}
		else {
			script_ret = cpu.op;
			//The VM Registers are already in the correct state, so just do a simple return here
			esp += functionReserveSpace;
			cpu.pop(ebp);
			cpu.pop(ebx);
			cpu.pop(edi);
			cpu.pop(esi);
			cpu.ret();
		}
		waitingForEntry = true;
	};

	auto PrepareJitScriptCall = [&](asCScriptFunction* func) -> bool {
		asDWORD* bc = func->byteCode.AddressOf();

#ifdef JIT_64
		Register arg0 = cpu.intArg64(0, 0, pax);
#else
		Register arg0 = pax;
#endif
		arg0 = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));

		//Prepare the vm state
		cpu.call_stdcall((void*)callScriptFunction,"rp", &arg0, func);
		if(flags & JIT_NO_SCRIPT_CALLS)
			return false;
		return *(asBYTE*)bc == asBC_JitEntry;
	};

	auto JitScriptCall = [&](asCScriptFunction* func) {
		//Call the first jit entry in the target function
		asDWORD* bc = func->byteCode.AddressOf();
#ifdef JIT_64
		Register arg0 = as<void*>(cpu.intArg64(0, 0));
		Register arg1 = as<void*>(cpu.intArg64(1, 1));
		Register ptr = pax;
#else
		Register arg0 = ecx;
		Register arg1 = ebx;
		Register ptr = pax;
#endif
		arg0 = as<void*>(ebp);

		asPWORD entryPoint = asBC_PTRARG(bc);
		if(entryPoint && func->jitFunction) {
			arg1 = (void*)entryPoint;
			ptr = (void*)func->jitFunction;
		}
		else {
			DeferredCodePointer def;
			def.jitEntry = (void**)arg1.setDeferred();
			def.jitFunction = (void**)ptr.setDeferred();

			deferredPointers.insert(std::pair<asIScriptFunction*,DeferredCodePointer>(func,def));
		}

		unsigned sb = cpu.call_cdecl_args("rr", &arg0, &arg1);
		cpu.call(ptr);
		cpu.call_cdecl_end(sb);
	};

	auto DynamicJitScriptCall = [&]() {
		//Expects the asCScriptFunction* to be in eax
#ifdef JIT_64
		Register arg0 = as<void*>(cpu.intArg64(0, 0));
		Register arg1 = as<void*>(cpu.intArg64(1, 1));
		Register ptr = pax;
#else
		Register arg0 = ecx;
		Register arg1 = ebx;
		Register ptr = pax;
#endif
		arg0 = as<void*>(ebp);

		//Read the first pointer from where byteCode is, which is the
		//array pointer from asCArray, skip the asBC_JitEntry byte and
		//then read the first entry pointer
		arg1 = as<void*>(*pax + offsetof(asCScriptFunction, byteCode));
		arg1 = as<void*>(*arg1 + sizeof(asDWORD));

		//Read the jit function pointer from the asCScriptFunction
		ptr = as<void*>(*pax + offsetof(asCScriptFunction, jitFunction));

		unsigned sb = cpu.call_cdecl_args("rr", &arg0, &arg1);
		cpu.call(ptr);
		cpu.call_cdecl_end(sb);
	};

	auto JitScriptCallIntf = [&](asCScriptFunction* func) {
#ifdef JIT_64
		Register arg0 = as<void*>(cpu.intArg64(0, 0));
#else
		Register arg0 = ecx;
#endif
		arg0 = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));

		//Prepare the vm state
		cpu.call_stdcall((void*)callInterfaceMethod,"rp", &arg0, func);
		//This returns the asCScriptFunction* in pax

		DynamicJitScriptCall();
	};

	auto JitScriptCallBnd = [&](int fid) {
#ifdef JIT_64
		Register arg0 = as<void*>(cpu.intArg64(0, 0));
#else
		Register arg0 = ecx;
#endif
		arg0 = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));

		//Prepare the vm state
		cpu.call_stdcall((void*)callBoundFunction,"rc", &arg0, (unsigned)fid);
		//This returns the asCScriptFunction* in pax

		pax &= pax;
		auto okay = cpu.prep_short_jump(NotZero);
		Return(false);
		cpu.end_short_jump(okay);

		DynamicJitScriptCall();
	};

	auto ReturnFromJittedScriptCall = [&](void* expectedPC) {
		//Check if we need to return to the vm
		// If the program pointer is what we expect, we don't need to return
		pcx = (void*)(expectedPC == 0 ? pOp+2 : expectedPC);
		pcx == as<void*>(*ebp + offsetof(asSVMRegisters,programPointer));

		auto skip_ret = cpu.prep_short_jump(Equal);
		ReturnFromScriptCall();
		cpu.end_short_jump(skip_ret);

		// If execution is finished, return to the vm as well so it can clean up
		as<asEContextState>(ecx) = asEXECUTION_FINISHED;
		pax = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));
		as<asEContextState>(ecx) == as<asEContextState>(*pax + offsetof(asCContext, m_status));

		auto skip_finish = cpu.prep_short_jump(NotEqual);
		ReturnFromScriptCall();
		cpu.end_short_jump(skip_finish);

		esi = as<void*>(*ebp+offsetof(asSVMRegisters,stackPointer)); //update stack pointer
		pbx = as<void*>(*ebp+offsetof(asSVMRegisters,valueRegister)); //update value register
	};

	auto do_jump = [&](JumpType type) {
		asDWORD* bc = pOp + asBC_INTARG(pOp) + 2;
		auto& jmp = jumpTable[bc - start];
		if(jmp != 0) {
			//Jump to code that already exists
			cpu.jump(type, jmp);
		}
		else if(bc > pOp) {
			//Prep the jump for a future instruction
			futureJumps.insert(std::pair<asDWORD*,void*>(bc,cpu.prep_long_jump(type)));
		}
		else {
			//We can't handle this address, so generate a special return that does the jump ahead of time
			rarg = bc;
			cpu.jump(type, ret_pos);
		}
	};

	auto do_jump_from = [&](JumpType type, asDWORD* op) {
		asDWORD* bc = op + asBC_INTARG(op) + 2;
		auto& jmp = jumpTable[bc - start];
		if(jmp != 0) {
			//Jump to code that already exists
			cpu.jump(type, jmp);
		}
		else if(bc > op) {
			//Prep the jump for a future instruction
			futureJumps.insert(std::pair<asDWORD*,void*>(bc,cpu.prep_long_jump(type)));
		}
		else {
			//We can't handle this address, so generate a special return that does the jump ahead of time
			rarg = bc;
			cpu.jump(type, ret_pos);
		}
	};

	auto check_space = [&](unsigned bytes) {
		unsigned remaining = activePage->getFreeSize() - (cpu.op - byteStart);
		if(remaining < bytes) {
			CodePage* newPage = new CodePage(codePageSize, reinterpret_cast<void*>(&toSize));

			cpu.migrate(*activePage, *newPage);

			activePage->drop();
			activePage = newPage;
			activePage->grab();

			pages.insert(std::pair<asJITFunction,assembler::CodePage*>(*output,activePage));
			byteStart = (byte*)cpu.op;
		}
	};

	unsigned reservedPushBytes = 0;
	asEBCInstr op;
	volatile void* lastop = 0;

	while(pOp < end) {
		currentEAX = nextEAX;
		nextEAX = EAX_Unknown;

		if(cpu.op > activePage->getActivePage() + activePage->getFreeSize())
			throw "Page exceeded...";

		op = asEBCInstr(*(asBYTE*)pOp);
		auto firstJump = futureJumps.lower_bound(pOp), lastJump = futureJumps.upper_bound(pOp);

		if(waitingForEntry && op != asBC_JitEntry) {
			check_space(futureJumps.size() * (2 + sizeof(void*)*2));

			//Handle cases where we jump to code we can't directly handle
			if(firstJump != futureJumps.end() && firstJump->first == pOp) {
				for(auto i = firstJump; i != lastJump; ++i)
					cpu.end_long_jump(i->second);
				futureJumps.erase(firstJump, lastJump);
				check_space(32);
				Return(true);
			}

			pOp += toSize(op);
			continue;
		}
		
		//Check for remaining space of at least 64 bytes (roughly 3 max-sized ops)
		// Do so before building jumps to save a jump when crossing pages
#ifdef JIT_DEBUG
		check_space(128);
#else
		check_space(64);
#endif

		jumpTable[pOp - start] = cpu.op;

#ifdef JIT_DEBUG
		void* beg = (void*)cpu.op;
		pdx = (void*)&DBG_CurrentOP;
		as<asEBCInstr>(*pdx) = op;
		pdx = (void*)&DBG_LastInstr;
		*pdx = (void*)lastop;
		pdx = (void*)&DBG_Instr;
		*pdx = (void*)beg;
		lastop = beg;
#endif

		//Handle jumps to code we hadn't made yet
		if(firstJump != futureJumps.end() && firstJump->first == pOp) {
			for(auto i = firstJump; i != lastJump; ++i)
				cpu.end_long_jump(i->second);
			futureJumps.erase(firstJump, lastJump);
		}

		//Multi-op optimization - special cases where specific sets of ops serve a common purpose
		auto pNextOp = pOp + toSize(op);

		if(pNextOp < end) {
			auto nextOp = asEBCInstr(*(asBYTE*)pNextOp);

			auto pThirdOp = pNextOp + toSize(nextOp);
			auto thirdOp = asBC_MAXBYTECODE;
			if(pThirdOp < end) {
				thirdOp = asEBCInstr(*(asBYTE*)pThirdOp);

				switch(op) {
				case asBC_SetV8:
					if(thirdOp == asBC_CpyVtoV8 &&
						(nextOp == asBC_ADDd || nextOp == asBC_DIVd ||
						 nextOp == asBC_SUBd || nextOp == asBC_MULd)) {
						if(asBC_SWORDARG0(pOp) != asBC_SWORDARG2(pNextOp) || asBC_SWORDARG0(pOp) != asBC_SWORDARG0(pNextOp))
							break;

						//Optimize <Variable Double> <op>= <Constant Double>
						fpu.load_double(*edi-offset(pNextOp,1));

						MemAddress doubleConstant(cpu, &asBC_QWORDARG(pOp));

						switch(nextOp) {
						case asBC_ADDd:
							fpu.add_double(doubleConstant); break;
						case asBC_SUBd:
							fpu.sub_double(doubleConstant); break;
						case asBC_MULd:
							fpu.mult_double(doubleConstant); break;
						case asBC_DIVd:
							fpu.div_double(doubleConstant); break;
						}

						if(asBC_SWORDARG0(pOp) == asBC_SWORDARG1(pThirdOp)) {
							fpu.store_double(*edi-offset(pOp,0),false);
							fpu.store_double(*edi-offset(pThirdOp,0));
						
							pOp = pThirdOp + toSize(thirdOp);
						}
						else {
							fpu.store_double(*edi-offset(pOp,0));
						
							pOp = pThirdOp;
						}
					
						continue;
					}
					break;
				case asBC_SetV4:
					if(nextOp == asBC_SetV4 && thirdOp == asBC_SetV4 && asBC_DWORDARG(pOp) == asBC_DWORDARG(pNextOp) && asBC_DWORDARG(pNextOp) == asBC_DWORDARG(pThirdOp)) {
						//Optimize intializing 3 variables to the same value (often 0)
						if(asBC_DWORDARG(pOp) == 0)
							eax ^= eax;
						else
							eax = asBC_DWORDARG(pOp);
						*edi-offset(pOp,0) = eax;
						*edi-offset(pNextOp,0) = eax;
						*edi-offset(pThirdOp,0) = eax;

						pOp = pThirdOp + toSize(thirdOp);
						continue;
					}
					break;
				case asBC_PshVPtr:
					//Optimize PshVPtr, ADDSi, RDSPtr to avoid many interim ops
					if(nextOp == asBC_ADDSi && thirdOp == asBC_RDSPtr) {
						pax = as<void*>(*edi-offset0);
						if(reservedPushBytes != 0)
							reservedPushBytes = 0;
						else
							esi -= sizeof(void*);

						pax &= pax;
						auto notNull = cpu.prep_short_jump(NotZero);
							as<void*>(*esi) = pax;
							Return(false);
						cpu.end_short_jump(notNull);

						pax = as<void*>(*pax+asBC_SWORDARG0(pNextOp));
						as<void*>(*esi) = pax;
						nextEAX = EAX_Stack;

						pOp = pThirdOp + toSize(thirdOp);
						continue;
					}
					break;
				}
			}

			switch(op) {
			case asBC_SetV4:
				if(nextOp == asBC_SetV4 && asBC_DWORDARG(pOp) == asBC_DWORDARG(pNextOp)) {
					//Optimize intializing 2 variables to the same value (often 0)
					if(asBC_DWORDARG(pOp) == 0)
						eax ^= eax;
					else
						eax = asBC_DWORDARG(pOp);
					*edi-offset(pOp,0) = eax;
					*edi-offset(pNextOp,0) = eax;

					pOp = pThirdOp;
					continue;
				}
				break;
			case asBC_RDR4:
				if(nextOp == asBC_PshV4 && asBC_SWORDARG0(pOp) == asBC_SWORDARG0(pNextOp)) {
					//Optimize:
					//Store temporary int
					//Push stored temporary
					eax = *ebx;
					*edi-offset0 = eax;
					
					reservedPushBytes = findTotalPushBatchSize(pNextOp, end);
					esi -= reservedPushBytes;
					reservedPushBytes -= sizeof(asDWORD);
					*esi + reservedPushBytes = eax;
					if(reservedPushBytes == 0)
						nextEAX = EAX_Stack;

					pOp = pThirdOp;
					continue;
				}
				break;
			case asBC_PSF:
			case asBC_PshVPtr:
				if(reservedPushBytes == 0 && nextOp == asBC_COPY) {
					//Optimize:
					//Push Pointer
					//Copy Pointer
					//To:
					//Copy Pointer

					check_space(256);
#ifdef JIT_64
					Register arg0 = as<void*>(cpu.intArg64(0, 0));
#else
					Register arg0 = pcx;
#endif
					if(op == asBC_PSF)
						arg0.copy_address(as<void*>(*edi-offset0));
					else //if(op == asBC_PshVPtr)
						arg0 = as<void*>(*edi-offset0);
					if(currentEAX != EAX_Stack)
						pax = as<void*>(*esi);

					//Check for null pointers
					pax &= pax;
					void* test1 = cpu.prep_short_jump(Zero);
					arg0 &= arg0;
					void* test2 = cpu.prep_short_jump(Zero);
					
					as<void*>(*esi) = arg0;
					nextEAX = EAX_Stack;

					cpu.call_cdecl((void*)memcpy,"rrc", &arg0, &pax, unsigned(asBC_WORDARG0(pNextOp))*4);
					void* skip_ret = cpu.prep_short_jump(Jump);
					//ERR
					cpu.end_short_jump(test1); cpu.end_short_jump(test2);
					Return(false);
					cpu.end_short_jump(skip_ret);

					pOp = pThirdOp;
					continue;
				}
				break;
			case asBC_CpyRtoV4:
				if(nextOp == asBC_CpyVtoV4 && offset(pOp,0) == offset(pNextOp,1)) {
					//Optimize
					//Copy Temp to Var X
					//Copy Var X to Var Y
					//To:
					//Copy Temp to Var X
					//Copy Temp to Var Y

					*edi-offset(pOp,0) = ebx;
					*edi-offset(pNextOp,0) = ebx;

					pOp = pThirdOp;
					continue;
				}
				break;
			case asBC_CpyVtoV4:
				if(nextOp == asBC_iTOf && offset(pOp,0) == offset(pNextOp,0)) {
					//Optimize:
					//Load integer
					//Convert integer to float in-place
					//To:
					//Load integer
					//Save float

					fpu.load_dword(*edi-offset(pOp,1));
					fpu.store_float(*edi-offset(pOp,0));

					pOp = pThirdOp;
					continue;
				}
				else if(nextOp == asBC_fTOd && offset(pOp,0) == offset(pNextOp,1)) {
					//Optimize:
					//Copy float
					//Convert float to double
					//To:
					//Copy float
					//Store double

					fpu.load_float(*edi-offset(pOp,1));
					fpu.store_float(*edi-offset(pOp,0),false);
					fpu.store_double(as<double>(*edi-offset(pNextOp,0)));

					pOp = pThirdOp;
					continue;
				}
				break;
			case asBC_ADDSi:
				//Optimize ADDSi, RDSPtr to avoid duplicate checks and copies
				if(nextOp == asBC_RDSPtr) {
					if(currentEAX != EAX_Stack)
						pax = as<void*>(*esi);

					pax &= pax;
					auto notNull = cpu.prep_short_jump(NotZero);
					Return(false);
					cpu.end_short_jump(notNull);

					pax = as<void*>(*pax+asBC_SWORDARG0(pOp));
					as<void*>(*esi) = pax;
					nextEAX = EAX_Stack;

					pOp = pThirdOp;
					continue;
				}
			case asBC_CMPi:
			case asBC_CMPIi:
			case asBC_CMPu:
			case asBC_CMPIu:
				{
				JumpType jump = Jump;
				bool isUnsigned = op == asBC_CMPu || op == asBC_CMPIu;

				//Optimize various CMPi, JConditional to avoid additional logic checks
				switch(nextOp) {
				case asBC_JZ: case asBC_JLowZ:
					jump = Equal; break;
				case asBC_JNZ: case asBC_JLowNZ:
					jump = NotEqual; break;
				case asBC_JS:
					jump = isUnsigned ? Below : Sign; break;
				case asBC_JNS:
					jump = isUnsigned ? NotBelow : NotSign; break;
				case asBC_JP:
					jump = isUnsigned ? Above : Greater; break;
				case asBC_JNP:
					jump = isUnsigned ? NotAbove : LessOrEqual; break;
				}

				//Conditional tests never use plain Jump
				if(jump != Jump) {
					eax = *edi-offset0;
					if(op == asBC_CMPIi || op == asBC_CMPIu)
						eax == asBC_DWORDARG(pOp);
					else
						eax == *edi-offset1;

					do_jump_from(jump, pNextOp);

					//Perform comparison if it could have an effect
					/*if(!clearsTemporary(thirdOp)) {
						if(op == asBC_CMPi || op == asBC_CMPIi) {
							bl.setIf(Greater);

							auto t2 = cpu.prep_short_jump(GreaterOrEqual);
							~bl;
							cpu.end_short_jump(t2);
						}
						else {//CMPu/Iu
							bl.setIf(Above);

							auto t2 = cpu.prep_short_jump(NotBelow);
							~bl;
							cpu.end_short_jump(t2);
						}
					}*/

					pOp = pThirdOp;
					continue;
				}
				}
			}
		}

		//Build ops
		switch(op) {
		case asBC_JitEntry:
			if(!firstJitEntry)
				firstJitEntry = (void*)cpu.op;
			asBC_PTRARG(pOp) = (asPWORD)cpu.op;
			waitingForEntry = false;
			break;

		case asBC_PopPtr:
			esi += sizeof(void*);
			break;
		
		//Handle all pushes here by allocating all contiguous push memory at once
#define pushPrep(use) \
	if(reservedPushBytes == 0) {\
		reservedPushBytes = findTotalPushBatchSize(pOp, end);\
		esi -= reservedPushBytes;\
	}\
	reservedPushBytes -= use;

		case asBC_PshC4:
			pushPrep(sizeof(asDWORD));
			*esi + reservedPushBytes = asBC_DWORDARG(pOp);
			break;
		case asBC_PshV4:
			pushPrep(sizeof(asDWORD));
			eax = *edi-offset0;
			*esi + reservedPushBytes = eax;
			if(reservedPushBytes == 0)
				nextEAX = EAX_Stack;
			break;
		case asBC_PSF:
			pushPrep(sizeof(void*));
			pax.copy_address(as<void*>(*edi-offset0));
			as<void*>(*esi + reservedPushBytes) = pax;
			if(reservedPushBytes == 0)
				nextEAX = EAX_Stack;
			break;
		case asBC_PshG4:
			pushPrep(sizeof(asDWORD));
			eax = MemAddress(cpu, (void*)asBC_PTRARG(pOp));
			*esi + reservedPushBytes = eax;
			if(reservedPushBytes == 0)
				nextEAX = EAX_Stack;
			break;
		case asBC_PshGPtr:
			pushPrep(sizeof(void*));
			pax = as<void*>(MemAddress(cpu, (void*)asBC_PTRARG(pOp)));
			as<void*>(*esi + reservedPushBytes) = pax;
			if(reservedPushBytes == 0)
				nextEAX = EAX_Stack;
			break;
		case asBC_PshC8:
			{
				pushPrep(sizeof(asQWORD));
				asQWORD qword = asBC_QWORDARG(pOp);
#ifdef JIT_64
				as<asQWORD>(eax) = qword;
				as<asQWORD>(*esi + reservedPushBytes) = eax;
#else
				asDWORD* as_dword = (asDWORD*)&qword;
				*esi + reservedPushBytes+4 = as_dword[1];
				*esi + reservedPushBytes = as_dword[0];
#endif
			} break;
		case asBC_PshVPtr:
			pushPrep(sizeof(void*));
			pax = as<void*>(*edi-offset0);
			as<void*>(*esi + reservedPushBytes) = pax;
			if(reservedPushBytes == 0)
				nextEAX = EAX_Stack;
			break;
		case asBC_PshRPtr: 
			pushPrep(sizeof(void*));
			as<void*>(*esi + reservedPushBytes) = pbx;
			break;
		case asBC_PshNull:
			pushPrep(sizeof(void*));
			pax ^= pax;
			as<void*>(*esi + reservedPushBytes) = pax;
			if(reservedPushBytes == 0)
				nextEAX = EAX_Stack;
			break;
		case asBC_OBJTYPE:
			pushPrep(sizeof(void*));
			as<void*>(*esi + reservedPushBytes) = (void*)asBC_PTRARG(pOp);
			break;
		case asBC_TYPEID:
			pushPrep(sizeof(asDWORD));
			*esi + reservedPushBytes = asBC_DWORDARG(pOp);
			break;
		case asBC_FuncPtr:
			pushPrep(sizeof(void*));
			as<void*>(*esi + reservedPushBytes) = (void*)asBC_PTRARG(pOp);
			break;
		case asBC_PshV8:
			pushPrep(sizeof(asQWORD));
			cpu.setBitMode(64);
			(*esi + reservedPushBytes).direct_copy(*edi-offset0, eax);
			cpu.resetBitMode();
			break;
		case asBC_PGA:
			pushPrep(sizeof(void*));
			as<void*>(*esi + reservedPushBytes) = (void*)asBC_PTRARG(pOp);
			break;
		case asBC_VAR:
			pushPrep(sizeof(void*));
			as<void*>(*esi + reservedPushBytes) = (void*)asBC_SWORDARG0(pOp);
			break;

		////Now the normally-ordered ops
		case asBC_SwapPtr:
			if(currentEAX != EAX_Stack)
				pax = as<void*>(*esi);
			pax.swap(as<void*>(*esi+sizeof(void*)));
			as<void*>(*esi) = pax;
			nextEAX = EAX_Stack;
			break;
		case asBC_NOT:
			{
				if(currentEAX != EAX_Offset + offset0)
					al = as<byte>(*edi-offset0);
				al &= al;
				al.setIf(Zero);
				eax.copy_zeroing(al);
				*edi-offset0 = eax;
				nextEAX = EAX_Offset + offset0;
			} break;
		//case asBC_PshG4: //All pushes are handled above, near asBC_PshC4
		case asBC_LdGRdR4:
			pbx = (void*) asBC_PTRARG(pOp);
			eax = *pbx;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_CALL:
			{
				check_space(256);
				as<void*>(*ebp + offsetof(asSVMRegisters,programPointer)) = pOp+2;
				as<void*>(*ebp + offsetof(asSVMRegisters,stackPointer)) = esi;

				asCScriptFunction* func = (asCScriptFunction*)function->GetEngine()->GetFunctionById(asBC_INTARG(pOp));
				if(PrepareJitScriptCall(func)) {
					JitScriptCall(func);
					ReturnFromJittedScriptCall(0);
				}
				else {
					ReturnFromScriptCall();
				}
			} break;
		case asBC_RET: {
				//Not implemented if script call jitting is off,
				//since it's dependent on how calls are made
				if(flags & JIT_NO_SCRIPT_CALLS) {
					Return(true);
					break;
				}
#ifdef JIT_64
				Register arg0 = cpu.intArg64(0, 0, pax);
#else
				Register arg0 = pax;
#endif
				arg0 = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));
				cpu.call_stdcall((void*)returnScriptFunction,"r", &arg0);

				//Pop arguments off the stack
				esi = as<void*>(*ebp+offsetof(asSVMRegisters,stackPointer));
				esi += asBC_WORDARG0(pOp) * sizeof(asDWORD);
				as<void*>(*ebp+offsetof(asSVMRegisters,stackPointer)) = esi;

				//Update value register
				as<void*>(*ebp+offsetof(asSVMRegisters,valueRegister)) = pbx;

				ReturnFromScriptCall();
		   } break;
		case asBC_JMP:
			do_jump(Jump);
			break;

		case asBC_JLowZ: //ClrHi is a NOP, so JlowZ is JZ (same with NZ)
		case asBC_JZ:
			bl &= bl; do_jump(Zero); break;
		case asBC_JLowNZ:
		case asBC_JNZ:
			bl &= bl; do_jump(NotZero); break;
		case asBC_JS:
			bl &= bl; do_jump(Sign); break;
		case asBC_JNS:
			bl &= bl; do_jump(NotSign); break;
		case asBC_JP:
			bl == 0; do_jump(Greater); break;
		case asBC_JNP:
			bl == 0; do_jump(LessOrEqual); break;

		case asBC_TZ:
			bl &= bl; ebx.setIf(Zero); ebx.copy_zeroing(ebx); break;
		case asBC_TNZ:
			bl &= bl; ebx.setIf(NotZero); ebx.copy_zeroing(ebx); break;
		case asBC_TS:
			bl &= bl; ebx.setIf(Sign); ebx.copy_zeroing(ebx); break;
		case asBC_TNS:
			bl &= bl; ebx.setIf(NotSign); ebx.copy_zeroing(ebx); break;
		case asBC_TP:
			bl == 0; ebx.setIf(Greater); ebx.copy_zeroing(ebx); break;
		case asBC_TNP:
			bl == 0; ebx.setIf(LessOrEqual); ebx.copy_zeroing(ebx); break;

		case asBC_NEGi:
			-(*edi-offset0);
			break;
		case asBC_NEGf:
			fpu.load_float(*edi-offset0);
			fpu.negate();
			fpu.store_float(*edi-offset0);
			break;
		case asBC_NEGd:
			fpu.load_double(*edi-offset0);
			fpu.negate();
			fpu.store_double(*edi-offset0);
			break;
		case asBC_INCi16:
			++as<short>(*ebx);
			break;
		case asBC_INCi8:
			++as<char>(*ebx);
			break;
		case asBC_DECi16:
			--as<short>(*ebx);
			break;
		case asBC_DECi8:
			--as<char>(*ebx);
			break;
		case asBC_INCi:
			++*ebx;
			break;
		case asBC_DECi:
			--*ebx;
			break;
		case asBC_INCf:
			fpu.load_const_1();
			fpu.add_float(*ebx);
			fpu.store_float(*ebx);
			break;
		case asBC_DECf:
			fpu.load_const_1();
			fpu.negate();
			fpu.add_float(*ebx);
			fpu.store_float(*ebx);
			break;
		case asBC_INCd:
			fpu.load_const_1();
			fpu.add_double(*ebx);
			fpu.store_double(*ebx);
			break;
		case asBC_DECd:
			fpu.load_const_1();
			fpu.negate();
			fpu.add_double(*ebx);
			fpu.store_double(*ebx);
			break;
		case asBC_IncVi:
			++(*edi-offset0);
			break;
		case asBC_DecVi:
			--(*edi-offset0);
			break;
		case asBC_BNOT:
			~(*edi-offset0);
			break;
		case asBC_BAND:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax &= *edi-offset2;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_BOR:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax |= *edi-offset2;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_BXOR:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax ^= *edi-offset2;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_BSLL: {
			Register c(cpu, ECX);
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			c = *edi-offset2;
			eax <<= c;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			} break;
		case asBC_BSRL: {
			Register c(cpu, ECX);
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			c = *edi-offset2;
			eax.rightshift_logical(c);
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			} break;
		case asBC_BSRA: {
			Register c(cpu, ECX);
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			c = *edi-offset2;
			eax >>= c;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			} break;
		case asBC_COPY:
			{
				check_space(128);
				if(currentEAX != EAX_Stack)
					pax = as<void*>(*esi);
				esi += sizeof(void*);
#ifdef JIT_64
				Register arg1 = as<void*>(cpu.intArg64(1, 1));
#else
				Register arg1 = pcx;
#endif
				arg1 = as<void*>(*esi);

				//Check for null pointers
				pax &= pax;
				void* test1 = cpu.prep_short_jump(Zero);
				arg1 &= arg1;
				void* test2 = cpu.prep_short_jump(Zero);
				
				as<void*>(*esi) = pax;

				cpu.call_cdecl((void*)memcpy,"rrc", &pax, &arg1, unsigned(asBC_WORDARG0(pOp))*4);
				void* skip_ret = cpu.prep_short_jump(Jump);
				//ERR
				cpu.end_short_jump(test1); cpu.end_short_jump(test2);
				Return(false);
				cpu.end_short_jump(skip_ret);
			} break;
		//case asBC_PshC8: //All pushes are handled above, near asBC_PshC4
		//case asBC_PshVPtr:
		case asBC_RDSPtr:
			{
				if(currentEAX != EAX_Stack)
					pax = as<void*>(*esi);

				pax &= pax;
				auto notNull = cpu.prep_short_jump(NotZero);
				Return(false);
				cpu.end_short_jump(notNull);

				pax = as<void*>(*pax);
				as<void*>(*esi) = pax;
				nextEAX = EAX_Stack;
			} break;
		case asBC_CMPd:
			{
				fpu.load_double(*edi-offset1);
				fpu.load_double(*edi-offset0);
				fpu.compare_toCPU(FPU_1);

				bl.setIf(Above);
				auto t2 = cpu.prep_short_jump(NotCarry);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);

				fpu.pop();
			} break;
		case asBC_CMPu:
			{
				eax = *edi-offset0;
				eax == *edi-offset1;

				bl.setIf(Above);
				auto t2 = cpu.prep_short_jump(NotBelow);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);
			} break;
		case asBC_CMPf:
			{
				fpu.load_float(*edi-offset1);
				fpu.load_float(*edi-offset0);
				fpu.compare_toCPU(FPU_1);

				bl.setIf(Above);
				auto t2 = cpu.prep_short_jump(NotCarry);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);

				fpu.pop();
			} break;
		case asBC_CMPi:
			{
				eax = *edi-offset0;
				eax == *edi-offset1;

				bl.setIf(Greater);
				auto t2 = cpu.prep_short_jump(GreaterOrEqual);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);
			} break;
		case asBC_CMPIi:
			{
				eax = *edi-offset0;
				eax == asBC_DWORDARG(pOp);

				bl.setIf(Greater);
				auto t2 = cpu.prep_short_jump(GreaterOrEqual);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);
			} break;
		case asBC_CMPIf:
			{
				fpu.load_float(MemAddress(cpu,&asBC_FLOATARG(pOp)));
				fpu.load_float(*edi-offset0);
				fpu.compare_toCPU(FPU_1);

				bl.setIf(Above);
				auto t2 = cpu.prep_short_jump(NotCarry);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);

				fpu.pop();
			} break;
		case asBC_CMPIu:
			{
				eax = *edi-offset0;
				eax == asBC_DWORDARG(pOp);

				bl.setIf(Above);
				auto t2 = cpu.prep_short_jump(NotBelow);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);
			} break;
		case asBC_JMPP:
			if((flags & JIT_NO_SWITCHES) == 0) {
				tableInUse = true;

				pax = (void*)(jumpTable + ((pOp + 1) - start));

				pdx.copy_expanding(as<int>(*edi - offset0));
				pdx += pdx;

				pcx = as<void*>(*pax + pdx*sizeof(void*));

				//Check for a pointer in the jump table to executable code
				pcx &= pcx;
				auto handled_jump = cpu.prep_short_jump(NotZero);

				//Copy the offsetted pointer to edx and return
				ecx = (void*)(pOp + 1);
				rarg.copy_address(*pcx + pdx*4);
				cpu.jump(Jump,ret_pos);

				cpu.end_short_jump(handled_jump);
				cpu.jump(pcx);
			}
			else {
				Return(true);
			}
			break;
		case asBC_PopRPtr:
			pbx = as<void*>(*esi);
			esi += sizeof(void*);
			break;
		//case asBC_PshRPtr: //All pushes are handled above, near asBC_PshC4
		case asBC_STR:
			{
				const asCString &str = ((asCScriptEngine*)function->GetEngine())->GetConstantString(asBC_WORDARG0(pOp));
				esi -= sizeof(void*) + sizeof(asDWORD);
				as<void*>(*esi + sizeof(asDWORD)) = (void*)str.AddressOf();
				as<asDWORD>(*esi) = (asDWORD)str.GetLength();
			} break;
		case asBC_CALLSYS:
			{
				check_space(512);
				asCScriptFunction* func = (asCScriptFunction*)function->GetEngine()->GetFunctionById(asBC_INTARG(pOp));
				sysCall.callSystemFunction(func);
			} break;
		case asBC_CALLBND:
			{
				check_space(512);
				as<void*>(*ebp + offsetof(asSVMRegisters,programPointer)) = pOp+2;
				as<void*>(*ebp + offsetof(asSVMRegisters,stackPointer)) = esi;

				if(flags & JIT_NO_SCRIPT_CALLS) {
#ifdef JIT_64
					Register arg0 = as<void*>(cpu.intArg64(0, 0, pax));
#else
					Register arg0 = pax;
#endif
					arg0 = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));

					cpu.call_stdcall((void*)callBoundFunction,"rc",
						&arg0,
						(unsigned int)asBC_INTARG(pOp));
					pax &= pax;
					auto p2 = cpu.prep_short_jump(NotZero);
					Return(false);
					cpu.end_short_jump(p2);
					ReturnFromScriptCall();
				}
				else {
					JitScriptCallBnd(asBC_INTARG(pOp));
					ReturnFromJittedScriptCall(0);
				}
			} break;
		case asBC_SUSPEND:
			if(flags & JIT_NO_SUSPEND) {
				//Do nothing
			}
			else {
				//Check if we should suspend
				cl = as<byte>(*ebp+offsetof(asSVMRegisters,doProcessSuspend));
				cl &= cl;
				auto skip = cpu.prep_short_jump(Zero);
				
				as<void*>(*ebp + offsetof(asSVMRegisters,programPointer)) = pOp;
				as<void*>(*ebp + offsetof(asSVMRegisters,stackPointer)) = esi;

#ifdef JIT_64
				Register arg0 = as<void*>(cpu.intArg64(0, 0));
#else
				Register arg0 = pdx;
#endif
				arg0 = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));
				cpu.call_stdcall((void*)doSuspend, "r", &arg0);

				//If doSuspend return true, return to AngelScript for a suspension
				rarg = (void*)pOp;
				al &= al;
				cpu.jump(NotZero, ret_pos);
				
				cpu.end_short_jump(skip);
			}
			break;
		case asBC_ALLOC:
			{
				check_space(512);
				asCObjectType *objType = (asCObjectType*)(size_t)asBC_PTRARG(pOp);
				int func = asBC_INTARG(pOp+AS_PTR_SIZE);

				if(objType->flags & asOBJ_SCRIPT_OBJECT) {
					as<void*>(*ebp + offsetof(asSVMRegisters,programPointer)) = pOp;
					as<void*>(*ebp + offsetof(asSVMRegisters,stackPointer)) = esi;
					as<void*>(*ebp + offsetof(asSVMRegisters,stackFramePointer)) = pdi;

					asIScriptEngine* engine = function->GetEngine();
					asCScriptFunction* f = ((asCScriptEngine*)engine)->GetScriptFunction(func);

					cpu.call_stdcall((void*)allocScriptObject,"pppr",objType,f,engine,&ebp);

					if(PrepareJitScriptCall(f)) {
						JitScriptCall(f);
						ReturnFromJittedScriptCall((void*)(pOp+(2+AS_PTR_SIZE)));
					}
					else {
						ReturnFromScriptCall();
					}
				}
				else {
					cpu.call_stdcall((void*)engineAlloc,"pp",
						(asCScriptEngine*)function->GetEngine(),
						objType);

					if( func ) {
						as<void*>(*esp + local::allocMem) = pax;
						auto pFunc = (asCScriptFunction*)function->GetEngine()->GetFunctionById(func);

						pcx = pax;
						sysCall.callSystemFunction(pFunc, &pcx);
						
						pax = as<void*>(*esp + local::allocMem);
					}

					//Pop pointer destination from vm stack
					pcx = as<void*>(*esi);
					esi += sizeof(void*);

					//Set it if not zero
					pcx &= pcx;
					auto p = cpu.prep_short_jump(Zero);
					as<void*>(*pcx) = pax;
					cpu.end_short_jump(p);
				}
			} break;
		case asBC_FREE:
			{
			asCObjectType *objType = (asCObjectType*)(size_t)asBC_PTRARG(pOp);

			if(!(objType->flags & asOBJ_REF) || !(objType->flags & asOBJ_NOCOUNT)) { //Only do FREE on non-reference types, or reference types without fake reference counting
				check_space(128);
				asSTypeBehaviour *beh = &objType->beh;

#ifdef JIT_64
				Register arg1 = as<void*>(cpu.intArg64(1, 1));
#else
				Register arg1 = pcx;
#endif

				//Check the pointer to see if it's already zero
				arg1 = as<void*>(*edi-offset0);
				arg1 &= arg1;
				auto p = cpu.prep_short_jump(Zero);

				//Copy over registers to the vm in case the called functions observe the call stack
				if((flags & JIT_ALLOC_SIMPLE) == 0) {
					cpu.setBitMode(sizeof(void*)*8);
					*ebp + offsetof(asSVMRegisters,programPointer) = pOp;
					*ebp + offsetof(asSVMRegisters,stackPointer) = esi;
					cpu.resetBitMode();
				}

				if(beh->release) {
					cpu.call_stdcall((void*)engineRelease,"prp",
						(asCScriptEngine*)function->GetEngine(),
						&arg1,
						(asCScriptFunction*)function->GetEngine()->GetFunctionById(beh->release) );
				}
				else if(beh->destruct) {
					cpu.call_stdcall((void*)engineDestroyFree,"prp",
						(asCScriptEngine*)function->GetEngine(),
						&arg1,
						(asCScriptFunction*)function->GetEngine()->GetFunctionById(beh->destruct) );
				}
				else {
					cpu.call_stdcall((void*)engineFree,"pr",
						(asCScriptEngine*)function->GetEngine(),
						&arg1);
				}

				as<void*>(*edi-offset0) = nullptr;
				cpu.end_short_jump(p);
			}
			else {
				as<void*>(*edi-offset0) = nullptr;
			}
			}break;
		case asBC_LOADOBJ:
			{
				cpu.setBitMode(sizeof(void*)*8);
				eax = *edi-offset0;
				*ebp+offsetof(asSVMRegisters,objectType) = nullptr;
				*ebp+offsetof(asSVMRegisters,objectRegister) = eax;
				*edi-offset0 = nullptr;
				cpu.resetBitMode();
			} break;
		case asBC_STOREOBJ:
			{
				cpu.setBitMode(sizeof(void*) * 8);
				(*edi-offset0).direct_copy( (*ebp+offsetof(asSVMRegisters,objectRegister)), eax);
				*ebp+offsetof(asSVMRegisters,objectRegister) = nullptr;
				cpu.resetBitMode();
			} break;
		case asBC_GETOBJ:
			{
				pax.copy_address(*esi+offset0);

				pdx = as<asDWORD>(*eax); //-Offset
				-pdx;

				pcx.copy_address(*edi+pdx*4);

				as<void*>(*pax).direct_copy(as<void*>(*pcx), pdx);
				as<void*>(*pcx) = nullptr;
			} break;
		case asBC_RefCpyV:
		case asBC_REFCPY:
			{
			asCObjectType *objType = (asCObjectType*)(size_t)asBC_PTRARG(pOp);

			if(objType->flags & asOBJ_NOCOUNT) {
				if(op == asBC_REFCPY) {
					pax = as<void*>(*esi);
					esi += sizeof(void*);
				}
				else { //Inline PSF
					pax.copy_address(as<void*>(*edi-offset0));
				}
				pcx = as<void*>(*esi);
				as<void*>(*pax) = pcx;
			}
			else {
				check_space(512);
				//Copy over registers to the vm in case the called functions observe the call stack
				as<void*>(*ebp + offsetof(asSVMRegisters,programPointer)) = pOp;
				as<void*>(*ebp + offsetof(asSVMRegisters,stackPointer)) = esi;

#ifdef JIT_64
				Register arg1 = as<void*>(cpu.intArg64(1, 1));
#else
				Register arg1 = pcx;
#endif

				asSTypeBehaviour *beh = &objType->beh;

				if(op == asBC_REFCPY) {
					pax = as<void*>(*esi);
					as<void*>(*esp + local::object2) = pax;

					esi += sizeof(void*);
				}
				else { //Inline PSF
					pax.copy_address(as<void*>(*edi-offset0));
					as<void*>(*esp + local::object2) = pax;
				}
				arg1 = as<void*>(*esi);
				as<void*>(*esp + local::object1) = arg1;

				//Add reference to object 1, if not null
				arg1 &= arg1;
				auto prev = cpu.prep_short_jump(Zero);
				cpu.call_stdcall((void*)engineCallMethod,"prp",
					(asCScriptEngine*)function->GetEngine(),
					&arg1,
					(asCScriptFunction*)function->GetEngine()->GetFunctionById(beh->addref) );
				cpu.end_short_jump(prev);

				//Release reference from object 2, if not null
				arg1 = as<void*>(*esp+local::object2);
				arg1 = as<void*>(*arg1);
				arg1 &= arg1;
				auto dest = cpu.prep_short_jump(Zero);
				cpu.call_stdcall((void*)engineCallMethod,"prp",
					(asCScriptEngine*)function->GetEngine(),
					&arg1,
					(asCScriptFunction*)function->GetEngine()->GetFunctionById(beh->release) );
				cpu.end_short_jump(dest);

				pax = as<void*>(*esp + local::object1);
				pdx = as<void*>(*esp + local::object2);
				as<void*>(*pdx) = pax;
			}
			}break;
		case asBC_CHKREF:
			{
				if(currentEAX != EAX_Stack)
					pax = as<void*>(*esi);
				pax &= pax;
				auto p = cpu.prep_short_jump(NotZero);
				Return(false);
				cpu.end_short_jump(p);
			} break;
		case asBC_GETOBJREF:
			pax.copy_address(*esi + (asBC_WORDARG0(pOp)*sizeof(asDWORD)));

			pcx = as<void*>(*pax); //-Offset
			-pcx;

			as<void*>(*pax).direct_copy(as<void*>(*pdi+pcx*sizeof(asDWORD)), pdx);
			break;
		case asBC_GETREF:
			pax.copy_address(*esi + (asBC_WORDARG0(pOp)*sizeof(asDWORD)));

			pcx = as<void*>(*pax); //-Offset
			-pcx;

			pcx.copy_address(*pdi+pcx*sizeof(asDWORD));
			as<void*>(*pax) = pcx;
			break;
		//case asBC_PshNull: //All pushes are handled above, near asBC_PshC4
		case asBC_ClrVPtr:
			pax ^= pax;
			as<void*>(*edi-offset0) = pax;
			break;
		//case asBC_OBJTYPE: //All pushes are handled above, near asBC_PshC4
		//case asBC_TYPEID:
		case asBC_SetV1: //V1 and V2 are identical on little-endian processors
		case asBC_SetV2:
		case asBC_SetV4:
			*edi-offset0 = asBC_DWORDARG(pOp);
			break;
		case asBC_SetV8:
			{
#ifdef JIT_64
			pax = asBC_QWORDARG(pOp);
			as<asQWORD>(*edi-offset0) = pax;
#else
			asQWORD* input = &asBC_QWORDARG(pOp);
			asDWORD* data = (asDWORD*)input;
			*edi-offset0+4 = *(data+1);
			*edi-offset0 = *data;
#endif
			} break;
		case asBC_ADDSi:
			{
				if(currentEAX != EAX_Stack)
					pax = as<void*>(*esi);

				pax &= pax;
				auto notNull = cpu.prep_short_jump(NotZero);
				Return(false);
				cpu.end_short_jump(notNull);

				pax += asBC_SWORDARG0(pOp);
				as<void*>(*esi) = pax;
				nextEAX = EAX_Stack;
			} break;

		case asBC_CpyVtoV4:
			as<asDWORD>(*edi-offset0).direct_copy(as<asDWORD>(*edi-offset1), eax);
			break;
		case asBC_CpyVtoV8:
			as<long long>(*edi-offset0).direct_copy(as<long long>(*edi-offset1), eax);
			break;

		case asBC_CpyVtoR4:
			ebx = *edi - offset0;
			break;
		case asBC_CpyVtoR8:
#ifdef JIT_64
			ebx = as<void*>(*edi-offset0);
#else
			ebx = *edi-offset0;
			eax = *edi-offset0+4;
			as<int>(*ebp+offsetof(asSVMRegisters,valueRegister)+4) = eax;
#endif
			break;

		case asBC_CpyVtoG4:
			eax = *edi-offset0;
			MemAddress(cpu, (void*)asBC_PTRARG(pOp)) = eax;
			nextEAX = EAX_Offset + offset0;
			break;

		case asBC_CpyRtoV4:
			as<unsigned>(*edi-offset0) = as<unsigned>(ebx);
			break;
		case asBC_CpyRtoV8:
#ifdef JIT_64
			as<void*>(*edi-offset0) = pbx;
#else
			*edi-offset0 = ebx;
			eax = *ebp + offsetof(asSVMRegisters,valueRegister)+4;
			*edi-offset0+4 = eax;
#endif
			break;

		case asBC_CpyGtoV4:
			eax = MemAddress(cpu, (void*)asBC_PTRARG(pOp));
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;

		case asBC_WRTV1:
			cpu.setBitMode(8);
			(*ebx).direct_copy(*edi-offset0, eax);
			cpu.resetBitMode();
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_WRTV2:
			cpu.setBitMode(16);
			(*ebx).direct_copy(*edi-offset0, eax);
			cpu.resetBitMode();;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_WRTV4:
			cpu.setBitMode(32);
			(*ebx).direct_copy(*edi-offset0, eax);
			cpu.resetBitMode();
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_WRTV8:
			cpu.setBitMode(64);
			(*ebx).direct_copy(*edi-offset0, eax);
			cpu.resetBitMode();
			break;

		case asBC_RDR1:
			eax = *ebx;
			eax &= 0x000000ff;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_RDR2:
			eax = *ebx;
			eax &= 0x0000ffff;
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_RDR4:
			as<asDWORD>(*edi-offset0).direct_copy(as<asDWORD>(*ebx), eax);
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_RDR8:
			as<asQWORD>(*edi-offset0).direct_copy(as<asQWORD>(*ebx), eax); break;
		case asBC_LDG:
			pbx = (void*)asBC_PTRARG(pOp);
			break;
		case asBC_LDV:
			pbx.copy_address(*edi-offset0);
			break;
		//case asBC_PGA: //All pushes are handled above, near asBC_PshC4
		case asBC_CmpPtr:
			{
				if(currentEAX != EAX_Offset + offset0)
					pax = as<void*>(*edi-offset0);
				pax == as<void*>(*edi-offset1);

				bl.setIf(Above);
				auto t2 = cpu.prep_short_jump(NotBelow);
				~bl; //0xff if < 0
				cpu.end_short_jump(t2);
			}
			break;
		//case asBC_VAR: //All pushes are handled above, near asBC_PshC4
		case asBC_sbTOi:
			eax.copy_expanding(as<char>(*edi-offset0));
			*edi-offset0 = eax;
			break;
		case asBC_swTOi:
			eax.copy_expanding(as<short>(*edi-offset0));
			*edi-offset0 = eax;
			break;
		case asBC_ubTOi:
			*edi-offset0 &= 0xff;
			break;
		case asBC_uwTOi:
			*edi-offset0 &= 0xffff;
			break;
		case asBC_ADDi:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax += *edi-offset2;
			*edi-offset0 = eax;
			break;
		case asBC_SUBi:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax -= *edi-offset2;
			*edi-offset0 = eax;
			break;
		case asBC_MULi:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax *= *edi-offset2;
			*edi-offset0 = eax;
			break;
		case asBC_DIVi:
			ecx = *edi-offset2;

			eax = ecx;
			eax &= eax;
			{
			void* zero_test = cpu.prep_short_jump(NotZero);
			Return(false);
			cpu.end_short_jump(zero_test);
			}

			eax = *edi-offset1;
			edx ^= edx;

			{
			eax == 0;
			auto notSigned = cpu.prep_short_jump(NotSign);
			~edx;
			cpu.end_short_jump(notSigned);
			}

			ecx.divide_signed();

			*edi-offset0 = eax;
			break;
		case asBC_MODi:
			ecx = *edi-offset2;

			eax = ecx;
			eax &= eax;
			{
			void* zero_test = cpu.prep_short_jump(NotZero);
			Return(false);
			cpu.end_short_jump(zero_test);
			}

			eax = *edi-offset1;
			edx ^= edx;

			{
			eax == 0;
			auto notSigned = cpu.prep_short_jump(NotSign);
			~edx;
			cpu.end_short_jump(notSigned);
			}

			ecx.divide_signed();

			*edi-offset0 = edx;
			break;
		case asBC_ADDf:
			fpu.load_float(*edi-offset1);
			fpu.add_float(*edi-offset2);
			fpu.store_float(*edi-offset0);
			break;
		case asBC_SUBf:
			fpu.load_float(*edi-offset1);
			fpu.sub_float(*edi-offset2);
			fpu.store_float(*edi-offset0);
			break;
		case asBC_MULf:
			fpu.load_float(*edi-offset1);
			fpu.mult_float(*edi-offset2);
			fpu.store_float(*edi-offset0);
			break;
		case asBC_DIVf:
			fpu.load_float(*edi-offset1);
			fpu.div_float(*edi-offset2);
			fpu.store_float(*edi-offset0);
			break;
		case asBC_MODf: {
#ifdef JIT_64
			Register arg0 = as<void*>(cpu.intArg64(0, 0));
			Register arg1 = as<void*>(cpu.intArg64(1, 1));
#else
			Register arg0 = ecx;
			Register arg1 = eax;
#endif
			arg0.copy_address(*edi-offset1);
			arg1.copy_address(*edi-offset2);
			cpu.call_stdcall((void*)fmod_wrapper_f,"rr",&arg0,&arg1);
#ifdef JIT_64
			*edi-offset0 = cpu.floatReturn64();
#else
			fpu.store_float(*edi-offset0);
#endif
		} break;
		case asBC_ADDd:
			fpu.load_double(*edi-offset1);
			fpu.add_double(*edi-offset2);
			fpu.store_double(*edi-offset0);
			break;
		case asBC_SUBd:
			fpu.load_double(*edi-offset1);
			fpu.sub_double(*edi-offset2);
			fpu.store_double(*edi-offset0);
			break;
		case asBC_MULd:
			fpu.load_double(*edi-offset1);
			fpu.mult_double(*edi-offset2);
			fpu.store_double(*edi-offset0);
			break;
		case asBC_DIVd:
			//TODO: AngelScript considers division by 0 an error, should we?
			fpu.load_double(*edi-offset1);
			fpu.div_double(*edi-offset2);
			fpu.store_double(*edi-offset0);
			break;
		case asBC_MODd: {
#ifdef JIT_64
			Register arg0 = as<void*>(cpu.intArg64(0, 0));
			Register arg1 = as<void*>(cpu.intArg64(1, 1));
#else
			Register arg0 = ecx;
			Register arg1 = eax;
#endif
			arg0.copy_address(*edi-offset1);
			arg1.copy_address(*edi-offset2);
			cpu.call_stdcall((void*)fmod_wrapper,"rr",&arg0,&arg1);
#ifdef JIT_64
			as<double>(*edi-offset0) = cpu.floatReturn64();
#else
			fpu.store_double(*edi-offset0);
#endif
			} break;
		case asBC_ADDIi:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax += asBC_INTARG(pOp+1);
			*edi-offset0 = eax;
			nextEAX = EAX_Offset + offset0;
			break;
		case asBC_SUBIi:
			if(currentEAX != EAX_Offset + offset1)
				eax = *edi-offset1;
			eax -= asBC_INTARG(pOp+1);
			*edi-offset0 = eax;
			break;
		case asBC_MULIi:
			eax.multiply_signed(*edi-offset1,asBC_INTARG(pOp+1));
			*edi-offset0 = eax;
			break;
		case asBC_ADDIf:
			fpu.load_float(*edi-offset1);
			fpu.add_float( MemAddress(cpu,&asBC_FLOATARG(pOp+1)) );
			fpu.store_float(*edi-offset0);
			break;
		case asBC_SUBIf:
			fpu.load_float(*edi-offset1);
			fpu.sub_float( MemAddress(cpu,&asBC_FLOATARG(pOp+1)) );
			fpu.store_float(*edi-offset0);
			break;
		case asBC_MULIf:
			fpu.load_float(*edi-offset1);
			fpu.mult_float( MemAddress(cpu,&asBC_FLOATARG(pOp+1)) );
			fpu.store_float(*edi-offset0);
			break;
		case asBC_SetG4:
			MemAddress(cpu,(void*)asBC_PTRARG(pOp)) = asBC_DWORDARG(pOp+AS_PTR_SIZE);
			break;
		case asBC_ChkRefS:
			//Return if *(*esi) == 0
			if(currentEAX != EAX_Stack)
				pax = as<void*>(*esi);
			eax = as<int>(*pax);
			eax &= eax;
			rarg = (void*)pOp;
			cpu.jump(Zero,ret_pos);
			break;
		case asBC_ChkNullV:
			//Return if (*edi-offset0) == 0
			if(currentEAX != EAX_Offset + offset0)
				eax = *edi-offset0;
			eax &= eax;
			rarg = (void*)pOp;
			cpu.jump(Zero,ret_pos);
			break;
		case asBC_CALLINTF:
			{
				check_space(256);
				as<void*>(*ebp + offsetof(asSVMRegisters,programPointer)) = (void*)(pOp+2);
				as<void*>(*ebp + offsetof(asSVMRegisters,stackPointer)) = as<void*>(esi);

				asCScriptFunction* func = (asCScriptFunction*)function->GetEngine()->GetFunctionById(asBC_INTARG(pOp));

				//This assumes all interface calls can be jitted since
				//there's no way to tell beforehand. It's probably
				//a safe assumption considering all functions are passed
				//through the jit at _some_ point, but that may change in the future
				if(flags & JIT_NO_SCRIPT_CALLS) {
					MemAddress ctxPtr( as<void*>(*ebp + offsetof(asSVMRegisters,ctx)) );

					cpu.call_stdcall((void*)callInterfaceMethod,"mp", &ctxPtr, func);
					ReturnFromScriptCall();
				}
				else {
					JitScriptCallIntf(func);
					ReturnFromJittedScriptCall(0);
				}
			} break;
		//asBC_SetV1 and asBC_SetV2 are aliased to asBC_SetV4
		case asBC_Cast: //Can't handle casts (script call)
			{
				check_space(512);
#ifdef JIT_64
				Register arg0 = as<void*>(cpu.intArg64(0, 0, eax));
#else
				Register arg0 = ecx;
#endif
				arg0 = as<void*>(*esi);
				arg0 &= arg0;
				auto toEnd1 = cpu.prep_short_jump(Zero);
				arg0 = as<void*>(*arg0);
				arg0 &= arg0;
				auto toEnd2 = cpu.prep_short_jump(Zero);
				
				asCObjectType *to = ((asCScriptEngine*)function->GetEngine())->GetObjectTypeFromTypeId(asBC_DWORDARG(pOp));
				cpu.call_stdcall((void*)castObject,"rp",&arg0,to);
				pax &= pax;
				auto toEnd3 = cpu.prep_short_jump(Zero);

				as<void*>(*ebp + offsetof(asSVMRegisters,objectRegister)) = pax;
				
				cpu.end_short_jump(toEnd1);
				cpu.end_short_jump(toEnd2);
				cpu.end_short_jump(toEnd3);
				esi += sizeof(void*);
			} break;

		case asBC_iTOb:
			*edi-offset0 &= 0xff;
			break;
		case asBC_iTOw:
			*edi-offset0 &= 0xffff;
			break;

#ifdef JIT_64
#ifdef _MSC_VER
#define cast(f,t) {\
	Register arg0 = as<void*>(cpu.intArg64(0, 0));\
	Register arg1 = as<void*>(cpu.intArg64(1, 1));\
	void* func = (void*)(void (*)(f*,t*))(directConvert<f,t>);\
	arg1.copy_address(*edi-offset0);\
	if(sizeof(f) != sizeof(t))\
		{ arg0.copy_address(*edi-offset1); cpu.call_stdcall(func,"rr",&arg0,&arg1); }\
	else\
		{ arg0 = arg1; cpu.call_stdcall(func,"rr",&arg0,&arg1); }\
	}
#else
#define cast(f,t) {\
	Register arg0 = as<void*>(cpu.intArg64(0, 0));\
	Register arg1 = as<void*>(cpu.intArg64(1, 1));\
	void* func = (void*)(void (*)(f*,t*))(directConvert<f,t>);\
	arg1.copy_address(*edi-offset0);\
	if(sizeof(f) != sizeof(t))\
		{ arg0.copy_address(*edi-offset1); cpu.call_cdecl(func,"rr",&arg0,&arg1); }\
	else\
		{ arg0 = arg1; cpu.call_cdecl(func,"rr",&arg0,&arg1); }\
	}
#endif
#else
#ifdef _MSC_VER
#define cast(f,t) {\
	void* func = (void*)(void (*)(f*,t*))(directConvert<f,t>);\
	pax.copy_address(*edi-offset0);\
	if(sizeof(f) != sizeof(t))\
		{ pcx.copy_address(*edi-offset1); cpu.call_stdcall(func,"rr",&pcx,&pax); }\
	else\
		cpu.call_stdcall(func,"rr",&pax,&pax);\
	}
#else
#define cast(f,t) {\
	void* func = (void*)(void (*)(f*,t*))(directConvert<f,t>);\
	pax.copy_address(*edi-offset0);\
	if(sizeof(f) != sizeof(t))\
		{ pcx.copy_address(*edi-offset1); cpu.call_cdecl(func,"rr",&pcx,&pax); }\
	else\
		cpu.call_cdecl(func,"rr",&pax,&pax);\
	}
#endif
#endif

		////All type conversions of QWORD to/from DWORD and Float to/from Int are here
		case asBC_iTOf:
			fpu.load_dword(*edi-offset0);
			fpu.store_float(*edi-offset0);
			break;
		case asBC_fTOi:
			cast(float,int); break;
		case asBC_uTOf:
			cast(unsigned, float); break;
		case asBC_fTOu:
			cast(float, unsigned); break;
		case asBC_dTOi:
			cast(double,int); break;
		case asBC_dTOu:
			cast(double, unsigned); break;
		case asBC_dTOf:
			fpu.load_double(*edi-offset1);
			fpu.store_float(*edi-offset0);
			break;
		case asBC_iTOd:
			fpu.load_dword(*edi-offset1);
			fpu.store_double(*edi-offset0);
			break;
		case asBC_uTOd:
			cast(unsigned, double); break;
		case asBC_fTOd:
			fpu.load_float(*edi-offset1);
			fpu.store_double(*edi-offset0);
			break;
		case asBC_i64TOi:
			cast(long long, int) break;
		case asBC_uTOi64:
			cast(unsigned int, long long) break;
		case asBC_iTOi64:
			cast(int, long long) break;
		case asBC_fTOi64:
			cast(float, long long) break;
		case asBC_fTOu64:
			cast(float, unsigned long long) break;
		case asBC_i64TOf:
			cast(long long, float) break;
		case asBC_u64TOf:
			cast(unsigned long long, float) break;
		case asBC_dTOi64:
			cast(double, long long) break;
		case asBC_dTOu64:
			cast(double, unsigned long long) break;
		case asBC_i64TOd:
			cast(long long, double) break;
		case asBC_u64TOd:
			cast(unsigned long long, double) break;

		case asBC_NEGi64:
			-as<long long>(*edi-offset0);
			break;
		case asBC_INCi64:
			++as<long long>(*ebx);
			break;
		case asBC_DECi64:
			--as<long long>(*ebx);
			break;
		case asBC_BNOT64:
			~as<long long>(*edi-offset0);
			break;
		case asBC_ADDi64:
			{
#ifdef JIT_64
			pax = as<int64_t>(*edi-offset1);
			pax += as<int64_t>(*edi-offset2);
			as<int64_t>(*edi-offset0) = pax;
#else
			eax = *edi-offset1;
			eax += *edi-offset2;
			*edi-offset0 = eax;
			eax = *edi-offset1+4;
			auto p = cpu.prep_short_jump(NotOverflow);
			++eax;
			cpu.end_short_jump(p);
			eax += *edi-offset2+4;
			*edi-offset0+4 = eax;
#endif
			} break;
		case asBC_SUBi64:
			{
#ifdef JIT_64
			pax = as<int64_t>(*edi-offset1);
			pax -= as<int64_t>(*edi-offset2);
			as<int64_t>(*edi-offset0) = pax;
#else
			eax = *edi-offset1;
			eax -= *edi-offset2;
			*edi-offset0 = eax;
			eax = *edi-offset1+4;
			auto p = cpu.prep_short_jump(NotOverflow);
			--eax;
			cpu.end_short_jump(p);
			eax -= *edi-offset2+4;
			*edi-offset0+4 = eax;
#endif
			} break;
		case asBC_MULi64:
#ifdef JIT_64
			pax = as<int64_t>(*edi-offset1);
			pax *= as<int64_t>(*edi-offset2);
			as<int64_t>(*edi-offset0) = pax;
#else
			ecx.copy_address(*edi-offset1);
			edx.copy_address(*edi-offset2);
			eax.copy_address(*edi-offset0);
			cpu.call_stdcall((void*)i64_mul,"rrr",&ecx,&edx,&eax);
#endif
			break;
		case asBC_DIVi64: {
#ifdef JIT_64
			Register arg0 = as<void*>(cpu.intArg64(0, 0));
			Register arg1 = as<void*>(cpu.intArg64(1, 1));
			Register arg2 = as<void*>(cpu.intArg64(2, 2));
#else
			Register arg0 = pcx;
			Register arg1 = pdx;
			Register arg2 = pax;
#endif
			arg0.copy_address(*edi-offset1);
			arg1.copy_address(*edi-offset2);
			arg2.copy_address(*edi-offset0);
			cpu.call_stdcall((void*)i64_div,"rrr",&arg0,&arg1,&arg2);
		  } break;
		case asBC_MODi64: {
#ifdef JIT_64
			Register arg0 = as<void*>(cpu.intArg64(0, 0));
			Register arg1 = as<void*>(cpu.intArg64(1, 1));
			Register arg2 = as<void*>(cpu.intArg64(2, 2));
#else
			Register arg0 = pcx;
			Register arg1 = pdx;
			Register arg2 = pax;
#endif
			arg0.copy_address(*edi-offset1);
			arg1.copy_address(*edi-offset2);
			arg2.copy_address(*edi-offset0);
			cpu.call_stdcall((void*)i64_mod,"rrr",&arg0,&arg1,&arg2);
		  } break;
		case asBC_BAND64:
#ifdef JIT_64
			pax = as<uint64_t>(*edi-offset1);
			pax &= as<uint64_t>(*edi-offset2);
			as<uint64_t>(*edi-offset0) = pax;
#else
			ecx = *edi-offset1;
			edx = *edi-offset1+4;
			ecx &= *edi-offset2;
			edx &= *edi-offset2+4;
			*edi-offset0 = ecx;
			*edi-offset0+4 = edx;
#endif
			break;
		case asBC_BOR64:
#ifdef JIT_64
			pax = as<uint64_t>(*edi-offset1);
			pax |= as<uint64_t>(*edi-offset2);
			as<uint64_t>(*edi-offset0) = pax;
#else
			ecx = *edi-offset1;
			edx = *edi-offset1+4;
			ecx |= *edi-offset2;
			edx |= *edi-offset2+4;
			*edi-offset0 = ecx;
			*edi-offset0+4 = edx;
#endif
			break;
		case asBC_BXOR64:
#ifdef JIT_64
			pax = as<uint64_t>(*edi-offset1);
			pax ^= as<uint64_t>(*edi-offset2);
			as<uint64_t>(*edi-offset0) = pax;
#else
			ecx = *edi-offset1;
			edx = *edi-offset1+4;
			ecx ^= *edi-offset2;
			edx ^= *edi-offset2+4;
			*edi-offset0 = ecx;
			*edi-offset0+4 = edx;
#endif
			break;
		case asBC_BSLL64: {
#ifdef JIT_64
			Register c(cpu, ECX, sizeof(uint64_t) * 8);
			pax = as<uint64_t>(*edi-offset1);
			c = as<uint32_t>(*edi-offset2);
			pax <<= c;
			as<uint64_t>(*edi-offset0) = pax;
#else
			ecx.copy_address(*edi-offset1);
			edx.copy_address(*edi-offset2);
			eax.copy_address(*edi-offset0);
			cpu.call_stdcall((void*)i64_sll,"rrr",&ecx,&edx,&eax);
#endif
			} break;
		case asBC_BSRL64: {
#ifdef JIT_64
			Register c(cpu, ECX, sizeof(uint64_t) * 8);
			pax = as<uint64_t>(*edi-offset1);
			c = as<uint32_t>(*edi-offset2);
			pax.rightshift_logical(c);
			as<uint64_t>(*edi-offset0) = pax;
#else
			ecx.copy_address(*edi-offset1);
			edx.copy_address(*edi-offset2);
			eax.copy_address(*edi-offset0);
			cpu.call_stdcall((void*)i64_srl,"rrr",&ecx,&edx,&eax);
#endif
			} break;
		case asBC_BSRA64: {
#ifdef JIT_64
			Register c(cpu, ECX, sizeof(uint64_t) * 8);
			pax = as<uint64_t>(*edi-offset1);
			c = as<uint32_t>(*edi-offset2);
			pax >>= c;
			as<uint64_t>(*edi-offset0) = pax;
#else
			ecx.copy_address(*edi-offset1);
			edx.copy_address(*edi-offset2);
			eax.copy_address(*edi-offset0);
			cpu.call_stdcall((void*)i64_sra,"rrr",&ecx,&edx,&eax);
#endif
			} break;
		case asBC_CMPi64: {
#ifdef JIT_64
			Register arg0 = as<void*>(cpu.intArg64(0, 0));
			Register arg1 = as<void*>(cpu.intArg64(1, 1));
#else
			Register arg0 = ecx;
			Register arg1 = eax;
#endif
			arg0.copy_address(*edi-offset0);
			arg1.copy_address(*edi-offset1);
			cpu.call_stdcall((void*)cmp_int64,"rr",&arg0,&arg1);
			ebx = eax;
		  } break;
		case asBC_CMPu64: {
#ifdef JIT_64
			Register arg0 = as<void*>(cpu.intArg64(0, 0));
			Register arg1 = as<void*>(cpu.intArg64(1, 1));
#else
			Register arg0 = ecx;
			Register arg1 = eax;
#endif
			arg0.copy_address(*edi-offset0);
			arg1.copy_address(*edi-offset1);
			cpu.call_stdcall((void*)cmp_uint64,"rr",&arg0,&arg1);
			ebx = eax;
		  } break;
		case asBC_ChkNullS:
			{
				if(asBC_WORDARG0(pOp) != 0 && currentEAX != EAX_Stack)
					eax = *esi+(asBC_WORDARG0(pOp) * sizeof(asDWORD));
				eax &= eax;
				void* not_zero = cpu.prep_short_jump(NotZero);
				Return(false);
				cpu.end_short_jump(not_zero);
			} break;
		case asBC_ClrHi:
			//Due to the way logic is handled, the upper bytes area always ignored, and don't need to be cleared
			//ebx &= 0x000000ff;
			break;
		case asBC_CallPtr:
			{
#ifdef JIT_64
				Register arg0 = as<void*>(cpu.intArg64(0, 0));
				Register arg1 = as<void*>(cpu.intArg64(1, 1));
				Register temp = as<int>(cpu.intArg64(2, 2));
#else
				Register arg0 = eax;
				Register arg1 = ecx;
				Register temp = edx;
#endif

				arg1 = as<void*>(*pdi-offset0);
				arg1 &= arg1;
				auto nullFunc = cpu.prep_short_jump(NotZero);

				temp = *arg1 + offsetof(asCScriptFunction,funcType);
				temp == asFUNC_SCRIPT;
				auto isScript = cpu.prep_short_jump(Zero);
				
				cpu.end_short_jump(nullFunc);
				Return(false);
				cpu.end_short_jump(isScript);

				*ebp + offsetof(asSVMRegisters,programPointer) = pOp+1;
				*ebp + offsetof(asSVMRegisters,stackPointer) = esi;

				arg0 = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));
				cpu.call_stdcall((void*)callScriptFunction,"rr",&arg0,&arg1);
				ReturnFromScriptCall();
			} break;
		//case asBC_FuncPtr: //All pushes are handled above, near asBC_PshC4
		case asBC_LoadThisR:
			{
			pbx = as<void*>(*edi);

			pbx &= pbx;
			auto j = cpu.prep_short_jump(NotZero);
			Return(false);
			cpu.end_short_jump(j);

			short off = asBC_SWORDARG0(pOp);
			if(off > 0)
				pbx += off;
			else
				pbx -= -off;
			} break;
		//case asBC_PshV8: //All pushes are handled above, near asBC_PshC4
		case asBC_DIVu:
			ecx = *edi-offset2;

			ecx &= ecx;
			{
			void* zero_test = cpu.prep_short_jump(NotZero);
			Return(false);
			cpu.end_short_jump(zero_test);
			}

			eax = *edi-offset1;
			edx ^= edx;
			ecx.divide();

			*edi-offset0 = eax;
			break;
		case asBC_MODu:
			ecx = *edi-offset2;

			ecx &= ecx;
			{
			void* zero_test = cpu.prep_short_jump(NotZero);
			Return(false);
			cpu.end_short_jump(zero_test);
			}

			eax = *edi-offset1;
			edx ^= edx;
			ecx.divide();

			*edi-offset0 = edx;
			break;
		case asBC_DIVu64:
			{
#ifdef JIT_64
				pcx = as<uint64_t>(*edi-offset2);

				pax = pcx;
				pax &= pax;
				{
				void* zero_test = cpu.prep_short_jump(NotZero);
				Return(false);
				cpu.end_short_jump(zero_test);
				}

				pax = as<uint64_t>(*edi-offset1);
				pdx ^= pdx;
				pcx.divide();

				as<uint64_t>(*edi-offset0) = pax;
#else
				ecx.copy_address(*edi-offset1);
				edx.copy_address(*edi-offset2);
				eax.copy_address(*edi-offset0);
				cpu.call_stdcall((void*)div_ull,"rrr",&ecx,&edx,&eax);
				eax &= eax;
				auto p = cpu.prep_short_jump(Zero);
				//If 1 is returned, this is a divide by 0 error
				Return(false);
				cpu.end_short_jump(p);
#endif
			} break;
		case asBC_MODu64:
			{
#ifdef JIT_64
				pcx = as<uint64_t>(*edi-offset2);

				pax = pcx;
				pax &= pax;
				{
				void* zero_test = cpu.prep_short_jump(NotZero);
				Return(false);
				cpu.end_short_jump(zero_test);
				}

				pax = as<uint64_t>(*edi-offset1);
				pdx ^= pdx;
				pcx.divide();

				as<uint64_t>(*edi-offset0) = pdx;
#else
				ecx.copy_address(*edi-offset1);
				edx.copy_address(*edi-offset2);
				eax.copy_address(*edi-offset0);
				cpu.call_stdcall((void*)mod_ull,"rrr",&ecx,&edx,&eax);
				eax &= eax;
				auto p = cpu.prep_short_jump(Zero);
				//If 1 is returned, this is a divide by 0 error
				Return(false);
				cpu.end_short_jump(p);
#endif
			} break;
		case asBC_LoadRObjR:
			{
			pbx = as<void*>(*edi-offset0);
			pbx &= pbx;
			auto j = cpu.prep_short_jump(NotZero);
			Return(false);
			cpu.end_short_jump(j);
			pbx += asBC_SWORDARG1(pOp);
			} break;
		case asBC_LoadVObjR:
			pbx.copy_address(*edi+(asBC_SWORDARG1(pOp) - offset0));
			break;
		default:
			//printf("Unhandled op: %i\n", op);
			Return(true);
			break;
		}

#ifdef JIT_DEBUG
		pdx = (void*)&DBG_LastOP;
		as<asEBCInstr>(*pdx) = op;
#endif

		pOp += toSize(op);
	}

	//Fill out all deferred pointers for this function
	if(curJitFunction && firstJitEntry) {
		auto range = deferredPointers.equal_range(function);
		for(auto it = range.first; it != range.second; ++it) {
			*it->second.jitFunction = curJitFunction;
			*it->second.jitEntry = firstJitEntry;
		}
	}

	if(waitingForEntry == false)
		Return(true);

	if(tableInUse)
		jumpTables[*output] = (unsigned char**)jumpTable;
	else
		delete[] jumpTable;

	activePage->markUsedAddress((void*)cpu.op);
	jitLock.leave();
	return 0;
}

void asCJITCompiler::finalizePages() {
	jitLock.enter();
	for(auto page = pages.begin(); page != pages.end(); ++page)
		if(!page->second->final)
			page->second->finalize();
	jitLock.leave();
}

void asCJITCompiler::ReleaseJITFunction(asJITFunction func) {
	jitLock.enter();
	auto start = pages.lower_bound(func);

	while(start != pages.end() && start->first == func) {
		if(start->second == activePage) {
			activePage->drop();
			activePage = 0;
		}
		start->second->drop();
		start = pages.erase(start);
	}

	auto table = jumpTables.find(func);

	if(table != jumpTables.end()) {
		delete[] table->second;
		jumpTables.erase(table);
	}
	jitLock.leave();
}

unsigned findTotalPushBatchSize(asDWORD* nextOp, asDWORD* endOfBytecode) {
	unsigned bytes = 0;
	while(nextOp < endOfBytecode) {
		asEBCInstr op = (asEBCInstr)*(asBYTE*)nextOp;
		switch(op) {
			case asBC_PshC4:
			case asBC_PshV4:
			case asBC_PshG4:
			case asBC_TYPEID:
				bytes += sizeof(asDWORD); break;
			case asBC_PshV8:
			case asBC_PshC8:
				bytes += sizeof(asQWORD); break;
			case asBC_PSF:
			case asBC_PshVPtr:
			case asBC_PshRPtr:
			case asBC_PshNull:
			case asBC_FuncPtr:
			case asBC_OBJTYPE:
			case asBC_PGA:
			case asBC_VAR:
			case asBC_PshGPtr:
				bytes += sizeof(void*); break;
			default:
				return bytes;
		}
		nextOp += toSize(op);
	}
	return bytes;
}

void stdcall allocScriptObject(asCObjectType* type, asCScriptFunction* constructor, asIScriptEngine* engine, asSVMRegisters* registers) {
	//Allocate and prepare memory
	void* mem = ((asCScriptEngine*)engine)->CallAlloc(type);
	ScriptObject_Construct(type, (asCScriptObject*)mem);

	//Store at address on the stack
	void** dest = *(void***)(registers->stackPointer + constructor->GetSpaceNeededForArguments());
	if(dest)
		*dest = mem;

	//Push pointer so the constructor can be called
	registers->stackPointer -= AS_PTR_SIZE;
	*(void**)registers->stackPointer = mem;

	registers->programPointer += 2 + AS_PTR_SIZE;

	//((asCContext*)registers->ctx)->CallScriptFunction(constructor);
}

void* stdcall engineAlloc(asCScriptEngine* engine, asCObjectType* type) {
	return engine->CallAlloc(type);
}

void stdcall engineRelease(asCScriptEngine* engine, void* memory, asCScriptFunction* release) {
	engine->CallObjectMethod(memory, release->sysFuncIntf, release);
}

void stdcall engineDestroyFree(asCScriptEngine* engine, void* memory, asCScriptFunction* destruct) {
	engine->CallObjectMethod(memory, destruct->sysFuncIntf, destruct);
	engine->CallFree(memory);
}

void stdcall engineFree(asCScriptEngine* engine, void* memory) {
	engine->CallFree(memory);
}

void stdcall engineCallMethod(asCScriptEngine* engine, void* object, asCScriptFunction* method) {
	engine->CallObjectMethod(object, method->sysFuncIntf, method);
}

void stdcall callScriptFunction(asIScriptContext* ctx, asCScriptFunction* func) {
	asCContext* context = (asCContext*)ctx;
	context->CallScriptFunction(func);
}

asCScriptFunction* stdcall callInterfaceMethod(asIScriptContext* ctx, asCScriptFunction* func) {
	asCContext* context = (asCContext*)ctx;
	context->CallInterfaceMethod(func);
	return context->m_currentFunction;
}

asCScriptFunction* stdcall callBoundFunction(asIScriptContext* ctx, unsigned short fid) {
	asCContext* context = (asCContext*)ctx;
	asCScriptEngine* engine = (asCScriptEngine*)context->GetEngine();
	int funcID = engine->importedFunctions[fid]->boundFunctionId;
	if(funcID == -1) {
		context->SetInternalException(TXT_UNBOUND_FUNCTION);
		return 0;
	}
	asCScriptFunction* func = engine->GetScriptFunction(funcID);
	context->CallScriptFunction(func);
	if(context->m_status != asEXECUTION_ACTIVE)
		return 0;
	return func;
}

void stdcall receiveObjectHandle(asIScriptContext* ctx, asCScriptObject* obj) {
	asCContext* context = (asCContext*)ctx;
	if(obj)
		((asCScriptEngine*)context->GetEngine())->CallObjectMethod(obj, obj->objType->beh.addref);
	context->m_regs.objectRegister = obj;
}

asCScriptObject* stdcall castObject(asCScriptObject* obj, asCObjectType* to) {
	asCObjectType *from = obj->objType;
	if( from->DerivesFrom(to) || from->Implements(to) ) {
		obj->AddRef();
		return obj;
	}
	else {
		return nullptr;
	}
}

bool stdcall doSuspend(asIScriptContext* ctx) {
	asCContext* Ctx = (asCContext*)ctx;

	if(Ctx->m_lineCallback)
		Ctx->CallLineCallback();

	if(Ctx->m_doSuspend) {
		Ctx->m_regs.programPointer += 1;
		if(Ctx->m_status == asEXECUTION_ACTIVE)
			Ctx->m_status = asEXECUTION_SUSPENDED;
		return true;
	}
	else {
		return false;
	}
}

void SystemCall::callSystemFunction(asCScriptFunction* func, Register* objPointer) {
	auto* sys = func->sysFuncIntf;
#ifdef JIT_PRINT_UNHANDLED_CALLS
	auto unhandled = [&]() {
		if(unhandledCalls.find(func) == unhandledCalls.end()) {
			printf("Unhandled JIT Call: %s\n", func->GetDeclaration());
			unhandledCalls.insert(func);
		}
	};
#endif

#ifdef JIT_64
	if( sys->takesObjByVal || sys->hasAutoHandles || sys->hostReturnSize > 4 ||
		(sys->paramAutoHandles.GetLength() != 0 && sys->paramSize == 0) )
#else
	if( sys->takesObjByVal || sys->hasAutoHandles || sys->hostReturnSize > 2 ||
		(sys->paramAutoHandles.GetLength() != 0 && sys->paramSize == 0))
#endif
	{
		//Handle various cases that we cannot yet
		//Note: We do not know parameter sizes for template factories, so we cannot compile them
		//However, they all receive a magic int& that we can detect (paramAutoHandles is not empty, paramSize is)
#ifdef JIT_PRINT_UNHANDLED_CALLS
		unhandled();
#endif
		call_viaAS(func, objPointer);
	}
	else {
		switch(sys->callConv) {
#ifdef JIT_64
		case ICC_CDECL:
		case ICC_STDCALL:
			call_64conv(sys, func, 0, OP_None); break;
		case ICC_CDECL_OBJLAST:
			call_64conv(sys, func, objPointer, OP_Last); break;
		case ICC_CDECL_OBJFIRST:
			call_64conv(sys, func, objPointer, OP_First); break;
		case ICC_THISCALL:
		case ICC_THISCALL_RETURNINMEM:
#ifdef _MSC_VER
			call_64conv(sys, func, objPointer, OP_This); break;
#else
			call_64conv(sys, func, objPointer, OP_First); break;
#endif
#else
		case ICC_CDECL:
			call_cdecl(sys, func); break;
		case ICC_STDCALL:
			call_stdcall(sys, func); break;
		case ICC_THISCALL:
		case ICC_THISCALL_RETURNINMEM:
			call_thiscall(sys, func, objPointer); break;
		case ICC_CDECL_OBJLAST:
			call_cdecl_obj(sys, func, objPointer, true); break;
		case ICC_CDECL_OBJFIRST:
			call_cdecl_obj(sys, func, objPointer, false); break;
#endif
		case ICC_GENERIC_FUNC:
		case ICC_GENERIC_FUNC_RETURNINMEM:
		case ICC_GENERIC_METHOD:
		case ICC_GENERIC_METHOD_RETURNINMEM:
		case ICC_VIRTUAL_THISCALL:
			call_viaAS(func, objPointer); break;
		default:
			//Probably can't reach here, but handle it anyway
#ifdef JIT_PRINT_UNHANDLED_CALLS
			unhandled();
#endif
			call_viaAS(func, objPointer); break;
		}
	}
}

void SystemCall::call_entry(asSSystemFunctionInterface* func, asCScriptFunction* sFunc) {
	unsigned pBits = sizeof(void*) * 8;
#ifdef JIT_64
	Register esi(cpu,R13,pBits);
#else
	Register esi(cpu,ESI,pBits);
#endif
	Register ebp(cpu,EBP), esp(cpu,ESP,pBits);
	Register pax(cpu,EAX,pBits);

	if(!(flags & JIT_SYSCALL_FPU_NORESET))
		fpu.init();

	as<void*>(*ebp + offsetof(asSVMRegisters,programPointer)) = pOp;
	as<void*>(*ebp + offsetof(asSVMRegisters,stackPointer)) = esi;

	if(!(flags & JIT_SYSCALL_NO_ERRORS)) {
		pax = as<void*>(*ebp + offsetof(asSVMRegisters,ctx));
		pax += offsetof(asCContext,m_callingSystemFunction); //&callingSystemFunction
		as<void*>(*pax) = sFunc;

		as<void*>(*esp + local::pIsSystem) = pax;
	}
}

//Undoes things performed in call_entry in the case of an error
void SystemCall::call_error() {
	Register pax(cpu,EAX,sizeof(void*)*8), esp(cpu,ESP);

	if(!(flags & JIT_SYSCALL_NO_ERRORS)) {
		pax = as<void*>(*esp + local::pIsSystem);
		as<void*>(*pax) = (void*)0;
	}
}

void SystemCall::call_exit(asSSystemFunctionInterface* func) {
	Register eax(cpu,EAX), edx(cpu,EDX), esp(cpu,ESP), ebp(cpu,EBP), cl(cpu,ECX,8);
	Register pax(cpu,EAX,sizeof(void*)*8);

	
	if((flags & JIT_SYSCALL_NO_ERRORS) == 0) {
		//Clear IsSystem*
		pax = as<void*>(*esp + local::pIsSystem);
		as<void*>(*pax) = (void*)0;
	}

	if((flags & JIT_SYSCALL_NO_ERRORS) == 0 || (flags & JIT_NO_SUSPEND) == 0) {
		cl = as<bool>(*ebp+offsetof(asSVMRegisters,doProcessSuspend));
		cl &= cl;
		auto* dontSuspend = cpu.prep_short_jump(Zero);
			
			pax = as<void*>(*ebp+offsetof(asSVMRegisters,ctx));

			if((flags & JIT_SYSCALL_NO_ERRORS) == 0) {
				edx = as<int>(*pax+offsetof(asCContext,m_status));
				edx == (int)asEXECUTION_ACTIVE;
				auto* activeContext = cpu.prep_short_jump(Equal);
				returnHandler(Jump);
				cpu.end_short_jump(activeContext);
			}

			if((flags & JIT_NO_SUSPEND) == 0) {
				cl = as<bool>(*pax+offsetof(asCContext,m_doSuspend));
				cl &= cl;
				auto* noSuspend = cpu.prep_short_jump(Zero);

				as<int>(*pax+offsetof(asCContext,m_status)) = (int)asEXECUTION_SUSPENDED;
				returnHandler(Jump);

				cpu.end_short_jump(noSuspend);
			}
			
		cpu.end_short_jump(dontSuspend);
	}
}

#ifdef JIT_64
void SystemCall::call_64conv(asSSystemFunctionInterface* func,
		asCScriptFunction* sFunc, Register* objPointer, ObjectPosition pos) {

	void* op = (void*)cpu.op;
	Register eax(cpu, EAX), edx(cpu, EDX);
	Register xmm0(cpu, XMM0), xmm1(cpu, XMM1);
	Register pax(cpu, EAX, sizeof(void*) * 8), esp(cpu, ESP, sizeof(void*) * 8);
	Register esi(cpu, R13, sizeof(void*) * 8), ebx(cpu, EBX, sizeof(void*) * 8);
	Register temp(cpu, R10, sizeof(void*) * 8), ebp(cpu, EBP, sizeof(void*) * 8);

	call_entry(func, sFunc);

	int argCount = sFunc->parameterTypes.GetLength();
	unsigned stackBytes = 0;
	unsigned argOffset = 0;
	bool stackObject = false;

	int intCount = 0;
	int floatCount = 0;
	int i = 0, a = 0;
	bool retPointer = false;
	bool retOnStack = false;
	int firstPos = 0;

	//if(sFunc->name == "draw")
	//	bool f = false;
	
	//'this' before 'return pointer' on MSVC
	if(pos == OP_This) {
		Register reg = as<void*>(cpu.intArg64(0, 0));
		if(objPointer) {
			reg = *objPointer;
		}
		else {
			reg = as<void*>(*esi);
			argOffset += sizeof(void*);
			stackObject = true;
		}
		reg &= reg;
		returnHandler(Zero);
		reg += func->baseOffset;

		++intCount;
		++a;
		firstPos = 1;
	}

	if(sFunc->DoesReturnOnStack()) {
		Register arg0 = as<void*>(cpu.intArg64(firstPos, firstPos, pax));
		if(pos == OP_None || objPointer)
			arg0 = as<void*>(*esi);
		else
			arg0 = as<void*>(*esi + sizeof(asPWORD));
		as<void*>(*esp + local::retPointer) = arg0;
		retPointer = true;
		argOffset += sizeof(void*);

		if(func->hostReturnInMemory) {
			if(!cpu.isIntArg64Register(firstPos, firstPos)) {
				stackBytes += cpu.pushSize();
				retOnStack = true;
			}

			++intCount;
			++a;

			firstPos += 1;
		}
	}

	if(pos == OP_First) {
		if(!cpu.isIntArg64Register(firstPos, firstPos))
			stackBytes += cpu.pushSize();

		++intCount;
		++a;
	}

	for(; i < argCount; ++i, ++a) {
		auto& type = sFunc->parameterTypes[i];

		if(type.GetTokenType() == ttQuestion) {
			if(!cpu.isIntArg64Register(intCount, a))
				stackBytes += cpu.pushSize();
			++intCount; ++a;
			if(!cpu.isIntArg64Register(intCount, a))
				stackBytes += cpu.pushSize();
			++intCount;

			argOffset += sizeof(void*);
			argOffset += sizeof(int);
		}
		else if(type.IsReference() || type.IsObjectHandle()) {
			if(!cpu.isIntArg64Register(intCount, a))
				stackBytes += cpu.pushSize();
			++intCount;
			argOffset += sizeof(void*);
		}
		else if(type.IsFloatType()) {
			if(!cpu.isFloatArg64Register(floatCount, a))
				stackBytes += cpu.pushSize();
			++floatCount;
			argOffset += sizeof(float);
		}
		else if(type.IsDoubleType()) {
			if(!cpu.isFloatArg64Register(floatCount, a))
				stackBytes += cpu.pushSize();
			++floatCount;
			argOffset += sizeof(double);
		}
		else if(type.IsPrimitive()) {
			if(!cpu.isIntArg64Register(intCount, a))
				stackBytes += cpu.pushSize();
			++intCount;
			argOffset += type.GetSizeOnStackDWords() * sizeof(asDWORD);
		}
		else {
			throw "Unsupported argument type in system call.";
		}
	}

	--i; --a; --intCount; --floatCount;
	cpu.call_cdecl_prep(stackBytes);

	if(pos != OP_None && pos != OP_This) {
		if(objPointer) {
			*objPointer &= *objPointer;
			returnHandler(Zero);

			if(pos == OP_First) {
				if(cpu.isIntArg64Register(firstPos, firstPos)) {
					Register reg = as<void*>(cpu.intArg64(firstPos, firstPos));
					reg = as<void*>(*objPointer);
				}
				else {
					temp = *objPointer;
				}
			}
			else if(pos == OP_Last) {
				if(cpu.isIntArg64Register(intCount+1, a+1)) {
					Register reg = as<void*>(cpu.intArg64(intCount+1, a+1));
					reg = as<void*>(*objPointer);
				}
				else {
					cpu.push(*objPointer);
				}
			}
		}
		else {
			stackObject = true;

			if(pos == OP_First) {
				if(cpu.isIntArg64Register(firstPos, firstPos)) {
					Register reg = as<void*>(cpu.intArg64(firstPos, firstPos));
					reg = as<void*>(*esi);
					reg &= reg;
					returnHandler(Zero);

					reg += func->baseOffset;
				}
				else {
					temp = as<void*>(*esi);
					temp &= temp;
					returnHandler(Zero);

					temp += func->baseOffset;
				}
			}
			else if(pos == OP_Last) {
				if(cpu.isIntArg64Register(intCount+1, a+1)) {
					Register reg = as<void*>(cpu.intArg64(intCount+1, a+1));
					reg = as<void*>(*esi);
					reg &= reg;
					returnHandler(Zero);

					reg += func->baseOffset;
				}
				else {
					temp = as<void*>(*esi);
					temp &= temp;
					returnHandler(Zero);

					temp += func->baseOffset;
					cpu.push(temp);
				}
			}

			argOffset += sizeof(void*);
		}

	}

	auto Arg = [&](Register* reg, bool dword) {
		if(dword)
			argOffset -= sizeof(asDWORD);
		else
			argOffset -= sizeof(asQWORD);

		if(reg) {
			if(dword)
				as<asDWORD>(*reg) = as<asDWORD>(*esi+argOffset);
			else
				as<asQWORD>(*reg) = as<asQWORD>(*esi+argOffset);
		}
		else {
			if(dword)
				cpu.push(as<asDWORD>(*esi+argOffset));
			else
				cpu.push(as<asDWORD>(*esi+argOffset));
		}
	};

	auto IntArg = [&](bool dword) {
		if(cpu.isIntArg64Register(intCount, a)) {
			Register arg = cpu.intArg64(intCount, a);
			Arg(&arg, dword);
		}
		else
			Arg(0, dword);
		--intCount;
	};

	auto FloatArg = [&](bool dword) {
		if(cpu.isFloatArg64Register(floatCount, a)) {
			Register arg = cpu.floatArg64(floatCount, a);
			Arg(&arg, dword);
		}
		else
			Arg(0, dword);
		--floatCount;
	};


	for(; i >= 0; --i, --a) {
		auto& type = sFunc->parameterTypes[i];

		if(type.GetTokenType() == ttQuestion) {
			IntArg(true);
			--a;
			IntArg(false);
		}
		else if(type.IsReference() || type.IsObjectHandle()) {
			IntArg(false);
		}
		else if(type.IsFloatType()) {
			FloatArg(true);
		}
		else if(type.IsDoubleType()) {
			FloatArg(false);
		}
		else if(type.IsPrimitive()) {
			IntArg(type.GetSizeOnStackDWords() == 1);
		}
	}

	if(pos == OP_First && !cpu.isIntArg64Register(firstPos, firstPos))
		cpu.push(temp);
	if(retPointer && !cpu.isIntArg64Register(0, 0) && func->hostReturnInMemory)
		cpu.push(pax);

#ifdef _MSC_VER
	stackBytes += 32;
	esp -= 32;
#endif

	cpu.call((void*)func->func);

	cpu.call_cdecl_end(stackBytes, retOnStack);

	size_t addParams = 0;
	if(retPointer)
		addParams += sizeof(void*);
	if(stackObject)
		addParams += sizeof(void*);
	if(func->paramSize > 0 || addParams > 0)
		esi += func->paramSize * sizeof(asDWORD) + addParams;

	if(sFunc->returnType.IsObject() && !sFunc->returnType.IsReference()) {
		if(sFunc->returnType.IsObjectHandle()) {
			Register ret = as<void*>(cpu.intReturn64());
			as<void*>(*ebp + offsetof(asSVMRegisters,objectRegister)) = ret;

			//Add reference for returned auto handle
			if(func->returnAutoHandle) {
				ret &= ret;
				auto noGrab = cpu.prep_short_jump(Zero);
				
				int addref = sFunc->returnType.GetObjectType()->beh.addref;
				asCScriptFunction* addrefFunc = (asCScriptFunction*)sFunc->GetEngine()->GetFunctionById(addref);

				cpu.call_stdcall((void*)engineCallMethod, "prp", sFunc->GetEngine(), &ret, addrefFunc);

				cpu.end_short_jump(noGrab);
			}
		}
		else {
			//Recover ret pointer
			temp = as<void*>(*esp + local::retPointer);

			//Store value
			if(!func->hostReturnInMemory) {
				if(func->hostReturnFloat) {
					if(func->hostReturnSize == 1) {
						as<float>(*temp) = as<float>(xmm0);
					}
					else if(func->hostReturnSize == 2) {
						as<double>(*temp) = as<double>(xmm0);
					}
					else if(func->hostReturnSize == 3) {
						as<double>(*temp) = as<double>(xmm0);
						temp += 8;
						as<float>(*temp) = as<float>(xmm1);
					}
					else if(func->hostReturnSize == 4) {
						as<double>(*temp) = as<double>(xmm0);
						temp += 8;
						as<double>(*temp) = as<double>(xmm1);
					}
					else {
						throw "Not supported.";
					}
				}
				else {
					if(func->hostReturnSize == 1) {
						as<asDWORD>(*temp) = as<asDWORD>(eax);
					}
					else if(func->hostReturnSize == 2) {
						as<asQWORD>(*temp) = as<asQWORD>(eax);
					}
					else if(func->hostReturnSize == 3) {
						as<asQWORD>(*temp) = as<asQWORD>(eax);
						temp += 8;
						as<asDWORD>(*temp) = as<asDWORD>(edx);
					}
					else if(func->hostReturnSize == 4) {
						as<asQWORD>(*temp) = as<asQWORD>(eax);
						temp += 8;
						as<asQWORD>(*temp) = as<asQWORD>(edx);
					}
					else {
						throw "Not supported.";
					}
				}
			}

			//Technically need to clear the objectRegister
			//However, anything that tries to read this when it isn't valid is making a mistake
			//as<void*>(*ebp + offsetof(asSVMRegisters,objectRegister)) = nullptr;
			int destruct = sFunc->returnType.GetObjectType()->beh.destruct;
			if(destruct) {
				asCScriptFunction* destructFunc = (asCScriptFunction*)sFunc->GetEngine()->GetFunctionById(destruct);

				Register arg0 = as<void*>(cpu.intArg64(0, 0));
				arg0 = as<void*>(*ebp+offsetof(asSVMRegisters,ctx));
				eax = as<int>(*arg0+offsetof(asCContext,m_status));
				eax == (int)asEXECUTION_EXCEPTION;
				auto noError = cpu.prep_short_jump(NotEqual);

				cpu.call_stdcall((void*)engineCallMethod, "prp", sFunc->GetEngine(), &temp, destructFunc);

				cpu.end_short_jump(noError);
			}
		}
	}
	else if(func->hostReturnSize > 0) {
		if(func->hostReturnFloat) {
			Register ret = cpu.floatReturn64();
			if(func->hostReturnSize == 1) {
				esp -= cpu.pushSize();
				as<float>(*esp) = as<float>(ret);
				as<float>(ebx) = as<float>(*esp);
				esp += cpu.pushSize();
			}
			else if(func->hostReturnSize == 2) {
				esp -= cpu.pushSize();
				as<double>(*esp) = as<double>(ret);
				cpu.pop(ebx);
			}
			else
				throw "Not supported.";
		}
		else {
			if(func->hostReturnSize == 1)
				as<uint32_t>(ebx) = as<uint32_t>(cpu.intReturn64());
			else if(func->hostReturnSize == 2)
				as<uint64_t>(ebx) = as<uint64_t>(cpu.intReturn64());
			else
				throw "Not supported.";
		}
	}

	call_exit(func);

#ifdef JIT_DEBUG
	pax = (void*)&DBG_LastCall;
	*pax = (void*)op;
#endif
}
#else
void SystemCall::call_getReturn(asSSystemFunctionInterface* func, asCScriptFunction* sFunc) {
	Register eax(cpu,EAX), ecx(cpu,ECX), ebx(cpu,EBX), edx(cpu,EDX), ebp(cpu,EBP), esp(cpu,ESP);

	if(sFunc->returnType.IsObject() && !sFunc->returnType.IsReference()) {
		if(sFunc->returnType.IsObjectHandle()) {
			as<void*>(*ebp + offsetof(asSVMRegisters,objectRegister)) = eax;

			//Add reference for returned auto handle
			if( func->returnAutoHandle ) {
				eax &= eax;
				auto noGrab = cpu.prep_short_jump(Zero);
				
				int addref = sFunc->returnType.GetObjectType()->beh.addref;
				asCScriptFunction* addrefFunc = (asCScriptFunction*)sFunc->GetEngine()->GetFunctionById(addref);

				cpu.call_stdcall((void*)engineCallMethod, "prp", sFunc->GetEngine(), &eax, addrefFunc);

				cpu.end_short_jump(noGrab);
			}
		}
		else {
			//Recover ret pointer
			ecx = as<void*>(*esp + local::retPointer);

			//Store value
			if(!func->hostReturnInMemory) {
				if(func->hostReturnSize >= 1)
					*ecx = eax;
				
				if(func->hostReturnSize == 2) {
					ecx += 4;
					*ecx = edx;
				}
			}

			if(sFunc->DoesReturnOnStack()) {
				//Technically need to clear the objectRegister
				//However, anything that tries to read this when it isn't valid is making a mistake
				//as<void*>(*ebp + offsetof(asSVMRegisters,objectRegister)) = nullptr;
				int destruct = sFunc->returnType.GetObjectType()->beh.destruct;
				if(destruct) {
					asCScriptFunction* destructFunc = (asCScriptFunction*)sFunc->GetEngine()->GetFunctionById(destruct);

					edx = as<void*>(*ebp+offsetof(asSVMRegisters,ctx));
					eax = as<int>(*edx+offsetof(asCContext,m_status));
					eax == (int)asEXECUTION_EXCEPTION;
					auto noError = cpu.prep_short_jump(NotEqual);

					cpu.call_stdcall((void*)engineCallMethod, "prp", sFunc->GetEngine(), &ecx, destructFunc);

					cpu.end_short_jump(noError);
				}
			}
			else {
				//Store object pointer
				as<void*>(*ebp + offsetof(asSVMRegisters,objectRegister)) = ecx;
			}
		}
	}
	else if(func->hostReturnSize > 0) {
		if(func->hostReturnFloat) {
			if(func->hostReturnSize == 1) {
				esp -= cpu.pushSize();
				fpu.store_float(*esp);
				cpu.pop(ebx);
			}
			else {
				fpu.store_double(*ebp+offsetof(asSVMRegisters,valueRegister));
				ebx = *ebp+offsetof(asSVMRegisters,valueRegister);
			}
		}
		else {
			if(func->hostReturnSize == 1) {
				ebx = eax;
			}
			else {
				ebx = eax;
				*ebp+offsetof(asSVMRegisters,valueRegister)+4 = edx;
			}
		}
	}
}

void SystemCall::call_stdcall(asSSystemFunctionInterface* func, asCScriptFunction* sFunc) {
	Register eax(cpu,EAX), ebx(cpu,EBX), edx(cpu,EDX), esp(cpu,ESP), esi(cpu,ESI);
	Register cl(cpu,ECX,8);

	call_entry(func,sFunc);

	int firstArg = 0, lastArg = func->paramSize;
	unsigned popCount = func->paramSize * sizeof(asDWORD);

	//Copy out retPointer; will be pushed normally as an argument in correct order
	if(sFunc->DoesReturnOnStack()) {
		eax = as<void*>(*esi);
		as<void*>(*esp + local::retPointer) = eax;
		lastArg += 1; popCount += sizeof(asDWORD);
	}

	for(int i = lastArg-1; i >= firstArg; --i)
		cpu.push(*esi+(i*sizeof(asDWORD)));

	cpu.call((void*)func->func);

	if(popCount > 0)
		esi += popCount;

	call_getReturn(func,sFunc);

	call_exit(func);
}
	
void SystemCall::call_cdecl(asSSystemFunctionInterface* func, asCScriptFunction* sFunc) {
	Register eax(cpu,EAX), ebx(cpu,EBX), edx(cpu,EDX), esp(cpu,ESP), esi(cpu,ESI);
	Register cl(cpu,ECX,8);

	call_entry(func,sFunc);

	int firstArg = 0, lastArg = func->paramSize;
	unsigned popCount = func->paramSize * sizeof(asDWORD);
	
	//Copy out retPointer; will be pushed normally as an argument in correct order
	if(sFunc->DoesReturnOnStack()) {
		eax = as<void*>(*esi);
		as<void*>(*esp + local::retPointer) = eax;
		lastArg += 1; popCount += sizeof(asDWORD);
	}

	int argBytes = (lastArg-firstArg) * cpu.pushSize();
	cpu.call_cdecl_prep(argBytes);

	for(int i = lastArg-1; i >= firstArg; --i)
		cpu.push(*esi+(i*sizeof(asDWORD)));

	cpu.call((void*)func->func);
	cpu.call_cdecl_end(argBytes, sFunc->DoesReturnOnStack());

	if(popCount > 0)
		esi += popCount;

	call_getReturn(func,sFunc);

	call_exit(func);
}


void SystemCall::call_cdecl_obj(asSSystemFunctionInterface* func, asCScriptFunction* sFunc, Register* objPointer, bool last) {
	Register eax(cpu,EAX), ebx(cpu,EBX), ecx(cpu,ECX), edx(cpu,EDX), esp(cpu,ESP), esi(cpu,ESI);
	Register cl(cpu,ECX,8);

	call_entry(func,sFunc);

	int firstArg = 0, lastArg = func->paramSize;
	int argBytes = (lastArg-firstArg + 1) * cpu.pushSize();
	unsigned popCount = func->paramSize * sizeof(asDWORD);

	if(!objPointer) {
		firstArg = 1;
		lastArg += 1;
		popCount += sizeof(void*);
	}

	//retPointer takes up an extra space
	if(sFunc->DoesReturnOnStack()) {
		argBytes += sizeof(asDWORD);
		popCount += sizeof(asDWORD);

		//Copy out retPointer
		edx = as<void*>(*esi + (firstArg * sizeof(asDWORD)));
		as<void*>(*esp + local::retPointer) = edx;

		firstArg += 1; lastArg += 1;
	}

	cpu.call_cdecl_prep(argBytes);

	if(objPointer) {
		*objPointer &= *objPointer;

		auto j = cpu.prep_short_jump(NotZero);
			call_error();
			returnHandler(Jump);
		cpu.end_short_jump(j);

		if(last)
			cpu.push(*objPointer);
	}
	else {
		ecx = as<void*>(*esi);
		ecx &= ecx;

		auto j = cpu.prep_short_jump(NotZero);
			call_error();
			returnHandler(Jump);
		cpu.end_short_jump(j);

		ecx += func->baseOffset;
		if(last)
			cpu.push(ecx);
	}

	for(int i = lastArg-1; i >= firstArg; --i)
		cpu.push(*esi+(i*sizeof(asDWORD)));

	if(!last)
		if(objPointer)
			cpu.push(*objPointer);
		else
			cpu.push(ecx);

	//retPointer is always last thing pushed
	if(sFunc->DoesReturnOnStack())
		cpu.push(edx);

	cpu.call((void*)func->func);
	cpu.call_cdecl_end(argBytes, sFunc->DoesReturnOnStack());

	if(popCount > 0)
		esi += popCount;

	call_getReturn(func,sFunc);

	call_exit(func);
}

void SystemCall::call_thiscall(asSSystemFunctionInterface* func, asCScriptFunction* sFunc, Register* objPointer) {
	Register eax(cpu,EAX), ebx(cpu,EBX), ecx(cpu,ECX), edx(cpu,EDX), esp(cpu,ESP), esi(cpu,ESI);
	Register cl(cpu,ECX,8);

	call_entry(func,sFunc);

	int firstArg = 0, lastArg = func->paramSize, argBytes;

	if(objPointer) {
		*objPointer &= *objPointer;
		auto j = cpu.prep_short_jump(NotZero);
			call_error();
			returnHandler(Jump);
		cpu.end_short_jump(j);
	}
	else {
		ecx = as<void*>(*esi);
		firstArg = 1; lastArg += 1;

		ecx &= ecx;
		auto j = cpu.prep_short_jump(NotZero);
			call_error();
			returnHandler(Jump);
		cpu.end_short_jump(j);

		ecx += func->baseOffset;
	}
	argBytes = (lastArg-firstArg) * cpu.pushSize();
	cpu.call_thiscall_prep(argBytes);

	//Get return pointer
	if(sFunc->DoesReturnOnStack()) {
		edx = as<void*>(*esi+(firstArg * sizeof(asDWORD)));
		as<void*>(*esp + local::retPointer) = edx;
		firstArg += 1; lastArg += 1;
	}

	for(int i = lastArg-1; i >= firstArg; --i)
		cpu.push(*esi+(i*sizeof(asDWORD)));

	if(!sFunc->DoesReturnOnStack()) {
		if(objPointer)
			cpu.call_thiscall_this(*objPointer);
		else
			cpu.call_thiscall_this(*esi);
	}
	else {
		if(objPointer)
			cpu.call_thiscall_this_mem(*objPointer, edx);
		else
			cpu.call_thiscall_this_mem(*esi, edx);
	}

	cpu.call((void*)func->func);
	cpu.call_thiscall_end(argBytes);

	unsigned popCount = func->paramSize * sizeof(asDWORD);
	if(!objPointer)
		popCount += sizeof(void*);
	if(sFunc->DoesReturnOnStack())
		popCount += sizeof(void*);
	if(popCount > 0)
		esi += popCount;

	call_getReturn(func,sFunc);

	call_exit(func);
}
#endif

void SystemCall::call_viaAS(asCScriptFunction* func, Register* objPointer) {
	unsigned pBits = sizeof(void*) * 8;
#ifdef JIT_64
	Register esi(cpu,R13,pBits);
#else
	Register esi(cpu,ESI,pBits);
#endif
	Register ebp(cpu,EBP,pBits), pax(cpu,EAX,pBits), esp(cpu,ESP,pBits), ebx(cpu,EBX);
	Register cl(cpu,ECX,8);

	//Copy state to VM state in case the call inspects the context
	call_entry(func->sysFuncIntf,func);

	MemAddress ctxPtr(as<void*>(*ebp + offsetof(asSVMRegisters,ctx)));

	int stdcall callSysWrapper(int id, asIScriptContext* ctx, void* obj);

	if(objPointer)
		cpu.call_stdcall((void*)callSysWrapper,"cmr",func->GetId(),&ctxPtr,objPointer);
	else
		cpu.call_stdcall((void*)callSysWrapper,"cmp",func->GetId(),&ctxPtr,nullptr);
	esi += pax;

	int stdcall sysExit(asSVMRegisters* registers);
	cpu.call_stdcall((void*)sysExit,"r",&ebp);

	//Check that there is a return in the valueRegister
	bool isGeneric = func->sysFuncIntf->callConv == ICC_GENERIC_FUNC || func->sysFuncIntf->callConv == ICC_GENERIC_FUNC_RETURNINMEM
		|| func->sysFuncIntf->callConv == ICC_GENERIC_METHOD || func->sysFuncIntf->callConv == ICC_GENERIC_METHOD_RETURNINMEM;

	if(((func->sysFuncIntf->hostReturnSize >= 1 && !func->sysFuncIntf->hostReturnInMemory) || isGeneric)
	 	&& !(func->returnType.IsObject() && !func->returnType.IsReference()) ) {

#ifdef JIT_64
		as<asQWORD>(ebx) = as<asQWORD>(*ebp + offsetof(asSVMRegisters,valueRegister));
#else
		ebx = *ebp + offsetof(asSVMRegisters,valueRegister);
#endif
	}

	call_exit(func->sysFuncIntf);
}

int stdcall sysExit(asSVMRegisters* registers) {
	if(registers->doProcessSuspend) {
		asCContext* context = (asCContext*)registers->ctx;
		if(context->m_status != asEXECUTION_ACTIVE) {
			return 1;
		}
		else if(context->m_doSuspend) {
			context->m_status = asEXECUTION_SUSPENDED;
			return 1;
		}
	}
	return 0;
}

int stdcall callSysWrapper(int id, asIScriptContext* ctx, void* obj) {
	return CallSystemFunction(id, (asCContext*)ctx, obj) * sizeof(asDWORD);
}

void stdcall returnScriptFunction(asCContext* ctx) {
	// Return if this was the first function, or a nested execution
	if( ctx->m_callStack.GetLength() == 0 ||
		ctx->m_callStack[ctx->m_callStack.GetLength() - 5] == 0 )
	{
		ctx->m_status = asEXECUTION_FINISHED;
		return;
	}

	ctx->PopCallState();
}
