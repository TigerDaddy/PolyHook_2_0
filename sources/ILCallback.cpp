#include "headers/Detour/ILCallback.hpp"

asmjit::CallConv::Id PLH::ILCallback::getCallConv(const std::string& conv) {
	if (conv == "cdecl") {
		return asmjit::CallConv::kIdHostCDecl;
	}else if (conv == "stdcall") {
		return asmjit::CallConv::kIdHostStdCall;
	}else if (conv == "fastcall") {
		return asmjit::CallConv::kIdHostFastCall;
	} 
	return asmjit::CallConv::kIdHost;
}

#define TYPEID_MATCH_STR_IF(var, T) if (var == #T) { return asmjit::Type::IdOfT<T>::kTypeId; }
#define TYPEID_MATCH_STR_ELSEIF(var, T)  else if (var == #T) { return asmjit::Type::IdOfT<T>::kTypeId; }

uint8_t PLH::ILCallback::getTypeId(const std::string& type) {
	if (type.find("*") != std::string::npos) {
		return asmjit::Type::kIdUIntPtr;
	}

	TYPEID_MATCH_STR_IF(type, signed char)
	TYPEID_MATCH_STR_ELSEIF(type, unsigned char)
	TYPEID_MATCH_STR_ELSEIF(type, short)
	TYPEID_MATCH_STR_ELSEIF(type, unsigned short)
	TYPEID_MATCH_STR_ELSEIF(type, int)
	TYPEID_MATCH_STR_ELSEIF(type, unsigned int)
	TYPEID_MATCH_STR_ELSEIF(type, long)
	TYPEID_MATCH_STR_ELSEIF(type, unsigned long)
	TYPEID_MATCH_STR_ELSEIF(type, __int64)
	TYPEID_MATCH_STR_ELSEIF(type, unsigned __int64)
	TYPEID_MATCH_STR_ELSEIF(type, long long)
	TYPEID_MATCH_STR_ELSEIF(type, unsigned long long)
	TYPEID_MATCH_STR_ELSEIF(type, char)
	TYPEID_MATCH_STR_ELSEIF(type, char16_t)
	TYPEID_MATCH_STR_ELSEIF(type, char32_t)
	TYPEID_MATCH_STR_ELSEIF(type, wchar_t)
	TYPEID_MATCH_STR_ELSEIF(type, uint8_t)
	TYPEID_MATCH_STR_ELSEIF(type, int8_t)
	TYPEID_MATCH_STR_ELSEIF(type, uint16_t)
	TYPEID_MATCH_STR_ELSEIF(type, int16_t)
	TYPEID_MATCH_STR_ELSEIF(type, int32_t)
	TYPEID_MATCH_STR_ELSEIF(type, uint32_t)
	TYPEID_MATCH_STR_ELSEIF(type, uint64_t)
	TYPEID_MATCH_STR_ELSEIF(type, int64_t)
	TYPEID_MATCH_STR_ELSEIF(type, float)
	TYPEID_MATCH_STR_ELSEIF(type, double)
	TYPEID_MATCH_STR_ELSEIF(type, bool)
	TYPEID_MATCH_STR_ELSEIF(type, void)
	else if (type == "intptr_t") {
		return asmjit::Type::kIdIntPtr;
	}else if (type == "uintptr_t") {
		return asmjit::Type::kIdUIntPtr;
	} 

	return asmjit::Type::kIdVoid;
}

uint64_t PLH::ILCallback::getJitFunc(const asmjit::FuncSignature& sig, const PLH::ILCallback::tUserCallback callback, const uint64_t retAddr /* = 0 */) {
	UNREFERENCED_PARAMETER(retAddr);
	/*AsmJit is smart enough to track register allocations and will forward
	  the proper registers the right values and fixup any it dirtied earlier.
	  This can only be done if it knows the signature, and ABI, so we give it 
	  them. It also only does this mapping for calls, so we need to generate 
	  calls on our boundaries of transfers when we want argument order correct
	  (ABI stuff is managed for us when calling C code within this project via host mode).
	  It also does stack operations for us including alignment, shadow space, and
	  arguments, everything really. Manual stack push/pop is not supported using
	  the AsmJit compiler, so we must create those nodes, and insert them into
	  the Node list manually to not corrupt the compiler's tracking of things.
	*/
	asmjit::CodeHolder code;                      
	code.init(asmjit::CodeInfo(asmjit::ArchInfo::kIdHost));			
	
	// initialize function
	asmjit::x86::Compiler cc(&code);            
	asmjit::FuncNode* func = cc.addFunc(sig);              

	asmjit::FileLogger log(stdout);
	uint32_t kFormatFlags = asmjit::FormatOptions::kFlagMachineCode | asmjit::FormatOptions::kFlagExplainImms | asmjit::FormatOptions::kFlagRegCasts 
		| asmjit::FormatOptions::kFlagAnnotations | asmjit::FormatOptions::kFlagDebugPasses | asmjit::FormatOptions::kFlagDebugRA
		| asmjit::FormatOptions::kFlagHexImms | asmjit::FormatOptions::kFlagHexOffsets | asmjit::FormatOptions::kFlagPositions;
	
	log.addFlags(kFormatFlags);
	code.setLogger(&log);
	
	// too small to really need it
	func->frame().resetPreservedFP();
	
	// map argument slots to registers, following abi.
	std::vector<asmjit::x86::Reg> argRegisters;
	for (uint8_t arg_idx = 0; arg_idx < sig.argCount(); arg_idx++) {
		const uint8_t argType = sig.args()[arg_idx];

		asmjit::x86::Reg arg;
		if (isGeneralReg(argType)) {
			arg = cc.newInt32();
		} else if (isXmmReg(argType)) {
			arg = cc.newXmm();
		} else {
			ErrorLog::singleton().push("Parameters wider than 64bits not supported", ErrorLevel::SEV);
			return 0;
		}

		cc.setArg(arg_idx, arg);
		argRegisters.push_back(arg);
	}
  
	// setup the stack structure to hold arguments for user callback
	argsStack = cc.newStack((uint32_t)(sizeof(uint64_t) * sig.argCount()), 4);
	asmjit::x86::Mem argsStackIdx(argsStack);               

	// assigns some register as index reg 
	asmjit::x86::Gp i = cc.newUIntPtr();

	// stackIdx <- stack[i].
	argsStackIdx.setIndex(i);                   

	// r/w are sizeof(uint64_t) width now
	argsStackIdx.setSize(sizeof(uint64_t));
	
	// set i = 0
	cc.mov(i, 0);  
	UNREFERENCED_PARAMETER(callback);
	//// mov from arguments registers into the stack structure
	for (uint8_t arg_idx = 0; arg_idx < sig.argCount(); arg_idx++) {
		const uint8_t argType = sig.args()[arg_idx];

		// have to cast back to explicit register types to gen right mov type
		if (isGeneralReg(argType)) {
			cc.mov(argsStackIdx, argRegisters.at(arg_idx).as<asmjit::x86::Gp>());
		} else if(isXmmReg(argType)) {
			cc.movq(argsStackIdx, argRegisters.at(arg_idx).as<asmjit::x86::Xmm>());
		} else {
			ErrorLog::singleton().push("Parameters wider than 64bits not supported", ErrorLevel::SEV);
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, sizeof(uint64_t));
	}

	// get pointer to stack structure and pass it to the user callback
	asmjit::x86::Gp argStruct = cc.newUIntPtr("argStruct");
	cc.lea(argStruct, argsStack);

	// fill reg to pass struct arg count to callback
	asmjit::x86::Gp argCountParam = cc.newU8();
	cc.mov(argCountParam, (uint8_t)sig.argCount());

	// call to user provided function (use ABI of host compiler)
	auto call = cc.call(asmjit::Imm(static_cast<int64_t>((intptr_t)callback)), asmjit::FuncSignatureT<void, Parameters*, uint8_t>(asmjit::CallConv::kIdHost));
	call->setArg(0, argStruct);
	call->setArg(1, argCountParam);

	// deref the trampoline ptr (holder must live longer, must be concrete reg since push later)
	asmjit::x86::Gp orig_ptr = cc.zbx();
	cc.mov(orig_ptr, (uintptr_t)getTrampolineHolder());
	cc.mov(orig_ptr, asmjit::x86::ptr(orig_ptr));

	auto orig_call = cc.call(orig_ptr, sig);
	for (uint8_t arg_idx = 0; arg_idx < sig.argCount(); arg_idx++) {
		orig_call->setArg(arg_idx, argRegisters.at(arg_idx));
	}

	unsigned char* retBufTmp = (unsigned char*)m_mem.getBlock(10);
	*(unsigned char*)retBufTmp = 0xC3;

	/*
	Spoof Method:
	mov [esp], newRetAddress (ret_jit_stub label)
	jmp orig_call
	mov [esp], oldRetAddress
	... stack cleanup ...
	ret
	*/
	asmjit::Label ret_jit_stub = cc.newLabel();
	asmjit::x86::Gp newRetAddr = cc.zcx();
	cc.lea(newRetAddr, asmjit::x86::ptr(ret_jit_stub));
	asmjit::x86::Gp retInstHolder = cc.zax();
	cc.mov(retInstHolder, (uintptr_t)retBufTmp);
	cc.xchg(asmjit::x86::ptr(cc.zsp()), retInstHolder);
	
	asmjit::BaseNode* spoofCursor = cc.cursor();

	/* Must do it this way to not corrupt because using compiler (stack operations + jmp out unsupported)*/
	asmjit::InstNode* pushRetAddr = cc.newInstNode(asmjit::x86::Inst::kIdPush, asmjit::BaseInst::Options::kOptionShortForm, newRetAddr);
	asmjit::InstNode* jmpOrigPtr = cc.newInstNode(asmjit::x86::Inst::kIdJmp, asmjit::BaseInst::Options::kOptionLongForm, orig_ptr);
	
	cc.xchg(asmjit::x86::ptr(cc.zsp()), retInstHolder);
	cc.bind(ret_jit_stub);
	cc.func()->frame().setAllDirty(asmjit::BaseReg::kGroupGp); // AsmJit bug?, workaround
	cc.endFunc();
	
	/*
		Optionally Spoof Return (TODO)
		finalize() Manually so we can mutate node list. In asmjit the compiler inserts implicit calculated 
		nodes around some instructions, such as call where it will emit implicit movs for params and stack stuff.
		We want to generate these so we emit a call, but we want to spoof the return address via a jmp, so we iterate 
		nodes and re-write the call with push, push, jmp. Only then can we serialize. Asmjit finalize applies
		optimization and reg assignment 'passes', then serializes via assembler (we do these steps manually).
	*/

	cc.runPasses();
	cc.removeNode(orig_call);
	cc.addAfter(pushRetAddr, spoofCursor);
	cc.addAfter(jmpOrigPtr, pushRetAddr);
	
	/* 
		Passes will also do virtual register allocations, which may be assigned multiple concrete
		registers throughout the lifetime of the function. So we must only emit raw assembly with
		concrete registers from this point on (after runPasses call).
	*/

	// write to buffer
	asmjit::x86::Assembler assembler(&code);
	cc.serialize(&assembler);

	// worst case, overestimates for case trampolines needed
	code.flatten();
	size_t size = code.codeSize();

	// Allocate a virtual memory (executable).
	m_callbackBuf = (uint64_t)m_mem.getBlock(size);
	if (!m_callbackBuf) {
		__debugbreak();
		return 0;
	}

	// if multiple sections, resolve linkage (1 atm)
	if (code.hasUnresolvedLinks()) {
		code.resolveUnresolvedLinks();
	}

	 // Relocate to the base-address of the allocated memory.
	code.relocateToBase(m_callbackBuf);
	code.copyFlattenedData((unsigned char*)m_callbackBuf, size);

	//ErrorLog::singleton().push("JIT Stub:\n" + std::string(log.data()), ErrorLevel::INFO);
	return m_callbackBuf;
}

uint64_t PLH::ILCallback::getJitFunc(const std::string& retType, const std::vector<std::string>& paramTypes, const tUserCallback callback, std::string callConv/* = ""*/, const uint64_t retAddr /* = 0 */) {
	asmjit::FuncSignature sig;
	std::vector<uint8_t> args;
	for (const std::string& s : paramTypes) {
		args.push_back(getTypeId(s));
	}
	sig.init(getCallConv(callConv),asmjit::FuncSignature::kNoVarArgs, getTypeId(retType), args.data(), (uint32_t)args.size());
	return getJitFunc(sig, callback, retAddr);
}

uint64_t* PLH::ILCallback::getTrampolineHolder() {
	return &m_trampolinePtr;
}

bool PLH::ILCallback::isGeneralReg(const uint8_t typeId) const {
	switch (typeId) {
	case asmjit::Type::kIdI8:
	case asmjit::Type::kIdU8:
	case asmjit::Type::kIdI16:
	case asmjit::Type::kIdU16:
	case asmjit::Type::kIdI32:
	case asmjit::Type::kIdU32:
	case asmjit::Type::kIdI64:
	case asmjit::Type::kIdU64:
	case asmjit::Type::kIdIntPtr:
	case asmjit::Type::kIdUIntPtr:
		return true;
	default:
		return false;
	}
}

bool PLH::ILCallback::isXmmReg(const uint8_t typeId) const {
	switch (typeId) {
	case  asmjit::Type::kIdF32:
	case asmjit::Type::kIdF64:
		return true;
	default:
		return false;
	}
}

PLH::ILCallback::ILCallback() : m_mem(0, 0) {
	m_callbackBuf = 0;
	m_trampolinePtr = 0;
}

PLH::ILCallback::~ILCallback() {
	
}