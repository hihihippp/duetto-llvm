//===-- DuettoWriter.cpp - The Duetto JavaScript generator -------------===//
//
//                     Duetto: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2011-2013 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "Relooper.h"
#include "llvm/Duetto/Utils.h"
#include "llvm/Duetto/Writer.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
using namespace std;
using namespace duetto;

class DuettoRenderInterface: public RenderInterface
{
private:
	DuettoWriter* writer;
public:
	DuettoRenderInterface(DuettoWriter* w):writer(w)
	{
	}
	void renderBlock(const void* privateBlock);
	void renderIfBlockBegin(const void* privateBlock, int branchId, bool first);
	void renderElseBlockBegin();
	void renderBlockEnd();
	void renderBlockPrologue(const void* privateBlockTo, const void* privateBlockFrom);
	void renderWhileBlockBegin();
	void renderWhileBlockBegin(int labelId);
	void renderDoBlockBegin();
	void renderDoBlockBegin(int labelId);
	void renderDoBlockEnd();
	void renderBreak();
	void renderBreak(int labelId);
	void renderContinue();
	void renderContinue(int labelId);
	void renderLabel(int labelId);
	void renderIfOnLabel(int labelId, bool first);
};

void DuettoWriter::handleBuiltinNamespace(const char* identifier, User::const_op_iterator it,
			User::const_op_iterator itE)
{
	const char* ident = identifier;
	//Read the class name
	char* className;
	int classLen = strtol(ident,&className,10);
	if(classLen == 0)
	{
		llvm::report_fatal_error(Twine("Unexpected C++ mangled name: ", StringRef(identifier)), false);
		return;
	}
	ident = className + classLen;

	//Read the function name
	char* funcName;
	int funcNameLen=strtol(ident,&funcName,10);
	if(funcNameLen==0)
	{
		//This means that we are parsing a fuction which is not in a class
		//and we already parsed the function name
		funcName = className;
		funcNameLen = classLen;
		className = NULL;
		classLen = 0;
	}
	//This condition is necessarily true
	assert(funcNameLen!=0);

	//The first arg should be the object
	if(strncmp(funcName,"get_",4)==0 && (itE-it)==1)
	{
		//Getter
		if(className == NULL)
		{
			llvm::report_fatal_error(Twine("Unexpected getter without class: ", StringRef(identifier)), false);
			return;
		}
		compileOperand(*it);
		stream << ".";
		stream.write(funcName+4,funcNameLen-4);
	}
	else if(strncmp(funcName,"set_",4)==0 && (itE-it)==2)
	{
		//Setter
		if(className == NULL)
		{
			llvm::report_fatal_error(Twine("Unexpected setter without class: ", StringRef(identifier)), false);
			return;
		}
		compileOperand(*it);
		++it;
		stream << ".";
		stream.write(funcName+4,funcNameLen-4);
		stream << " = ";
		compileOperand(*it);
	}
	else
	{
		//Regular call
		if(className)
		{
			if(it == itE)
			{
				llvm::report_fatal_error(Twine("At least 'this' parameter was expected: ", StringRef(identifier)), false);
				return;
			}
			compileOperand(*it);
			++it;
			stream << ".";
		}
		stream.write(funcName,funcNameLen);
		compileMethodArgs(it,itE);
	}
}

bool DuettoWriter::isBitCast(const Value* v) const
{
	const User* b=static_cast<const User*>(v);
	if(isa<BitCastInst>(v))
	{
		bool validCast = isValidTypeCast(v, b->getOperand(0), b->getOperand(0)->getType(), v->getType());
		if(!validCast)
		{
			llvm::errs() << "Error while handling cast " << *v << "\n";
			llvm::report_fatal_error("Unsupported code found, please report a bug", false);
			return false;
		}
		return true;
	}
	const ConstantExpr* ce=dyn_cast<const ConstantExpr>(v);
	if(ce && ce->getOpcode()==Instruction::BitCast)
	{
		bool validCast = isValidTypeCast(v, b->getOperand(0), b->getOperand(0)->getType(), v->getType());
		if(!validCast)
		{
			llvm::errs() << "Error while handling cast " << *v << "\n";
			llvm::report_fatal_error("Unsupported code found, please report a bug", false);
			return false;
		}
		return true;
	}
	return false;
}

bool DuettoWriter::isGEP(const Value* v) const
{
	if(GetElementPtrInst::classof(v))
		return true;
	const ConstantExpr* ce=dyn_cast<const ConstantExpr>(v);
	if(ce && ce->getOpcode()==Instruction::GetElementPtr)
		return true;
	return false;
}

void DuettoWriter::compileCopyRecursive(const std::string& baseName, const Value* baseDest,
		const Value* baseSrc, const Type* currentType, const char* namedOffset)
{
	switch(currentType->getTypeID())
	{
		case Type::IntegerTyID:
		case Type::FloatTyID:
		case Type::DoubleTyID:
		case Type::PointerTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			stream << baseName << " = ";
			compileDereferencePointer(baseSrc, NULL, namedOffset);
			stream << baseName << ";\n";
			break;
		}
		case Type::StructTyID:
		{
			if(isUnion(currentType))
			{
				stream << "__tmp__=new Int8Array(";
				compileDereferencePointer(baseDest, NULL, namedOffset);
				stream << baseName << ");\n";
				stream << "__tmp__.set(";
				stream << "new Int8Array(";
				compileDereferencePointer(baseSrc, NULL, namedOffset);
				stream << baseName << "));\n";
				break;
			}
			const StructType* st=static_cast<const StructType*>(currentType);
			StructType::element_iterator E=st->element_begin();
			StructType::element_iterator EE=st->element_end();
			uint32_t offset=0;
			for(;E!=EE;++E)
			{
				char buf[16];
				snprintf(buf,16,".a%u",offset);
				compileCopyRecursive(baseName+buf, baseDest, baseSrc, *E, namedOffset);
				offset++;
			}
			break;
		}
		case Type::ArrayTyID:
		{
			const ArrayType* at=static_cast<const ArrayType*>(currentType);
			char buf[16];
			for(uint64_t i=0;i<at->getNumElements();i++)
			{
				snprintf(buf,16,"[%lu]",i);
				compileCopyRecursive(baseName+buf, baseDest, baseSrc, at->getElementType(), namedOffset);
			}
			break;
		}
		default:
			llvm::errs() << "Support type in copy ";
			currentType->dump();
			llvm::errs() << '\n';
	}
}

void DuettoWriter::compileResetRecursive(const std::string& baseName, const Value* baseDest,
		const Value* resetValue, const Type* currentType, const char* namedOffset)
{
	switch(currentType->getTypeID())
	{
		case Type::IntegerTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			stream << baseName << " = ";
			if(Constant::classof(resetValue))
			{
				uint8_t constResetValue = getIntFromValue(resetValue);
				char buf[11];
				buf[10]=0;
				if(currentType->getIntegerBitWidth()==8)
					snprintf(buf,10,"0x%x",constResetValue);
				else if(currentType->getIntegerBitWidth()==16)
					snprintf(buf,10,"0x%x%x",constResetValue,constResetValue);
				else if(currentType->getIntegerBitWidth()==32)
				{
					snprintf(buf,10,"0x%x%x%x%x",
						constResetValue,constResetValue,constResetValue,constResetValue);
				}
				else
					llvm::report_fatal_error("Unsupported values for memset", false);
				stream << buf;
			}
			else
			{
				if(currentType->getIntegerBitWidth()!=8)
					llvm::report_fatal_error("Unsupported values for memset", false);
				compileOperand(resetValue);
			}
			stream << ";\n";
			break;
		}
		case Type::FloatTyID:
		case Type::DoubleTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			if(!Constant::classof(resetValue) || getIntFromValue(resetValue) != 0)
				llvm::report_fatal_error("Unsupported values for memset", false);
			stream << baseName << " = 0;\n";
			break;
		}
		case Type::PointerTyID:
		{
			compileDereferencePointer(baseDest, NULL, namedOffset);
			if(!Constant::classof(resetValue) || getIntFromValue(resetValue) != 0)
				llvm::report_fatal_error("Unsupported values for memset", false);
			//Pointers to client objects must use a normal null
			const Type* pointedType = currentType->getPointerElementType();
			stream << baseName << " = ";
			if(isClientType(pointedType))
				stream << "null";
			else
				stream << "nullObj";
			stream << ";\n";
			break;
		}
		case Type::StructTyID:
		{
			if(isUnion(currentType))
			{
				stream << "__tmp__=new Int8Array(";
				compileDereferencePointer(baseDest, NULL, namedOffset);
				stream << baseName << ");\n";
				stream << "for(var __i__=0;__i__<__tmp__.length;__i__++) __tmp__[__i__]=0\n";
				break;
			}
			const StructType* st=static_cast<const StructType*>(currentType);
			StructType::element_iterator E=st->element_begin();
			StructType::element_iterator EE=st->element_end();
			uint32_t offset=0;
			for(;E!=EE;++E)
			{
				char buf[16];
				snprintf(buf,16,".a%u",offset);
				compileResetRecursive(baseName+buf, baseDest, resetValue, *E, namedOffset);
				offset++;
			}
			break;
		}
		case Type::ArrayTyID:
		{
			const ArrayType* at=static_cast<const ArrayType*>(currentType);
			char buf[16];
			for(uint64_t i=0;i<at->getNumElements();i++)
			{
				snprintf(buf,16,"[%lu]",i);
				compileResetRecursive(baseName+buf, baseDest, resetValue, at->getElementType(), namedOffset);
			}
			break;
		}
		default:
			llvm::errs() << "Support type in reset ";
			currentType->dump();
			llvm::errs() << '\n';
	}
}

Type* DuettoWriter::findRealType(const Value* v, std::set<const PHINode*>& visitedPhis) const
{
	if(isBitCast(v))
		return static_cast<const User*>(v)->getOperand(0)->getType();

	const PHINode* newPHI=dyn_cast<const PHINode>(v);
	if(newPHI)
 	{
		if(visitedPhis.count(newPHI))
		{
			//Assume true, if needed it will become false later on
			return NULL;
		}
		visitedPhis.insert(newPHI);
		assert(newPHI->getNumIncomingValues()>=1);
		Type* ret=findRealType(newPHI->getIncomingValue(0),visitedPhis);
		for(unsigned i=1;i<newPHI->getNumIncomingValues();i++)
		{
			Type* t=findRealType(newPHI->getIncomingValue(i),visitedPhis);
			if(ret==NULL)
				ret=t;
			else if(ret!=t)
			{
				llvm::errs() << "Unconsistent real types for phi " << *v << "\n";
				llvm::report_fatal_error("Unsupported code found, please report a bug", false);
				return ret;
			}
		}
		visitedPhis.erase(newPHI);
		return ret;
 	}
	return v->getType();
}

void DuettoWriter::compileDowncast(const Value* src, uint32_t baseOffset)
{
	std::set<const PHINode*> visitedPhis;
	Type* pointerType=findRealType(src, visitedPhis);
	assert(pointerType->isPointerTy());
	Type* t=cast<PointerType>(pointerType)->getElementType();
	if(isClientType(t))
		compileOperand(src);
	else
	{
		//Do a runtime downcast
		compileDereferencePointer(src, NULL);
		stream << ".a[";
		compileDereferencePointer(src, NULL);
		stream << ".o-" << baseOffset << "]";
	}
}

void DuettoWriter::compileMove(const Value* dest, const Value* src, const Value* size)
{
	//TODO: Optimize the checks if possible
	//Check if they are inside the same memory island
	stream << "if(";
	const Type* lastTypeDest=compileObjectForPointer(dest, NORMAL);
	stream << "===";
	const Type* lastTypeSrc=compileObjectForPointer(src, NORMAL);
	//If so they may overlap, check and use reverse copy if needed
	stream << "&&";
	bool notFirst=compileOffsetForPointer(dest,lastTypeDest);
	if(!notFirst)
		stream << '0';
	stream << ">";
	notFirst=compileOffsetForPointer(src,lastTypeSrc);
	if(!notFirst)
		stream << '0';
	stream << "){\n";
	//Destination is after source, copy backward
	compileMemFunc(dest, src, size, BACKWARD);
	stream << "}else{";
	//Destination is before source, copy forward
	compileMemFunc(dest, src, size, FORWARD);
	stream << "}\n";
}

bool DuettoWriter::isTypedArrayType(Type* t) const
{
	return t->isIntegerTy(8) || t->isIntegerTy(16) || t->isIntegerTy(32) ||
		t->isFloatTy() || t->isDoubleTy();
}

void DuettoWriter::compileTypedArrayType(Type* t)
{
	if(t->isIntegerTy(8))
		stream << "Int8Array";
	else if(t->isIntegerTy(16))
		stream << "Int16Array";
	else if(t->isIntegerTy(32))
		stream << "Int32Array";
	else if(t->isFloatTy())
		stream << "Float32Array";
	else if(t->isDoubleTy())
		stream << "Float64Array";
	else
	{
		llvm::errs() << "Typed array requested for type " << *t << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
	}
}

/* Method that handles memcpy, memset and memmove.
 * If src is not NULL present a copy operation is done using the supplied direction.
 * memset is handled by passing a NULL src and setting resetValue as needed. direction should be FORWARD */
void DuettoWriter::compileMemFunc(const Value* dest, const Value* src, const Value* size,
		COPY_DIRECTION copyDirection)
{
	//Find out the real type of the copied object
	std::set<const PHINode*> visitedPhis;
	Type* destType=findRealType(dest,visitedPhis);
	if(copyDirection!=RESET)
	{
		visitedPhis.clear();
		Type* srcType=findRealType(src,visitedPhis);
		if(destType!=srcType)
			llvm::report_fatal_error("Different destination and source type for memcpy/memmove", false);
	}
	assert(destType->isPointerTy());

	Type* pointedType = static_cast<PointerType*>(destType)->getElementType();
	if(isUnion(pointedType))
	{
		//We can use the natural i8*, since the union will have already an allocated
		//typed array when it has been casted to i8*
		pointedType = dest->getType()->getPointerElementType();
	}
	uint32_t typeSize = targetData.getTypeAllocSize(pointedType);

	//Check that the number of element is not zero
	if(ConstantInt::classof(size))
	{
		uint32_t allocatedSize = getIntFromValue(size);
		uint32_t numElem = (allocatedSize+typeSize-1)/typeSize;
		if(numElem==0)
			return;
	}
	else
	{
		//Compute number of elements at runtime
		stream << "var __numElem__=";
		compileOperand(size);
		stream << '/' << typeSize;
		//Make sure to close this if below
		stream << ";\nif(__numElem__!=0)\n{";
	}

	//The first element is handled copied directly, to support complete objects
	if(copyDirection==RESET)
		compileResetRecursive("", dest, src, pointedType, NULL);
	else
		compileCopyRecursive("", dest, src, pointedType, NULL);

	//The rest is compiled using a for loop, or native TypedArray set operator

	//NOTE: For constant values we can stop code generation here
	//For the dynamic case we still need to close the if below
	if(ConstantInt::classof(size))
	{
		uint32_t allocatedSize = getIntFromValue(size);
		uint32_t numElem = (allocatedSize+typeSize-1)/typeSize;

		if(numElem==1)
			return;
	}

	const Type* lastTypeSrc = NULL;
	const Type* lastTypeDest = NULL;
	//Prologue: Construct the first part, up to using the size
	if(copyDirection!=RESET && isTypedArrayType(pointedType))
	{
		// The semantics of set is memmove like, no need to care about direction
		lastTypeDest=compileObjectForPointer(dest, NORMAL);
		stream << ".set(";
		lastTypeSrc=compileObjectForPointer(src, NORMAL);
		//We need to get a subview of the source
		stream << ".subarray(";
		bool notFirst=compileOffsetForPointer(src,lastTypeSrc);
		if(!notFirst)
			stream << '0';
		stream << ',';
		notFirst=compileOffsetForPointer(src,lastTypeSrc);
		if(notFirst)
			stream << '+';
	}
	else
	{
		//memset is always handled using the for loop
		if(copyDirection == FORWARD || copyDirection == RESET)
			stream << "for(var __i__=1;__i__<";
		else
			stream << "for(var __i__=";
	}

	// Use the size
	if(ConstantInt::classof(size))
	{
		uint32_t allocatedSize = getIntFromValue(size);
		uint32_t numElem = (allocatedSize+typeSize-1)/typeSize;

		stream << numElem;
	}
	else
	{
		stream << "__numElem__";
	}

	//Epilogue: Write the code after the size
	if(copyDirection!=RESET && isTypedArrayType(pointedType))
	{
		stream << "),";
		bool notFirst=compileOffsetForPointer(dest,lastTypeDest);
		if(!notFirst)
			stream << '0';
		stream << ')';
	}
	else
	{
		if(copyDirection == FORWARD || copyDirection == RESET)
			stream	<< ";__i__++){\n";
		else
			stream << "-1;__i__>0;__i__--){\n";

		if(copyDirection==RESET)
			compileResetRecursive("", dest, src, pointedType,"__i__");
		else
			compileCopyRecursive("", dest, src, pointedType,"__i__");
		stream << "\n}";
	}

	if(!ConstantInt::classof(size))
	{
		//Close the if for the '0' case
		stream << "\n}";
	}
}

void DuettoWriter::compileAllocation(const Value* callV, const Value* size)
{
	//Find out if this is casted to something
	Value::const_use_iterator it=callV->use_begin();
	Value::const_use_iterator itE=callV->use_end();
	const Type* castedType = NULL;
	//If we are using the typed allocation it's easy
	if(const CallInst* ci=dyn_cast<CallInst>(callV))
	{
		if(ci->getCalledFunction() &&
			ci->getCalledFunction()->getIntrinsicID() == Intrinsic::duetto_allocate)
		{
			castedType = ci->getType();
		}
	}
	for(;it!=itE && castedType==NULL;++it)
	{
		if(BitCastInst::classof(*it))
			castedType = it->getType();
		else if(const CallInst* ci = dyn_cast<CallInst>(*it))
		{
			//Support duetto.cast.user
			if(ci->getCalledFunction() &&
				ci->getCalledFunction()->getIntrinsicID() == Intrinsic::duetto_cast_user)
			{
				castedType = it->getType();
			}
		}
	}

	//If there are no casts, use i8* from the call itself
	if(castedType==NULL)
		castedType = callV->getType();

	assert(castedType->isPointerTy());
	Type* t=static_cast<const PointerType*>(castedType)->getElementType();
	uint32_t typeSize = targetData.getTypeAllocSize(t);
	//For numerical types, create typed arrays
	if(isTypedArrayType(t))
	{
		stream << "new ";
		compileTypedArrayType(t);
		stream << '(';
		if(ConstantInt::classof(size))
		{
			uint32_t allocatedSize = getIntFromValue(size);
			uint32_t numElem = (allocatedSize+typeSize-1)/typeSize;
			stream << numElem;
		}
		else
		{
			compileOperand(size);
			stream << '/' << typeSize;
		}
		stream << ')';
	}
	else
	{
		if(ConstantInt::classof(size))
		{
			uint32_t allocatedSize = getIntFromValue(size);
			uint32_t numElem = (allocatedSize+typeSize-1)/typeSize;
			stream << '[';
			for(uint64_t i=0;i<numElem;i++)
			{
				compileType(t);
				if((i+1)<numElem)
					stream << ",";
			}
			stream << ']';
		}
		else
		{
			if(t->isStructTy())
			{
				StructType* st=cast<StructType>(t);
				arraysNeeded.insert(st);
				stream << "createArray";
				printLLVMName(st->getName(), GLOBAL);
				stream << '(';
				compileOperand(size);
				stream << '/' << typeSize << ')';
			}
			else if(t->isPointerTy())
			{
				stream << "createPointerArray(";
				compileOperand(size);
				stream << '/' << typeSize << ')';
			}
			else
			{
				llvm::errs() << "Allocating type " << *t << "\n";
				llvm::report_fatal_error("Unsupported type in allocation", false);
			}
		}
	}
}

void DuettoWriter::compileFree(const Value* obj)
{
	//Best effort
	POINTER_KIND k=getPointerKind(obj);
	compilePointer(obj, k);
	stream << "=null";
}

bool DuettoWriter::handleBuiltinCall(const char* ident, const Value* callV,
			User::const_op_iterator it, User::const_op_iterator itE, bool userImplemented)
{
	//First handle high priority builtins, they will be used even
	//if an implementation is available from the user
	if(strncmp(ident,"llvm.memmove",12)==0)
	{
		compileMove(*(it), *(it+1), *(it+2));
		return true;
	}
	else if(strncmp(ident,"llvm.memcpy",11)==0)
	{
		compileMemFunc(*(it), *(it+1), *(it+2), FORWARD);
		return true;
	}
	else if(strncmp(ident,"llvm.memset",11)==0)
	{
		//TODO: memset on allocate memory may be optimized
		compileMemFunc(*(it), *(it+1), *(it+2), RESET);
		return true;
	}
	else if(strncmp(ident,"llvm.lifetime",13)==0)
	{
		return true;
	}
	else if(strncmp(ident,"llvm.invariant",14)==0)
	{
		//TODO: Try to optimize using this, for now just pass the second arg
		compileOperand(*(it+1));
		return true;
	}
	else if(strncmp(ident,"llvm.va_start",13)==0)
	{
		compileDereferencePointer(*it, NULL);
		stream << " = { d:arguments, o:_" << currentFun->getName() << ".length }";
		return true;
	}
	else if(strncmp(ident,"llvm.va_end",11)==0)
	{
		compileDereferencePointer(*it, NULL);
		stream << "=null";
		return true;
	}
	else if(strncmp(ident,"llvm.duetto.downcast",20)==0)
	{
		compileDowncast(*(it), getIntFromValue(*(it+1)));
		return true;
	}
	else if(strncmp(ident,"llvm.duetto.upcast.collapsed",28)==0)
	{
		compileOperand(*it);
		return true;
	}
	else if(strncmp(ident,"llvm.duetto.cast.user",21)==0)
	{
		compileOperand(*it);
		return true;
	}
	else if(strcmp(ident,"malloc")==0 ||
		strcmp(ident,"_Znaj")==0 ||
		strcmp(ident,"_Znwj")==0 ||
		strncmp(ident,"llvm.duetto.allocate",20)==0)
	{
		compileAllocation(callV, *it);
		return true;
	}
	else if(strcmp(ident,"free")==0 ||
		strcmp(ident,"_ZdlPv")==0 ||
		strcmp(ident,"_ZdaPv")==0)
	{
		compileFree(*it);
		return true;
	}
	else if(strcmp(ident,"fmod")==0)
	{
		// Handle this internally, C++ does not have float mod operation
		stream << '(';
		compileOperand(*(it));
		stream << '%';
		compileOperand(*(it+1));
		stream << ')';
		return true;
	}

	//If the method is implemented by the user, stop here
	if(userImplemented)
		return false;

	if(strcmp(ident,"_ZN6client5ArrayixEi")==0 ||
		strcmp(ident,"_ZNK6client6ObjectcvdEv")==0)
	{
		//Do not touch method that are implemented in native JS code
		return false;
	}
	else if(strncmp(ident,"_ZN6client18duettoVariadicTrap",30)==0)
	{
		//Forward to the actual method, which is the first argument
		assert(isGEP(*it));
		Value* strVal = cast<User>(*it)->getOperand(0);
		assert(GlobalVariable::classof(strVal));
		Constant* strGlobal = cast<GlobalVariable>(strVal)->getInitializer();
		assert(ConstantDataSequential::classof(strGlobal));
		StringRef strName=cast<ConstantDataSequential>(strGlobal)->getAsCString();
		stream << strName;
		compileMethodArgs(it+1, itE);
		return true;
	}
	else if(strncmp(ident,"_ZN6client24duettoVariadicMemberTrap",36)==0)
	{
		//Forward to the actual method, which is the first argument
		assert(isGEP(*it));
		Value* strVal = cast<User>(*it)->getOperand(0);
		assert(GlobalVariable::classof(strVal));
		Constant* strGlobal = cast<GlobalVariable>(strVal)->getInitializer();
		assert(ConstantDataSequential::classof(strGlobal));
		StringRef strName=cast<ConstantDataSequential>(strGlobal)->getAsCString();
		assert((it+1)!=itE);
		compileOperand(*(it+1));
		stream << '.' << strName;
		compileMethodArgs(it+2, itE);
		return true;
	}
	else if(strncmp(ident,"_ZN6client",10)==0)
	{
		handleBuiltinNamespace(ident+10,it,itE);
		return true;
	}
	else if(strncmp(ident,"_ZNK6client",11)==0)
	{
		handleBuiltinNamespace(ident+11,it,itE);
		return true;
	}
	else if(strncmp(ident,"duettoCreate_ZN6client",22)==0)
	{
		//Default handling of builtin constructors
		char* typeName;
		int typeLen=strtol(ident+22,&typeName,10);
		//For builtin String, do not use new
		if(strncmp(typeName, "String", 6)!=0)
			stream << "new ";
		stream.write(typeName, typeLen);
		compileMethodArgs(it, itE);
		return true;
	}
	return false;
}

void DuettoWriter::compilePredicate(CmpInst::Predicate p)
{
	switch(p)
	{
		case CmpInst::FCMP_OEQ:
		case CmpInst::ICMP_EQ:
			stream << " === ";
			break;
		case CmpInst::FCMP_UNE: //The undordered case correspond to the usual JS operator
					//See ECMA-262, Section 11.9.6
		case CmpInst::ICMP_NE:
			stream << " !== ";
			break;
		case CmpInst::FCMP_OGT: //TODO: fix this, if an operand is NaN LLVM expects false,
		case CmpInst::FCMP_UGT:	//but JS returns undefined. Adding ==true after the whole expression
					//should work
		case CmpInst::ICMP_SGT:
		case CmpInst::ICMP_UGT: //TODO: To support unsigned we need to add casts around the ops
			stream << " > ";
			break;
		case CmpInst::FCMP_UGE:
		case CmpInst::FCMP_OGE:
		case CmpInst::ICMP_SGE:
		case CmpInst::ICMP_UGE:
			stream << " >= ";
			break;
		case CmpInst::FCMP_OLT: //TODO: fix this, if an operand is NaN LLVM expects false,
		case CmpInst::FCMP_ULT:	//but JS returns undefined. Adding ==true after the whole expression
					//should work
		case CmpInst::ICMP_SLT:
		case CmpInst::ICMP_ULT: //TODO: To support unsigned we need to add casts around the ops
			stream << " < ";
			break;
		case CmpInst::FCMP_ULE:
		case CmpInst::FCMP_OLE:
		case CmpInst::ICMP_SLE:
		case CmpInst::ICMP_ULE:
			stream << " <= ";
			break;
		default:
			llvm::errs() << "Support predicate " << p << '\n';
	}
}

void DuettoWriter::compileOperandForIntegerPredicate(const Value* v, CmpInst::Predicate p)
{
	if(CmpInst::isUnsigned(p))
		compileUnsignedInteger(v);
	else
		compileSignedInteger(v);
}

void DuettoWriter::compileEqualPointersComparison(const llvm::Value* lhs, const llvm::Value* rhs, CmpInst::Predicate p)
{
	// To correctly compare every kind of pointers, we need to distinguish between
	// pointer to immutable types and the rest.
	// 1) Pointers to immutable types must be compared by base and offset, they are
	//    guaranteed to have the same array as a base since they are created as array
	// 2) Other pointers are compared by dereferencing the JS pointer struct. This works
	//    because they can be compatred by reference, while immutable types are compared by
	//    value.

	llvm::Type* pointedType = lhs->getType()->getPointerElementType();
	bool isImmutable = isImmutableType(pointedType);
	bool isFunction = pointedType->isFunctionTy();
	bool isClient = isClientType(pointedType);

	if (isImmutable)
	{
		const Type* lastType1=compileObjectForPointer(lhs, NORMAL);
		if(p==CmpInst::ICMP_NE)
			stream << "!==";
		else
			stream << "===";
		const Type* lastType2=compileObjectForPointer(rhs, NORMAL);
		if(getPointerKind(lhs)==REGULAR ||
			getPointerKind(rhs)==REGULAR)
		{
			if(p==CmpInst::ICMP_NE)
				stream << " || ";
			else
				stream << " && ";
			bool notFirst=compileOffsetForPointer(lhs,lastType1);
			if(!notFirst)
				stream << '0';
			if(p==CmpInst::ICMP_NE)
				stream << "!==";
			else
				stream << "===";
			notFirst=compileOffsetForPointer(rhs,lastType2);
			if(!notFirst)
				stream << '0';
		}
	}
	else if(isFunction || isClient)
	{
		//Functions can be compared by reference, the can't be in an array
		//There can be an array of pointer to functions, not an array of functions
		compileOperand(lhs);
		if(p==CmpInst::ICMP_NE)
			stream << "!==";
		else
			stream << "===";
		compileOperand(rhs);
	}
	else
	{
		compileDereferencePointer(lhs, NULL);
		if(p==CmpInst::ICMP_NE)
			stream << "!==";
		else
			stream << "===";
		compileDereferencePointer(rhs, NULL);
	}
}

void DuettoWriter::printLLVMName(const StringRef& s, NAME_KIND nameKind) const
{
	const char* data=s.data();
	//Add an '_' or 'L' to skip reserved names
	stream.write((nameKind==GLOBAL)?"_":"L",1);
	for(uint32_t i=0;i<s.size();i++)
	{
		//We need to escape invalid chars
		switch(data[i])
		{
			case '.':
				stream.write("_p",2);
				break;
			case '-':
				stream.write("_m",2);
				break;
			case ':':
				stream.write("_c",2);
				break;
			case '<':
				stream.write("_l",2);
				break;
			case '>':
				stream.write("_r",2);
				break;
			case ' ':
				stream.write("_s",2);
				break;
			case '_':
				//NOTE: This may cause collisions
				stream.write("_",1);
				break;
			default:
				stream.write(data+i,1);
		}
	}
}

bool DuettoWriter::isImmutableType(const Type* t) const
{
	if(t->isIntegerTy() || t->isFloatTy() || t->isDoubleTy() || t->isPointerTy())
		return true;
	return false;
}

/*
 * The map is used to handle cyclic PHI nodes
 */
DuettoWriter::POINTER_KIND DuettoWriter::getPointerKind(const Value* v, std::map<const PHINode*, POINTER_KIND>& visitedPhis)
{
	assert(v->getType()->isPointerTy());
	PointerType* pt=cast<PointerType>(v->getType());
	if(isClientType(pt->getElementType()))
	{
		//Pointers to client type are complete objects, and are never expanded to
		//regular ones since an array of client objects does not exists.
		//NOTE: An array of pointer to client objects exists, not an array of objects.
		return COMPLETE_OBJECT;
	}
	if(AllocaInst::classof(v) || GlobalVariable::classof(v))
	{
		if(isImmutableType(pt->getElementType()))
			return COMPLETE_ARRAY;
		else
			return COMPLETE_OBJECT;
	}
	//Follow bitcasts
	if(isBitCast(v))
	{
		const User* bi=static_cast<const User*>(v);
		//Casts from unions return regular pointers
		if(isUnion(bi->getOperand(0)->getType()->getPointerElementType()))
		{
			//Special case arrays
			if(ArrayType::classof(pt->getElementType()))
				return COMPLETE_OBJECT;
			else
				return REGULAR;
		}
		return getPointerKind(bi->getOperand(0), visitedPhis);
	}
	if(isNopCast(v))
	{
		const User* bi=static_cast<const User*>(v);
		return getPointerKind(bi->getOperand(0), visitedPhis);
	}
	if(isDowncast(v))
		return COMPLETE_OBJECT;
	if(isComingFromAllocation(v))
		return COMPLETE_ARRAY;
	//Follow PHIs
	const PHINode* newPHI=dyn_cast<const PHINode>(v);
	if(newPHI)
	{
		std::map<const PHINode*, POINTER_KIND>::iterator alreadyVisited=visitedPhis.find(newPHI);
		if(alreadyVisited!=visitedPhis.end())
		{
			//Assume true, if needed it will become false later on
			return alreadyVisited->second;
		}
		//Intialize the value with the best scenario, demote it later if necessary
		std::map<const PHINode*, POINTER_KIND>::iterator current=
			visitedPhis.insert(make_pair(newPHI, COMPLETE_OBJECT)).first;
		for(unsigned i=0;i<newPHI->getNumIncomingValues();i++)
		{
			POINTER_KIND k=getPointerKind(newPHI->getIncomingValue(i), visitedPhis);
			if (k > current->second)
				current->second=k;
			if (k == REGULAR)
				break;
		}
		return current->second;
	}
	return REGULAR;
}

DuettoWriter::POINTER_KIND DuettoWriter::getPointerKind(const Value* v)
{
	std::map<const PHINode*, POINTER_KIND> visitedPhis;
	return getPointerKind(v, visitedPhis);
}

bool DuettoWriter::isDowncast(const Value* val) const
{
	const CallInst* newCall=dyn_cast<const CallInst>(val);
	if(newCall && newCall->getCalledFunction())
		return newCall->getCalledFunction()->getName()=="llvm.duetto.downcast";
	return false;
}

bool DuettoWriter::isNopCast(const Value* val) const
{
	const CallInst* newCall=dyn_cast<const CallInst>(val);
	if(newCall && newCall->getCalledFunction())
		return newCall->getCalledFunction()->getName()=="llvm.duetto.upcast.collapsed"
			|| newCall->getCalledFunction()->getIntrinsicID() == Intrinsic::duetto_cast_user;
	return false;
}

void DuettoWriter::compileDereferencePointer(const Value* v, const Value* offset, const char* namedOffset)
{
	assert(v->getType()->isPointerTy());
	POINTER_KIND k=getPointerKind(v);

	bool isOffsetConstantZero = false;
	if(offset==NULL || (ConstantInt::classof(offset) && getIntFromValue(offset)==0))
		isOffsetConstantZero = true;

	const Type* lastType=compileObjectForPointer(v, NORMAL);
	if(k==COMPLETE_OBJECT)
	{
		assert(isOffsetConstantZero);
		assert(namedOffset==NULL);
		return;
	}
	stream << '[';
	if(k==COMPLETE_ARRAY)
	{
		bool notFirst=false;
		if(namedOffset)
		{
			stream << namedOffset;
			notFirst = true;
		}

		if(isOffsetConstantZero)
		{
			if(!notFirst)
				stream << '0';
		}
		else
		{
			if(notFirst)
				stream << '+';
			compileOperand(offset);
		}
	}
	else
	{
		// We need to explicitly avoid adding 0 to the offset. It has no
		// effect on numeric offsets, but screws up string offsets.
		// When there are string offset the pointer is for a struct member,
		// in this case it is only valid to dereference the exact pointer.
		// It is also possible to add 1, but in such case only comparing is allowed
		// so doing the string concatenation is ok.
		// NOTE: It actually breaks if we do +1 -1
		// NOTE: namedOffset is only used in memcpy/memove which already handle the 0th
		// case in a special way
		// TODO: We need to add a pointer kind which allows arithmetic
		if(!isOffsetConstantZero)
		{
			assert(namedOffset==NULL);
			compileOperand(offset);
			stream << "===0?";
			bool notFirst=compileOffsetForPointer(v, lastType);
			//TODO: Optimize this
			if(!notFirst)
				stream << '0';
			stream << ':';
		}
		bool notFirst=compileOffsetForPointer(v, lastType);
		if(!isOffsetConstantZero)
		{
			if(notFirst)
				stream << '+';
			compileOperand(offset);
			notFirst = true;
		}
		if(namedOffset)
		{
			if(notFirst)
				stream << '+';
			stream << namedOffset;
			notFirst = true;
		}
		if(!notFirst)
			stream << '0';
	}
	stream << ']';
}

uint32_t DuettoWriter::getIntFromValue(const Value* v) const
{
	if(!ConstantInt::classof(v))
	{
		llvm::errs() << "Expected constant int found " << *v << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
		return 0;
	}

	const ConstantInt* i=cast<const ConstantInt>(v);
	return i->getZExtValue();
}

const Type* DuettoWriter::compileRecursiveAccessToGEP(Type* curType, const Use* it, const Use* const itE,
							COMPILE_FLAG flag)
{
	//Before this the base name has been already printed
	if(it==itE)
		return curType;
	Type* subType = NULL;
	if(curType->isStructTy())
	{
		const StructType* st=static_cast<const StructType*>(curType);
		//Special handling for constant offsets
		assert(ConstantInt::classof(*it));
		uint32_t elementIndex = getIntFromValue(*it);
		if(flag!=DRY_RUN)
			stream << ".a" << elementIndex;
		subType = st->getElementType(elementIndex);
	}
	else if(curType->isArrayTy())
	{
		const ArrayType* at=static_cast<const ArrayType*>(curType);
		if(flag!=DRY_RUN)
		{
			stream << '[';
			//Special handling for constant offsets
			if(ConstantInt::classof(*it))
			{
				uint32_t elementIndex = getIntFromValue(*it);
				stream << elementIndex;
			}
			else
				compileOperand(*it);
			stream << ']';
		}
		subType = at->getElementType();
	}
	else
	{
		llvm::errs() << "Unexpected type during GEP access " << *curType << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
		return curType;
	}
	return compileRecursiveAccessToGEP(subType, ++it, itE, flag);
}

bool DuettoWriter::isClientType(const Type* t) const
{
	return (t->isStructTy() && cast<StructType>(t)->hasName() &&
		strncmp(t->getStructName().data(), "class._ZN6client", 16)==0);
}

bool DuettoWriter::isUnion(const Type* t) const
{
	return (t->isStructTy() && cast<StructType>(t)->hasName() &&
		t->getStructName().startswith("union."));
}

bool DuettoWriter::safeUsagesForNewedMemory(const Value* v) const
{
	Value::const_use_iterator it=v->use_begin();
	Value::const_use_iterator itE=v->use_end();
	for(;it!=itE;++it)
	{
		const PHINode* p=dyn_cast<const PHINode>(*it);
		//If the usage is a PHI node, recursively check its usages
		if(p)
		{
			if(!safeUsagesForNewedMemory(p))
				return false;
		}
		else
		{
			const CallInst* ci=dyn_cast<const CallInst>(*it);
			if(!safeCallForNewedMemory(ci))
				return false;
		}
	}
	return true;
}

bool DuettoWriter::safeCallForNewedMemory(const CallInst* ci) const
{
	//We allow the unsafe cast to i8* only
	//if the usage is memcpy, memset, free or delete
	//or one of the lifetime/invariant intrinsics
	return (ci && ci->getCalledFunction() &&
		(ci->getCalledFunction()->getName()=="llvm.memcpy.p0i8.p0i8.i32" ||
		ci->getCalledFunction()->getName()=="llvm.memset.p0i8.i32" ||
		ci->getCalledFunction()->getName()=="llvm.memset.p0i8.i64" ||
		ci->getCalledFunction()->getName()=="llvm.memmove.p0i8.p0i8.i32" ||
		ci->getCalledFunction()->getName()=="free" ||
		ci->getCalledFunction()->getName()=="_ZdaPv" ||
		ci->getCalledFunction()->getName()=="_ZdlPv" ||
		ci->getCalledFunction()->getName()=="llvm.lifetime.start" ||
		ci->getCalledFunction()->getName()=="llvm.lifetime.end" ||
		ci->getCalledFunction()->getName()=="llvm.invariant.start" ||
		ci->getCalledFunction()->getName()=="llvm.invariant.end" ||
		//Allow unsafe casts for a limited number of functions that accepts callback args
		//TODO: find a nicer approach for this
		ci->getCalledFunction()->getName()=="__cxa_atexit"));
}

bool DuettoWriter::isValidVoidPtrSource(const Value* val) const
{
	std::set<const PHINode*> visitedPhis;
	return isValidVoidPtrSource(val, visitedPhis);
}

bool DuettoWriter::isComingFromAllocation(const Value* val) const
{
	const CallInst* newCall=dyn_cast<const CallInst>(val);
	if(newCall && newCall->getCalledFunction())
	{
		return newCall->getCalledFunction()->getName()=="_Znwj"
			|| newCall->getCalledFunction()->getName()=="_Znaj"
			|| newCall->getCalledFunction()->getName()=="realloc"
			|| newCall->getCalledFunction()->getName()=="malloc"
			|| newCall->getCalledFunction()->getIntrinsicID() == Intrinsic::duetto_allocate;
	}
	//Try invoke as well
	const InvokeInst* newInvoke=dyn_cast<const InvokeInst>(val);
	if(newInvoke && newInvoke->getCalledFunction())
	{
		//TODO: Disable throw in new, it's nonsense in JS context
		return newInvoke->getCalledFunction()->getName()=="_Znwj"
			|| newInvoke->getCalledFunction()->getName()=="_Znaj"
			|| newInvoke->getCalledFunction()->getName()=="realloc"
			|| newInvoke->getCalledFunction()->getName()=="malloc"
			|| newInvoke->getCalledFunction()->getIntrinsicID() == Intrinsic::duetto_allocate;
	}
	return false;
}

bool DuettoWriter::isValidVoidPtrSource(const Value* val, std::set<const PHINode*>& visitedPhis) const
{
	if(isComingFromAllocation(val))
		return true;
	const CallInst* newCall=dyn_cast<const CallInst>(val);
	if(newCall && newCall->getCalledFunction())
	{
		return newCall->getCalledFunction()->getName()=="llvm.duetto.downcast"
			|| newCall->getCalledFunction()->getName()=="llvm.duetto.upcast.collapsed"
			|| newCall->getCalledFunction()->getName()=="llvm.duetto.cast.user";
	}
	//Try invoke as well
	const InvokeInst* newInvoke=dyn_cast<const InvokeInst>(val);
	if(newInvoke && newInvoke->getCalledFunction())
	{
		return newInvoke->getCalledFunction()->getName()=="llvm.duetto.downcast"
			|| newInvoke->getCalledFunction()->getName()=="llvm.duetto.upcast.collapsed"
			|| newInvoke->getCalledFunction()->getName()=="llvm.duetto.cast.user";
	}
	const PHINode* newPHI=dyn_cast<const PHINode>(val);
	if(newPHI)
	{
		if(visitedPhis.count(newPHI))
		{
			//Assume true, if needed it will become false later on
			return true;
		}
		visitedPhis.insert(newPHI);
		for(unsigned i=0;i<newPHI->getNumIncomingValues();i++)
		{
			if(!isValidVoidPtrSource(newPHI->getIncomingValue(i),visitedPhis))
			{
				visitedPhis.erase(newPHI);
				return false;
			}
		}
		visitedPhis.erase(newPHI);
		return true;
	}
	return false;
}

bool DuettoWriter::isValidTypeCast(const Value* castI, const Value* castOp, Type* srcPtr, Type* dstPtr) const
{
	//Only pointer casts are possible anyway
	assert(srcPtr->isPointerTy() && dstPtr->isPointerTy());
	Type* src=cast<PointerType>(srcPtr)->getElementType();
	Type* dst=cast<PointerType>(dstPtr)->getElementType();
	//Conversion between client objects is free
	if(isClientType(src) && isClientType(dst))
		return true;
	//Conversion between any function pointer are ok
	if(src->isFunctionTy() && dst->isFunctionTy())
		return true;
	//Allow conversions between equivalent struct types
	if(src->isStructTy() && dst->isStructTy())
	{
		StructType* srcSt = cast<StructType>(src);
		StructType* dstSt = cast<StructType>(dst);
		if(srcSt->isLayoutIdentical(dstSt))
			return true;
	}
	if(dst->isIntegerTy(8))
		return true;
	//Support getting functions back from the Vtable
	if(src->isPointerTy() && dst->isPointerTy())
	{
		Type* innerSrc=cast<PointerType>(src)->getElementType();
		Type* innerDst=cast<PointerType>(dst)->getElementType();
		if(innerSrc->isIntegerTy(8) || innerDst->isFunctionTy())
		{
			const ConstantExpr* constGep=dyn_cast<const ConstantExpr>(castOp);
			if(constGep && constGep->getOpcode()==Instruction::GetElementPtr)
			{
				const Value* sourceVal = constGep->getOperand(0);
				if(sourceVal->hasName() &&
					strncmp(sourceVal->getName().data(),"_ZTV",4)==0)
				{
					//This casts ultimately comes from a VTable, it's ok
					return true;
				}
			}
		}
		if(innerSrc->isFunctionTy() && innerDst->isFunctionTy())
			return true;
	}
	//Also allow the unsafe cast from i8* in a few selected cases
	if(src->isIntegerTy(8))
	{
		bool comesFromNew = isValidVoidPtrSource(castOp);
		bool allowedRawUsages = true;
		Value::const_use_iterator it=castOp->use_begin();
		Value::const_use_iterator itE=castOp->use_end();
		for(;it!=itE;++it)
		{
			//Check that the other use is a memset or an icmp
			if((*it)==castI)
				continue;
			const CallInst* ci=dyn_cast<const CallInst>(*it);
			if(!(ICmpInst::classof(*it) || safeCallForNewedMemory(ci)))
				allowedRawUsages = false;
		}
		if(comesFromNew && allowedRawUsages)
			return true;
	}
	if(isUnion(src) && (ArrayType::classof(dst) || isTypedArrayType(dst)))
		return true;
	//Allow changing the size of an array
	if (ArrayType::classof(src) && ArrayType::classof(dst) &&
		src->getSequentialElementType() == dst->getSequentialElementType())
	{
		return true;
	}
	return false;
}

void DuettoWriter::compileConstantExpr(const ConstantExpr* ce)
{
	switch(ce->getOpcode())
	{
		case Instruction::GetElementPtr:
		{
			if(ce->getNumOperands()<3)
			{
				//HACK: Type info for type info is accessed weirdly, understand why
				stream << "null";
				return;
			}
			Value* base = ce->getOperand(0);
			//NOTE: the first dereference must be 0, they point to a single object
#ifndef NDEBUG
			Value* first=ce->getOperand(1);
#endif
			assert(getIntFromValue(first)==0);
			compileGEP(base, ce->op_begin()+1, ce->op_end()-1);
			break;
		}
		case Instruction::BitCast:
		{
			Value* val=ce->getOperand(0);
			//Special case guard variables, they are defined as 64bit,
			//but only the first byte is specified and probably used
			//Guard variables are identified by their mangling prefix
			if(val->hasName() && strncmp("_ZGV",val->getName().data(),4)==0)
			{
				compileOperand(val);
				break;
			}
			Type* dst=ce->getType();
			Type* src=val->getType();
			if(!isValidTypeCast(ce, val, src, dst))
			{
				llvm::errs() << "Between:\n\t" << *src << "\n\t" << *dst << "\n";
				llvm::errs() << "warning: Type conversion is not safe, expect issues. And report a bug.\n";
			}
			compileOperand(val);
			break;
		}
		default:
			llvm::errs() << "warning: Unsupported constant expr " << ce->getOpcodeName() << '\n';
	}
}

bool DuettoWriter::isClientGlobal(const char* mangledName) const
{
	return strncmp(mangledName,"_ZN6client",10)==0;
}

void DuettoWriter::compileConstant(const Constant* c)
{
	if(ConstantExpr::classof(c))
		compileConstantExpr(cast<ConstantExpr>(c));
	else if(ConstantDataSequential::classof(c))
	{
		const ConstantDataSequential* d=cast<const ConstantDataSequential>(c);
		Type* t=d->getElementType();
		stream << "new ";
		compileTypedArrayType(t);
		stream << "([";
		for(uint32_t i=0;i<d->getNumElements();i++)
		{
			compileConstant(d->getElementAsConstant(i));
			if((i+1)<d->getNumElements())
				stream << ",";
		}
		stream << "])";
	}
	else if(ConstantArray::classof(c))
	{
		const ConstantArray* d=cast<const ConstantArray>(c);
		stream << '[';
		assert(d->getType()->getNumElements() == d->getNumOperands());
		for(uint32_t i=0;i<d->getNumOperands();i++)
		{
			compileOperand(d->getOperand(i), OPERAND_EXPAND_COMPLETE_OBJECTS);
			if((i+1)<d->getNumOperands())
				stream << ",";
		}
		stream << ']';
	}
	else if(ConstantStruct::classof(c))
	{
		const ConstantStruct* d=cast<const ConstantStruct>(c);
		stream << '{';
		assert(d->getType()->getNumElements() == d->getNumOperands());
		for(uint32_t i=0;i<d->getNumOperands();i++)
		{
			stream << 'a' << i << ':';
			compileOperand(d->getOperand(i), OPERAND_EXPAND_COMPLETE_OBJECTS);
			if((i+1)<d->getNumOperands())
				stream << ",";
		}
		stream << '}';
	}
	else if(ConstantFP::classof(c))
	{
		const ConstantFP* f=cast<const ConstantFP>(c);
		//Must compare pointers for semantics, it seems
		if(f->getValueAPF().isInfinity())
		{
			if(f->getValueAPF().isNegative())
				stream << '-';
			stream << "Infinity";
		}
		else if(&f->getValueAPF().getSemantics()==&APFloat::IEEEsingle)
			stream << f->getValueAPF().convertToFloat();
		else if(&f->getValueAPF().getSemantics()==&APFloat::IEEEdouble)
			stream << f->getValueAPF().convertToDouble();
		else
			llvm::report_fatal_error("Unsupported float type, please report a bug", false);
	}
	else if(ConstantInt::classof(c))
	{
		const ConstantInt* i=cast<const ConstantInt>(c);
		if(i->getBitWidth()==1)
			stream << (i->isZero()?"false":"true");
		else
			stream << i->getSExtValue();
	}
	else if(ConstantPointerNull::classof(c))
	{
		const Type* pointedType = c->getType()->getPointerElementType();
		if(isClientType(pointedType))
			stream << "null";
		else
			stream << "nullObj";
	}
	else if(UndefValue::classof(c))
	{
		stream << "undefined";
	}
	else if(GlobalAlias::classof(c))
	{
		const GlobalAlias* a=cast<const GlobalAlias>(c);
		compileConstant(a->getAliasedGlobal());
	}
	else if(GlobalValue::classof(c))
	{
		assert(c->hasName());
		//Check if this is a client global value, if so skip mangling
		const char* mangledName = c->getName().data();
		if(isClientGlobal(mangledName))
		{
			//Client value
			char* objName;
			int nameLen=strtol(mangledName+10,&objName,10);
			stream.write(objName, nameLen);
		}
		else
		{
			const GlobalValue* GV=cast<const GlobalValue>(c);
			printLLVMName(c->getName(), GLOBAL);
			if(globalsDone.count(GV)==0)
#ifdef DEBUG_GLOBAL_DEPS
				globalsQueue.insert(make_pair(GV,currentFun));
#else
				globalsQueue.insert(GV);
#endif
		}
	}
	else if(ConstantAggregateZero::classof(c))
	{
		compileType(c->getType());
	}
	else
	{
		llvm::errs() << "Unsupported constant type ";
		c->dump();
		stream << "null";
	}
}

void DuettoWriter::compilePointer(const Value* v, POINTER_KIND acceptedKind)
{
	assert(v->getType()->isPointerTy());
	POINTER_KIND k=getPointerKind(v);
	assert(acceptedKind>=k);
	if(acceptedKind==k)
	{
		//Nothing to do, forward
		compileOperandImpl(v);
	}
	else if(acceptedKind==COMPLETE_ARRAY)
	{
		assert(k==COMPLETE_OBJECT);
		//We need to forge a complete array
		stream << '[';
		compileOperandImpl(v);
		stream << ']';
	}
	else
	{
		assert(acceptedKind==REGULAR);
		if(k==COMPLETE_ARRAY)
		{
			stream << "{ d: ";
			compileOperandImpl(v);
			stream << ", o: 0}";
		}
		else if(k==COMPLETE_OBJECT)
		{
			stream << "{ d: [";
			compileOperandImpl(v);
			stream << "], o: 0}";
		}
	}
}

void DuettoWriter::compileOperandImpl(const Value* v)
{
	if(const Constant* c=dyn_cast<const Constant>(v))
		compileConstant(c);
	else if(const Instruction* it=dyn_cast<Instruction>(v))
	{
		if(isInlineable(*it))
			compileInlineableInstruction(*cast<Instruction>(v));
		else
			printVarName(it);
	}
	else if(const Argument* arg=dyn_cast<const Argument>(v))
		printArgName(arg);
	else
	{
		llvm::errs() << "No name for value ";
		v->dump();
	}
}

void DuettoWriter::compileOperand(const Value* v, OperandFix fix)
{
	//First deal with complete objects, but never expand pointers to client objects
	if(v->getType()->isPointerTy() &&
		fix==OPERAND_EXPAND_COMPLETE_OBJECTS &&
		!isClientType(v->getType()->getPointerElementType()))
	{
		compilePointer(v, REGULAR);
	}
	else
		compileOperandImpl(v);
}

void DuettoWriter::compileTypeImpl(Type* t)
{
	switch(t->getTypeID())
	{
		case Type::IntegerTyID:
		{
			//We only really have 32bit integers.
			//We will allow anything shorter.
			//NOTE: Only bit operations are allowed on shorter types
			//this is enforced on a per-operation basis
			//Print out a '0' to let the engine know this is an integer.
			stream << '0';
			break;
		}
		case Type::FloatTyID:
		case Type::DoubleTyID:
		{
			stream << '0';
			break;
		}
		case Type::StructTyID:
		{
			//Special case union first
			if(isUnion(t))
			{
				uint32_t typeSize = targetData.getTypeAllocSize(t);
				stream << "new ArrayBuffer(";
				stream << typeSize;
				stream << ')';
				break;
			}
			StructType* st=static_cast<StructType*>(t);
			stream << "{ ";
			StructType::element_iterator E=st->element_begin();
			StructType::element_iterator EE=st->element_end();
			uint32_t offset=0;
			for(;E!=EE;++E)
			{
				if(offset!=0)
					stream << ", ";
				stream << "a" << offset << ": ";
				compileType(*E);
				offset++;
			}
			stream << " }";
			break;
		}
		case Type::PointerTyID:
		{
			const Type* pointedType = t->getPointerElementType();
			if(isClientType(pointedType))
				stream << "null";
			else
				stream << "nullObj";
			break;
		}
		case Type::ArrayTyID:
		{
			ArrayType* at=static_cast<ArrayType*>(t);
			Type* et=at->getElementType();
			//For numerical types, create typed arrays
			if(isTypedArrayType(et))
			{
				stream << "new ";
				compileTypedArrayType(et);
				stream << '(' << at->getNumElements() << ')';
			}
			else
			{
				stream << '[';
				for(uint64_t i=0;i<at->getNumElements();i++)
				{
					compileType(at->getElementType());
					if((i+1)<at->getNumElements())
						stream << ",";
				}
				stream << ']';
			}
			break;
		}
		default:
			llvm::errs() << "Support type ";
			t->dump();
			llvm::errs() << '\n';
	}
}

void DuettoWriter::compileType(Type* t)
{
	if(StructType* st=dyn_cast<StructType>(t))
	{
		NamedMDNode* basesMeta=NULL;
		//TODO: Verify that it makes sense to assume struct with no name has no bases
		if(st->hasName())
			basesMeta=module.getNamedMetadata(Twine(st->getName(),"_bases"));
		if(basesMeta)
		{
			classesNeeded.insert(st);
			stream << "create";
			printLLVMName(st->getName(), GLOBAL);
			stream << "()";
			return;
		}
		//Else fallthrough to base case
	}
	compileTypeImpl(t);
}

uint32_t DuettoWriter::getUniqueIndexForValue(const Value* v)
{
	std::map<const Value*,uint32_t>::iterator it=unnamedValueMap.find(v);
	if(it==unnamedValueMap.end())
		it=unnamedValueMap.insert(make_pair(v, currentUniqueIndex++)).first;
	return it->second;
}

uint32_t DuettoWriter::getUniqueIndex()
{
	return currentUniqueIndex++;
}

void DuettoWriter::printVarName(const Value* val)
{
	if(val->hasName())
		printLLVMName(val->getName(), GlobalValue::classof(val)?GLOBAL:LOCAL);
	else
		stream << "tmp" << getUniqueIndexForValue(val);
}

void DuettoWriter::printArgName(const Argument* val) const
{
	if(val->hasName())
		printLLVMName(val->getName(), LOCAL);
	else
		stream << "arg" << val->getArgNo();
}

void DuettoWriter::compilePHIOfBlockFromOtherBlock(const BasicBlock* to, const BasicBlock* from)
{
	BasicBlock::const_iterator I=to->begin();
	BasicBlock::const_iterator IE=to->end();
	SmallVector<uint32_t, 4> tmps;
	//Phase 1, use temporaries to store the results of PHIs
	for(;I!=IE;++I)
	{
		const PHINode* phi=dyn_cast<const PHINode>(I);
		//TODO: I think that after the first non-phi node we can stop
		if(phi==NULL)
			continue;
		const Value* val=phi->getIncomingValueForBlock(from);
		uint32_t tmpIndex = getUniqueIndex();
		stream << "var tmpphi" << tmpIndex << " = ";
		tmps.push_back(tmpIndex);
		if(val->getType()->isPointerTy())
		{
			//Fix complete object pointers if needed
			POINTER_KIND k=getPointerKind(phi);
			compilePointer(val, k);
		}
		else
			compileOperand(val);
		stream << ";\n";
	}
	//Phase 2, actually assign the values
	I=to->begin();
	for(uint32_t tmpI=0;I!=IE;++I,tmpI++)
	{
		const PHINode* phi=dyn_cast<const PHINode>(I);
		if(phi==NULL)
			continue;
		//TODO: verify that 'var' works
		stream << "var ";
		printVarName(phi);
		stream << " = tmpphi" << tmps[tmpI] << ";\n";
	}
}

void DuettoWriter::compileMethodArgs(const llvm::User::const_op_iterator it, const llvm::User::const_op_iterator itE)
{
	stream << '(';
	for(llvm::User::const_op_iterator cur=it;cur!=itE;++cur)
	{
		if(cur!=it)
			stream << ", ";
		compileOperand(*cur, OPERAND_EXPAND_COMPLETE_OBJECTS);
	}
	stream << ')';
}

/*
 * This method is fragile, each opcode must handle the phis in the correct place
 */
void DuettoWriter::compileTerminatorInstruction(const TerminatorInst& I)
{
	switch(I.getOpcode())
	{
		case Instruction::Ret:
		{
			const ReturnInst& ri=static_cast<const ReturnInst&>(I);
			assert(I.getNumSuccessors()==0);
			Value* retVal = ri.getReturnValue();
			stream << "return ";
			if(retVal)
				compileOperand(retVal, OPERAND_EXPAND_COMPLETE_OBJECTS);
			stream << ";\n";
			break;
		}
		case Instruction::Invoke:
		{
			const InvokeInst& ci=static_cast<const InvokeInst&>(I);
			//TODO: Support unwind
			//For now, pretend it's a regular call
			if(ci.getCalledFunction())
			{
				//Direct call
				const char* funcName=ci.getCalledFunction()->getName().data();
				if(handleBuiltinCall(funcName,&ci,ci.op_begin(),ci.op_begin()+ci.getNumArgOperands(),
						!ci.getCalledFunction()->empty()))
				{
					stream << ";\n";
					//Only consider the normal successor for PHIs here
					//For each successor output the variables for the phi nodes
					compilePHIOfBlockFromOtherBlock(ci.getNormalDest(), I.getParent());
					break;
				}
				else
				{
					stream << '_' << funcName;
					if(!globalsDone.count(ci.getCalledFunction()))
#ifdef DEBUG_GLOBAL_DEPS
						globalsQueue.insert(make_pair(ci.getCalledFunction(),currentFun));
#else
						globalsQueue.insert(ci.getCalledFunction());
#endif
				}
			}
			else
			{
				//Indirect call
				compileOperand(ci.getCalledValue());
			}

			compileMethodArgs(ci.op_begin(),ci.op_begin()+ci.getNumArgOperands());
			stream << ";\n";
			//Only consider the normal successor for PHIs here
			//For each successor output the variables for the phi nodes
			compilePHIOfBlockFromOtherBlock(ci.getNormalDest(), I.getParent());
			break;
		}
		case Instruction::Resume:
		{
			//TODO: support exceptions
			break;
		}
		case Instruction::Br:
		case Instruction::Switch:
			break;
		default:
			stream << "alert('Unsupported code');\n";
			llvm::errs() << "\tImplement terminator inst " << I.getOpcodeName() << '\n';
			break;
	}
}

void DuettoWriter::compileTerminatorInstruction(const TerminatorInst& I,
		const std::map<const BasicBlock*, uint32_t>& blocksMap)
{
	compileTerminatorInstruction(I);
	switch(I.getOpcode())
	{
		case Instruction::Ret:
			break;
		case Instruction::Invoke:
		{
			//TODO: Support unwind
			const InvokeInst& ci=static_cast<const InvokeInst&>(I);
			stream << "__block = " << blocksMap.find(ci.getNormalDest())->second << ";\n";
			break;
		}
		case Instruction::Resume:
		{
			//TODO: support exceptions
			break;
		}
		case Instruction::Br:
		{
			//For each successor output the variables for the phi nodes
			const BranchInst& bi=static_cast<const BranchInst&>(I);
			if(bi.isUnconditional())
			{
				//Generate the PHIs
				compilePHIOfBlockFromOtherBlock(bi.getSuccessor(0), I.getParent());
				stream << "__block = " << blocksMap.find(bi.getSuccessor(0))->second << ";\n";
			}
			else
			{
				//In each branch generate the right PHIs
				stream << "if( ";
				compileOperand(bi.getCondition());
				stream << ") { ";
				compilePHIOfBlockFromOtherBlock(bi.getSuccessor(0), I.getParent());
				stream << "__block = " << blocksMap.find(bi.getSuccessor(0))->second <<
					"; } else {";
				compilePHIOfBlockFromOtherBlock(bi.getSuccessor(1), I.getParent());
				stream << "__block = " << blocksMap.find(bi.getSuccessor(1))->second <<
					";}\n";
			}
			break;
		}
		case Instruction::Switch:
		{
			//Create a JS switch
			const SwitchInst& si=static_cast<const SwitchInst&>(I);
			stream << "switch (";
			compileOperand(si.getCondition());
			stream << "){";
			SwitchInst::ConstCaseIt it=si.case_begin();
			for(;it!=si.case_end();++it)
			{
				stream << "case ";
				compileConstant(it.getCaseValue());
				stream << ":\n__block = " << blocksMap.find(it.getCaseSuccessor())->second <<
					"; break;";
			}
			if(si.getDefaultDest())
				stream << "default:\n__block = " << blocksMap.find(si.getDefaultDest())->second << ";";
			stream << "}\n";
			break;
		}
		default:
			stream << "alert('Unsupported code');\n";
			llvm::errs() << "\tImplement terminator inst " << I.getOpcodeName() << '\n';
			break;
	}
}

bool DuettoWriter::compileNotInlineableInstruction(const Instruction& I)
{
	switch(I.getOpcode())
	{
		case Instruction::Alloca:
		{
			const AllocaInst& ai=static_cast<const AllocaInst&>(I);
			Type* t=ai.getAllocatedType();
			//Alloca returns complete objects or arrays, not pointers
			if(isImmutableType(t))
				stream << '[';
			compileType(t);
			if(isImmutableType(t))
				stream << ']';
			return true;
		}
		case Instruction::Call:
		{
			const CallInst& ci=static_cast<const CallInst&>(I);
			if(ci.getCalledFunction())
			{
				//Direct call
				const char* funcName=ci.getCalledFunction()->getName().data();
				if(handleBuiltinCall(funcName,&ci,ci.op_begin(),ci.op_begin()+ci.getNumArgOperands(),
						!ci.getCalledFunction()->empty()))
				{
					return true;
				}
				stream << '_' << funcName;
				if(!globalsDone.count(ci.getCalledFunction()))
#ifdef DEBUG_GLOBAL_DEPS
					globalsQueue.insert(make_pair(ci.getCalledFunction(),currentFun));
#else
					globalsQueue.insert(ci.getCalledFunction());
#endif
			}
			else
			{
				//Indirect call
				compileOperand(ci.getCalledValue());
			}
			compileMethodArgs(ci.op_begin(),ci.op_begin()+ci.getNumArgOperands());
			return true;
		}
		case Instruction::LandingPad:
		{
			//TODO: Support exceptions
			stream << " alert('Exceptions not supported')";
			//Do not continue block
			return false;
		}
		case Instruction::InsertValue:
		{
			const InsertValueInst& ivi=static_cast<const InsertValueInst&>(I);
			const Value* aggr=ivi.getAggregateOperand();
			Type* t=aggr->getType();
			if(!t->isStructTy())
			{
				llvm::errs() << "insertvalue: Expected struct, found " << *t << "\n";
				llvm::report_fatal_error("Unsupported code found, please report a bug", false);
				return true;
			}
			if(UndefValue::classof(aggr))
			{
				//We have to assemble the type object from scratch
				compileType(t);
				stream << ";\n";
				//Also assign the element
				assert(ivi.getNumIndices()==1);
				//Find the offset to the pointed element
				assert(ivi.hasName());
				printLLVMName(ivi.getName(), LOCAL);
			}
			else
			{
				//Optimize for the assembly of the aggregate values
				assert(aggr->hasOneUse());
				assert(aggr->hasName());
				printLLVMName(aggr->getName(), LOCAL);
			}
			uint32_t offset=ivi.getIndices()[0];
			stream << ".a" << offset << " = ";
			compileOperand(ivi.getInsertedValueOperand());
			return true;
		}
		case Instruction::Load:
		{
			const LoadInst& li=static_cast<const LoadInst&>(I);
			const Value* ptrOp=li.getPointerOperand();
			stream << "(";
			//If the ptrOp has a single use and it'a a GEP
			//we can optimize it
			if(GetElementPtrInst::classof(ptrOp))
			{
				const GetElementPtrInst& gep=static_cast<const GetElementPtrInst&>(*ptrOp);
				compileFastGEPDereference(gep.getOperand(0), gep.idx_begin(), gep.idx_end());
			}
			else if((ConstantExpr::classof(ptrOp)) &&
					cast<ConstantExpr>(ptrOp)->getOpcode()==Instruction::GetElementPtr)
			{
				const ConstantExpr& cgep=static_cast<const ConstantExpr&>(*ptrOp);
				compileFastGEPDereference(cgep.getOperand(0), cgep.op_begin()+1, cgep.op_end());
			}
			else
				compileDereferencePointer(ptrOp, NULL);
			stream << ")";
			return true;
		}
		case Instruction::Store:
		{
			const StoreInst& si=static_cast<const StoreInst&>(I);
			const Value* ptrOp=si.getPointerOperand();
			const Value* valOp=si.getValueOperand();
			//If the ptrOp is a GEP we can optimize the access
			if(GetElementPtrInst::classof(ptrOp))
			{
				const GetElementPtrInst& gep=static_cast<const GetElementPtrInst&>(*ptrOp);
				compileFastGEPDereference(gep.getOperand(0), gep.idx_begin(), gep.idx_end());
			}
			else if((ConstantExpr::classof(ptrOp)) &&
					cast<ConstantExpr>(ptrOp)->getOpcode()==Instruction::GetElementPtr)
			{
				const ConstantExpr& cgep=static_cast<const ConstantExpr&>(*ptrOp);
				compileFastGEPDereference(cgep.getOperand(0), cgep.op_begin()+1, cgep.op_end());
			}
			else
				compileDereferencePointer(ptrOp, NULL);
			stream << " = ";
			compileOperand(valOp, OPERAND_EXPAND_COMPLETE_OBJECTS);
			return true;
		}
		default:
			return compileInlineableInstruction(I);
	}
}

bool DuettoWriter::isI32Type(Type* t) const
{
	return t->isIntegerTy() && static_cast<IntegerType*>(t)->getBitWidth()==32;
}

void DuettoWriter::compileFastGEPDereference(const Value* operand, const Use* idx_begin, const Use* idx_end)
{
	assert(idx_begin!=idx_end);
	compileObjectForPointerGEP(operand, idx_begin, idx_end, NORMAL);
}

const Type* DuettoWriter::compileObjectForPointer(const Value* val, COMPILE_FLAG flag)
{
	assert(val->getType()->isPointerTy());
	if(isGEP(val))
	{
		const User* gep=static_cast<const User*>(val);
		GetElementPtrInst::const_op_iterator it=gep->op_begin()+1;
		//We compile as usual till the last level
		GetElementPtrInst::const_op_iterator itE=gep->op_end()-1;
		return compileObjectForPointerGEP(gep->getOperand(0), it, itE, flag);
	}
	else if(isBitCast(val))
	{
		const User* b=static_cast<const User*>(val);
		//If it is a union, handle below
		if(!isUnion(b->getOperand(0)->getType()->getPointerElementType()))
			return compileObjectForPointer(b->getOperand(0), flag);
	}

	if(flag!=DRY_RUN)
	{
		POINTER_KIND k=getPointerKind(val);
		compilePointer(val, k);
		if(k==REGULAR)
			stream << ".d";
	}
	return NULL;
}

bool DuettoWriter::compileOffsetForPointer(const Value* val, const Type* lastType)
{
	assert(val->getType()->isPointerTy());
	if(isGEP(val))
	{
		const User* gep=static_cast<const User*>(val);
		GetElementPtrInst::const_op_iterator it=gep->op_begin()+1;
		//We compile as usual till the last level
		GetElementPtrInst::const_op_iterator itE=gep->op_end()-1;
		return compileOffsetForPointerGEP(gep->getOperand(0), it, itE, lastType);
	}
	else if(isBitCast(val))
	{
		const User* b=static_cast<const User*>(val);
		//If it is a union, handle below
		if(!isUnion(b->getOperand(0)->getType()->getPointerElementType()))
			return compileOffsetForPointer(b->getOperand(0), lastType);
	}
	else if(getPointerKind(val)!=REGULAR)
		return false;

	compileOperand(val);
	stream << ".o";
	return true;
}

const Type* DuettoWriter::compileObjectForPointerGEP(const Value* val, const Use* it, const Use* const itE, COMPILE_FLAG flag)
{
	Type* t=val->getType();
	if(it==itE)
	{
		//Same level access, we are just computing another pointer from this pointer
		compileObjectForPointer(val, flag);
		return t;
	}
	else
	{
		assert(t->isPointerTy());
		PointerType* ptrT=static_cast<PointerType*>(t);
		//First dereference the pointer
		if(flag!=DRY_RUN)
			compileDereferencePointer(val, *it);
		return compileRecursiveAccessToGEP(ptrT->getElementType(), ++it, itE, flag);
	}
}

bool DuettoWriter::compileOffsetForPointerGEP(const Value* val, const Use* it, const Use* const itE, const Type* lastType)
{
	if(it==itE)
	{
		//Same level access, we are just computing another pointer from this pointer
		bool notFirst=compileOffsetForPointer(val, lastType);
		//Compute the offset
		if(ConstantInt::classof(*itE))
		{
			uint32_t firstElement = getIntFromValue(*itE);
			if(firstElement==0)
				return notFirst;
			if(notFirst)
				stream << '+';
			compileOperand(*itE);
		}
		else
		{
			if(notFirst)
				stream << '+';
			compileOperand(*itE);
		}
	}
	else
	{
		//Now add the offset for the desired element
		if(ConstantInt::classof(*itE))
		{
			uint32_t elementIndex = getIntFromValue(*itE);
			if(elementIndex == 0 && !lastType->isStructTy())
				return false;
			if(lastType->isStructTy())
				stream << "\"a";
			stream << elementIndex;
			if(lastType->isStructTy())
				stream << '"';
		}
		else
		{
			assert(!lastType->isStructTy());
			compileOperand(*itE);
		}
	}
	//TODO: Skip some useless offsets when possible
	return true;
}

void DuettoWriter::compileGEP(const Value* val, const Use* it, const Use* const itE)
{
	assert(val->getType()->isPointerTy());
	stream << "{ d: ";
	const Type* lastType=compileObjectForPointerGEP(val, it, itE, NORMAL);
	stream << ", o: ";
	bool notFirst=compileOffsetForPointerGEP(val, it, itE,lastType);
	if(!notFirst)
		stream << '0';
	stream << '}';
}

uint32_t DuettoWriter::getMaskForBitWidth(int width)
{
	return (1 << width) - 1;
}

void DuettoWriter::compileSignedInteger(const llvm::Value* v)
{
	//We anyway have to use 32 bits for sign extension to work
	uint32_t shiftAmount = 32-v->getType()->getIntegerBitWidth();
	if(shiftAmount==0)
	{
		//Use simpler code
		stream << '(';
		compileOperand(v);
		stream << ">> 0)";
	}
	else
	{
		stream << "((";
		compileOperand(v);
		stream << "<<" << shiftAmount << ")>>" << shiftAmount << ')';
	}
}

void DuettoWriter::compileUnsignedInteger(const llvm::Value* v)
{
	//We anyway have to use 32 bits for sign extension to work
	uint32_t initialSize = v->getType()->getIntegerBitWidth();
	stream << '(';
	if(initialSize == 32)
	{
		//Use simpler code
		compileOperand(v);
		stream << ">>> 0)";
	}
	else
	{
		compileOperand(v);
		stream << " & " << getMaskForBitWidth(initialSize) << ')';
	}
}

/*
 * This can be used for both named instructions and inlined ones
 * NOTE: Call, Ret, Invoke are NEVER inlined
 */
bool DuettoWriter::compileInlineableInstruction(const Instruction& I)
{
	switch(I.getOpcode())
	{
		case Instruction::BitCast:
		{
			const BitCastInst& bi=static_cast<const BitCastInst&>(I);
			Type* src=bi.getSrcTy();
			Type* dst=bi.getDestTy();
			if(!isValidTypeCast(&bi, bi.getOperand(0), src, dst))
			{
				llvm::errs() << "Between:\n\t" << *src << "\n\t" << *dst << "\n";
				llvm::errs() << "warning: Type conversion is not safe, expect issues. And report a bug.\n";
			}
			//Special case unions
			if(src->isPointerTy() && isUnion(src->getPointerElementType()))
			{
				//Find the type
				llvm::Type* elementType = dst->getPointerElementType();
				bool isArray=ArrayType::classof(elementType);
				if(!isArray)
					stream << "{d:";
				stream << "new ";
				compileTypedArrayType((isArray)?elementType->getSequentialElementType():elementType);
				stream << '(';
				compileOperand(bi.getOperand(0));
				stream << ')';
				if(!isArray)
					stream << ", o: 0}";
				return true;
			}

			compileOperand(bi.getOperand(0));
			return true;
		}
		case Instruction::FPToSI:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			stream << '(';
			compileOperand(ci.getOperand(0));
			//Seems to be the fastest way
			//http://jsperf.com/math-floor-vs-math-round-vs-parseint/33
			stream << " >> 0)";
			return true;
		}
		case Instruction::FPToUI:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			stream << '(';
			compileOperand(ci.getOperand(0));
			//Cast to signed anyway
			//ECMA-262 guarantees that (a >> 0) >>> 0
			//is the same as (a >>> 0)
			stream << " >> 0)";
			return true;
		}
		case Instruction::SIToFP:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			stream << "(+";
			compileOperand(ci.getOperand(0));
			stream << ')';
			return true;
		}
		case Instruction::UIToFP:
		{
			const CastInst& ci=static_cast<const CastInst&>(I);
			//We need to cast to unsigned before
			stream << "(+(";
			compileOperand(ci.getOperand(0));
			stream << " >>> 0))";
			return true;
		}
		case Instruction::GetElementPtr:
		{
			const GetElementPtrInst& gep=static_cast<const GetElementPtrInst&>(I);
			Type* t=gep.getOperand(0)->getType();
			assert(t->isPointerTy());
			PointerType* ptrT=static_cast<PointerType*>(t);
			if(isClientType(ptrT->getElementType()))
			{
				//Client objects are just passed through
				compileOperand(gep.getOperand(0));
			}
			else
			{
				const Value* val=gep.getOperand(0);
				GetElementPtrInst::const_op_iterator it=gep.idx_begin();
				//We compile as usual till the last level
				GetElementPtrInst::const_op_iterator itE=gep.idx_end()-1;
				compileGEP(val, it, itE);
			}
			return true;
		}
		case Instruction::Add:
		{
			//Integer addition
			stream << "((";
			compileOperand(I.getOperand(0));
			stream << " + ";
			compileOperand(I.getOperand(1));
			stream << ')';
			if(isI32Type(I.getType()))
				stream << ">> 0";
			else
				stream << "& " << getMaskForBitWidth(I.getType()->getIntegerBitWidth());
			stream << ')';
			return true;
		}
		case Instruction::FAdd:
		{
			//Double addition
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " + ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::Sub:
		{
			//Integer subtraction
			//TODO: optimize negation
			stream << "((";
			compileOperand(I.getOperand(0));
			stream << " - ";
			compileOperand(I.getOperand(1));
			stream << ')';
			if(isI32Type(I.getType()))
				stream << ">> 0";
			else
				stream << "& " << getMaskForBitWidth(I.getType()->getIntegerBitWidth());
			stream << ')';
			return true;
		}
		case Instruction::FSub:
		{
			//Double subtraction
			//TODO: optimize negation
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " - ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::ZExt:
		{
			const BitCastInst& bi=static_cast<const BitCastInst&>(I);
			Type* src=bi.getSrcTy();
#ifndef NDEBUG
			Type* dst=bi.getDestTy();
#endif
			assert(src->isIntegerTy() && dst->isIntegerTy());
			if(src->isIntegerTy(1))
			{
				//If the source type is i1, attempt casting from Boolean
				stream << '(';
				compileOperand(bi.getOperand(0));
				stream << "==false?0:1)";
			}
			else
			{
				//Let's mask out upper bits, to make sure we get zero extension
				//The value might have been initialized with a negative value
				compileUnsignedInteger(I.getOperand(0));
			}
			return true;
		}
		case Instruction::SDiv:
		{
			//Integer signed division
			stream << "((";
			compileSignedInteger(I.getOperand(0));
			stream << " / ";
			compileSignedInteger(I.getOperand(1));
			stream << ") >> 0)";
			return true;
		}
		case Instruction::UDiv:
		{
			//Integer unsigned division
			stream << "((";
			compileUnsignedInteger(I.getOperand(0));
			stream << " / ";
			compileUnsignedInteger(I.getOperand(1));
			stream << ") >>> 0)";
			return true;
		}
		case Instruction::SRem:
		{
			//Integer signed remainder
			stream << "((";
			compileSignedInteger(I.getOperand(0));
			stream << " % ";
			compileSignedInteger(I.getOperand(1));
			stream << ") >> 0)";
			return true;
		}
		case Instruction::URem:
		{
			//Integer unsigned remainder
			stream << "((";
			compileUnsignedInteger(I.getOperand(0));
			stream << " % ";
			compileUnsignedInteger(I.getOperand(1));
			stream << ") >>> 0)";
			return true;
		}
		case Instruction::FDiv:
		{
			//Double division
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " / ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::Mul:
		{
			//Integer signed multiplication
			stream << "((";
			compileOperand(I.getOperand(0));
			stream << " * ";
			compileOperand(I.getOperand(1));
			stream << ')';
			if(isI32Type(I.getType()))
				stream << ">> 0";
			else
				stream << "& " << getMaskForBitWidth(I.getType()->getIntegerBitWidth());
			stream << ')';
			return true;
		}
		case Instruction::FMul:
		{
			//Double multiplication
			stream << "(";
			compileOperand(I.getOperand(0));
			stream << " * ";
			compileOperand(I.getOperand(1));
			stream << ")";
			return true;
		}
		case Instruction::ICmp:
		{
			//Integer comparison
			const CmpInst& ci=static_cast<const CmpInst&>(I);
			stream << '(';
			if(ci.getOperand(0)->getType()->isPointerTy())
			{
				if(ci.getPredicate()==CmpInst::ICMP_EQ ||
				   ci.getPredicate()==CmpInst::ICMP_NE)
				{
					compileEqualPointersComparison(ci.getOperand(0), ci.getOperand(1), ci.getPredicate());
				}
				else
				{
					//Comparison on different bases is anyway undefined, so ignore them
					const Type* lastType1=compileObjectForPointer(ci.getOperand(0), DRY_RUN);
					const Type* lastType2=compileObjectForPointer(ci.getOperand(1), DRY_RUN);
					bool notFirst=compileOffsetForPointer(ci.getOperand(0),lastType1);
					if(!notFirst)
						stream << '0';
					compilePredicate(ci.getPredicate());
					notFirst=compileOffsetForPointer(ci.getOperand(1),lastType2);
					if(!notFirst)
						stream << '0';
				}
			}
			else
			{
				compileOperandForIntegerPredicate(ci.getOperand(0),ci.getPredicate());
				compilePredicate(ci.getPredicate());
				compileOperandForIntegerPredicate(ci.getOperand(1),ci.getPredicate());
			}
			stream << ')';
			return true;
		}
		case Instruction::FCmp:
		{
			//Integer comparison
			const CmpInst& ci=static_cast<const CmpInst&>(I);
			//Check that the operation is JS safe
			stream << "(";
			//Special case orderedness check
			if(ci.getPredicate()==CmpInst::FCMP_ORD)
			{
				stream << "!isNaN(";
				compileOperand(ci.getOperand(0));
				stream << ") && !isNaN(";
				compileOperand(ci.getOperand(1));
				stream << ')';
			}
			else
			{
				compileOperand(ci.getOperand(0));
				compilePredicate(ci.getPredicate());
				compileOperand(ci.getOperand(1));
			}
			stream << ")";
			return true;
		}
		case Instruction::And:
		{
			//Integer logical and
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " & ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::LShr:
		{
			//Integer logical shift right
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " >>> ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::AShr:
		{
			//Integer arithmetic shift right
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			if(isI32Type(I.getOperand(0)->getType()))
				compileOperand(I.getOperand(0));
			else
				compileSignedInteger(I.getOperand(0));
			stream << " >> ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Shl:
		{
			//Integer shift left
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " << ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Or:
		{
			//Integer logical or
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " | ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Xor:
		{
			//Integer logical xor
			//Xor with 1s is used to implement bitwise and logical negation
			//TODO: Optimize the operation with 1s
			//No need to apply the >> operator. The result is an integer by spec
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " ^ ";
			compileOperand(I.getOperand(1));
			stream << ')';
			return true;
		}
		case Instruction::Trunc:
		{
			//Well, ideally this should not be used since, since it's a waste of bit to
			//use integers less than 32 bit wide. Still we can support it
			uint32_t finalSize = I.getType()->getIntegerBitWidth();
			stream << '(';
			compileOperand(I.getOperand(0));
			stream << " & " << getMaskForBitWidth(finalSize) << ')';
			return true;
		}
		case Instruction::SExt:
		{
			//We can use a couple of shift to make this work
			compileSignedInteger(I.getOperand(0));
			return true;
		}
		case Instruction::Select:
		{
			const SelectInst& si=static_cast<const SelectInst&>(I);
			stream << "(";
			compileOperand(si.getCondition());
			stream << "?";
			compileOperand(si.getTrueValue(), OPERAND_EXPAND_COMPLETE_OBJECTS);
			stream << ":";
			compileOperand(si.getFalseValue(), OPERAND_EXPAND_COMPLETE_OBJECTS);
			stream << ")";
			return true;
		}
		case Instruction::ExtractValue:
		{
			const ExtractValueInst& evi=static_cast<const ExtractValueInst&>(I);
			const Value* aggr=evi.getAggregateOperand();
			Type* t=aggr->getType();
			if(!t->isStructTy())
			{
				llvm::errs() << "extractvalue: Expected struct, found " << *t << "\n";
				llvm::report_fatal_error("Unsupported code found, please report a bug", false);
				return true;
			}
			assert(!UndefValue::classof(aggr));

			printVarName(aggr);

			uint32_t offset=evi.getIndices()[0];
			stream << ".a" << offset;
			return true;
		}
		case Instruction::FPExt:
		{
			const Value* src=I.getOperand(0);
			compileOperand(src);
			return true;
		}
		case Instruction::FPTrunc:
		{
			const Value* src=I.getOperand(0);
			compileOperand(src);
			return true;
		}
		case Instruction::PtrToInt:
		{
			const PtrToIntInst& pi=static_cast<const PtrToIntInst&>(I);
			//Comparison between pointers is significant only for pointers in the same array
			POINTER_KIND k=getPointerKind(pi.getOperand(0));
			if(k==REGULAR)
			{
				compileOperand(pi.getOperand(0));
				stream << ".o";
			}
			else
				stream << '0';
			return true;
		}
		case Instruction::VAArg:
		{
			const VAArgInst& vi=static_cast<const VAArgInst&>(I);
			stream << "handleVAArg(";
			compileDereferencePointer(vi.getPointerOperand(), NULL);
			stream << ')';
			return true;
		}
		default:
			stream << "alert('Unsupported code')";
			llvm::errs() << "\tImplement inst " << I.getOpcodeName() << '\n';
			return false;
	}
}

bool DuettoWriter::isInlineable(const Instruction& I) const
{
	//Inlining a variable used by a PHI it's unsafe
	//When the phi's are computed the result
	//correctness may depend on the order they are
	//computed. Check all uses
	Value::const_use_iterator it=I.use_begin();
	Value::const_use_iterator itE=I.use_end();
	for(;it!=itE;++it)
	{
		if(PHINode::classof(*it))
			return false;
	}
	//Beside a few cases, instructions with a single use may be inlined
	//TODO: Find out a better heuristic for inlining, it seems that computing
	//may be faster even on more than 1 use
	if(I.hasOneUse())
	{
		//A few opcodes needs to be executed anyway as they
		//do not operated on registers
		switch(I.getOpcode())
		{
			case Instruction::Call:
			case Instruction::Invoke:
			case Instruction::Ret:
			case Instruction::LandingPad:
			case Instruction::PHI:
			case Instruction::Load:
			case Instruction::Store:
			case Instruction::InsertValue:
			case Instruction::Resume:
			case Instruction::Br:
			case Instruction::Alloca:
			case Instruction::Switch:
			case Instruction::Unreachable:
			case Instruction::VAArg:
				return false;
			case Instruction::Add:
			case Instruction::Sub:
			case Instruction::Mul:
			case Instruction::And:
			case Instruction::Or:
			case Instruction::Xor:
			case Instruction::Trunc:
			case Instruction::FPToSI:
			case Instruction::SIToFP:
			case Instruction::SDiv:
			case Instruction::SRem:
			case Instruction::Shl:
			case Instruction::AShr:
			case Instruction::LShr:
			case Instruction::FAdd:
			case Instruction::FDiv:
			case Instruction::FSub:
			case Instruction::FPTrunc:
			case Instruction::FPExt:
			case Instruction::FMul:
			case Instruction::FCmp:
			case Instruction::ICmp:
			case Instruction::ZExt:
			case Instruction::SExt:
			case Instruction::Select:
			case Instruction::ExtractValue:
			case Instruction::URem:
			case Instruction::UDiv:
			case Instruction::UIToFP:
			case Instruction::FPToUI:
			case Instruction::PtrToInt:
			case Instruction::GetElementPtr:
				return true;
			case Instruction::BitCast:
			{
				//Union are complex, do not inline them
				llvm::Type* src=I.getOperand(0)->getType();
				return !(src->isPointerTy() && isUnion(src->getPointerElementType()));
			}
			default:
				llvm::report_fatal_error(Twine("Unsupported opcode: ",StringRef(I.getOpcodeName())), false);
				return true;
		}
	}
	else if(I.getOpcode()==Instruction::GetElementPtr)
	{
		//Special case GEPs. They should always be inline since creating the object is really slow
		return true;
	}
	return false;
}

void DuettoWriter::compileBB(const BasicBlock& BB, const std::map<const BasicBlock*, uint32_t>& blocksMap)
{
	BasicBlock::const_iterator I=BB.begin();
	BasicBlock::const_iterator IE=BB.end();
	for(;I!=IE;++I)
	{
		if(isInlineable(*I))
			continue;
		if(I->getOpcode()==Instruction::PHI) //Phys are manually handled
			continue;
		if(I->getType()->getTypeID()!=Type::VoidTyID)
		{
			stream << "var ";
			printVarName(&(*I));
			stream << " = ";
		}
		if(I->isTerminator())
		{
			//TODO: Keep support for both relooper and swicth generation
			compileTerminatorInstruction(*dyn_cast<TerminatorInst>(I));
			//compileTerminatorInstruction(*dyn_cast<TerminatorInst>(I), blocksMap);
		}
		else
		{
			bool ret=compileNotInlineableInstruction(*I);
			stream << ";\n";
			if(ret==false)
			{
				//Stop basic block compilation
				return;
			}
		}
	}
	//At the end of the block
}

void DuettoRenderInterface::renderBlock(const void* privateBlock)
{
	const BasicBlock* bb=(const BasicBlock*)privateBlock;
	std::map<const BasicBlock*, uint32_t> blocksMap;
	writer->compileBB(*bb, blocksMap);
}

void DuettoRenderInterface::renderIfBlockBegin(const void* privateBlock, int branchId, bool first)
{
	const BasicBlock* bb=(const BasicBlock*)privateBlock;
	if(!first)
		writer->stream << "} else ";
	const TerminatorInst* term=bb->getTerminator();
	writer->stream << "if (";
	if(BranchInst::classof(term))
	{
		const BranchInst* bi=cast<const BranchInst>(term);
		assert(bi->isConditional());
		//The second branch is the default
		assert(branchId==0);
		writer->compileOperand(bi->getCondition());
	}
	else if(SwitchInst::classof(term))
	{
		const SwitchInst* si=cast<const SwitchInst>(term);
		assert(branchId > 0);
		SwitchInst::ConstCaseIt it=si->case_begin();
		for(int i=1;i<branchId;i++)
			++it;
		const BasicBlock* dest=it.getCaseSuccessor();
		//We found the destination, there may be more cases for the same
		//destination though
		writer->compileOperand(si->getCondition());
		writer->stream << " === ";
		writer->compileConstant(it.getCaseValue());
		for(;it!=si->case_end();++it)
		{
			if(it.getCaseSuccessor()==dest)
			{
				//Also add this condition
				writer->stream << "|| (";
				writer->compileOperand(si->getCondition());
				writer->stream << " === ";
				writer->compileConstant(it.getCaseValue());
				writer->stream << ')';
			}
		}
	}
	else
	{
		term->dump();
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
	}
	writer->stream << ") {\n";
}

void DuettoRenderInterface::renderElseBlockBegin()
{
	writer->stream << "} else {\n";
}

void DuettoRenderInterface::renderBlockEnd()
{
	writer->stream << "}\n";
}

void DuettoRenderInterface::renderBlockPrologue(const void* privateBlockTo, const void* privateBlockFrom)
{
	const BasicBlock* bbTo=(const BasicBlock*)privateBlockTo;
	const BasicBlock* bbFrom=(const BasicBlock*)privateBlockFrom;
	writer->compilePHIOfBlockFromOtherBlock(bbTo, bbFrom);
}

void DuettoRenderInterface::renderWhileBlockBegin()
{
	writer->stream << "while(1) {\n";
}

void DuettoRenderInterface::renderWhileBlockBegin(int blockLabel)
{
	writer->stream << 'L' << blockLabel << ':';
	renderWhileBlockBegin();
}

void DuettoRenderInterface::renderDoBlockBegin()
{
	writer->stream << "do {\n";
}

void DuettoRenderInterface::renderDoBlockBegin(int blockLabel)
{
	writer->stream << 'L' << blockLabel << ':';
	renderDoBlockBegin();
}

void DuettoRenderInterface::renderDoBlockEnd()
{
	writer->stream << "} while(0);\n";
}

void DuettoRenderInterface::renderBreak()
{
	writer->stream << "break;\n";
}

void DuettoRenderInterface::renderBreak(int labelId)
{
	writer->stream << "break L" << labelId << ";\n";
}

void DuettoRenderInterface::renderContinue()
{
	writer->stream << "continue;\n";
}

void DuettoRenderInterface::renderContinue(int labelId)
{
	writer->stream << "continue L" << labelId << ";\n";
}

void DuettoRenderInterface::renderLabel(int labelId)
{
	writer->stream << "label = " << labelId << ";\n";
}

void DuettoRenderInterface::renderIfOnLabel(int labelId, bool first)
{
	if(first==false)
		writer->stream << "else ";
	writer->stream << "if (label === " << labelId << ") {\n";
}

void DuettoWriter::compileMethod(const Function& F)
{
	if(F.empty())
		return;
	globalsDone.insert(&F);
	currentFun = &F;
	stream << "function _" << F.getName() << "(";
	const Function::const_arg_iterator A=F.arg_begin();
	const Function::const_arg_iterator AE=F.arg_end();
	for(Function::const_arg_iterator curArg=A;curArg!=AE;++curArg)
	{
		if(curArg!=A)
			stream << ", ";
		printArgName(curArg);
	}
	stream << ") {\n";
	std::map<const BasicBlock*, uint32_t> blocksMap;
	if(F.size()==1)
		compileBB(*F.begin(), blocksMap);
	else
	{
		stream << "var label = 0;\n";
		//TODO: Support exceptions
		Function::const_iterator B=F.begin();
		Function::const_iterator BE=F.end();
		//First run, create the corresponding relooper blocks
		std::map<const BasicBlock*, /*relooper::*/Block*> relooperMap;
		for(;B!=BE;++B)
		{
			if(B->isLandingPad())
				continue;
			Block* rlBlock = new Block(&(*B));
			relooperMap.insert(make_pair(&(*B),rlBlock));
		}

		B=F.begin();
		BE=F.end();
		//Second run, add the branches
		for(;B!=BE;++B)
		{
			if(B->isLandingPad())
				continue;
			const TerminatorInst* term=B->getTerminator();
			uint32_t defaultBranchId=-1;
			//Find out which branch id is the default
			if(BranchInst::classof(term))
			{
				const BranchInst* bi=cast<const BranchInst>(term);
				if(bi->isUnconditional())
					defaultBranchId = 0;
				else
					defaultBranchId = 1;
			}
			else if(SwitchInst::classof(term))
			{
#ifndef NDEBUG
				const SwitchInst* si=cast<const SwitchInst>(term);
#endif
				assert(si->getDefaultDest()==si->getSuccessor(0));
				defaultBranchId = 0;
			}
			else if(InvokeInst::classof(term))
			{
#ifndef NDEBUG
				const InvokeInst* ii=cast<const InvokeInst>(term);
#endif
				assert(ii->getNormalDest()==ii->getSuccessor(0));
				defaultBranchId = 0;
			}
			else if(term->getNumSuccessors())
			{
				//Only a problem if there are successors
				term->dump();
				llvm::report_fatal_error("Unsupported code found, please report a bug", false);
			}

			for(uint32_t i=0;i<term->getNumSuccessors();i++)
			{
				if(term->getSuccessor(i)->isLandingPad())
					continue;
				Block* target=relooperMap[term->getSuccessor(i)];
				//Use -1 for the default target
				bool ret=relooperMap[&(*B)]->AddBranchTo(target, (i==defaultBranchId)?-1:i);
				if(ret==false) //More than a path for a single block can only happen for switch
					assert(SwitchInst::classof(term));
			}
		}

		B=F.begin();
		BE=F.end();
		//Third run, add the block to the relooper and run it
		Relooper* rl=new Relooper();
		for(;B!=BE;++B)
		{
			if(B->isLandingPad())
				continue;
			rl->AddBlock(relooperMap[&(*B)]);
		}
		rl->Calculate(relooperMap[&F.getEntryBlock()]);
		RenderInterface* ri=new DuettoRenderInterface(this);
		rl->Render(ri);
/*		//Build a map from basicblocks to ids
		uint32_t blockId=0;
		for(;B!=BE;++B)
		{
			blocksMap.insert(make_pair(&(*B), blockId));
			blockId++;
		}
		//Rest the iterator
		B=F.begin();

		//Create an emscripten style switch for now
		stream << "var __block=0;\nwhile(true){\nswitch(__block){\n";
		for(;B!=BE;++B)
		{
			stream << "case " << blocksMap.find(&(*B))->second << ":\n";
			compileBB(*B, blocksMap);
			stream << "break;\n";
		}
		stream << "}}\n";*/
	}

	stream << "}\n";
	currentFun = NULL;
}

/*
 * Use Twine since in most cases the complete string will not be used
 */
void DuettoWriter::gatherDependencies(const Constant* c, const llvm::GlobalVariable* base,
		const Twine& baseName, const Constant* value)
{
	//TODO: Maybe add Function too
	if(ConstantExpr::classof(c))
	{
		const ConstantExpr* ce=cast<const ConstantExpr>(c);
		if(ce->getOpcode()==Instruction::GetElementPtr)
		{
			//TODO: Maybe it's possible to set directly .d in the fixup
			Constant* gepBase = ce->getOperand(0);
			gatherDependencies(gepBase, base, baseName, c);
		}
		else if(ce->getOpcode()==Instruction::BitCast)
		{
			Value* val=ce->getOperand(0);
			gatherDependencies(cast<Constant>(val), base, baseName, c);
		}
	}
	else if(ConstantArray::classof(c))
	{
		const ConstantArray* d=cast<const ConstantArray>(c);
		assert(d->getType()->getNumElements() == d->getNumOperands());
		char buf[12];
		for(uint32_t i=0;i<d->getNumOperands();i++)
		{
			snprintf(buf,12,"[%u]",i);
			gatherDependencies(d->getOperand(i), base, baseName.concat(buf), NULL);
		}
	}
	else if(ConstantStruct::classof(c))
	{
		const ConstantStruct* d=cast<const ConstantStruct>(c);
		assert(d->getType()->getNumElements() == d->getNumOperands());
		char buf[12];
		for(uint32_t i=0;i<d->getNumOperands();i++)
		{
			snprintf(buf,12,".a%u",i);
			gatherDependencies(d->getOperand(i), base, baseName.concat(buf), NULL);
		}
	}
	else if(GlobalAlias::classof(c))
	{
		const GlobalAlias* a=cast<const GlobalAlias>(c);
		gatherDependencies(a->getAliasedGlobal(), base, baseName, NULL);
	}
	else if(GlobalVariable::classof(c))
	{
		assert(c->hasName());
		const GlobalVariable* GV=cast<GlobalVariable>(c);
		if(globalsDone.count(GV))
			return;
		/*
		 * Insert the fixup in the map, if a value is specified (e.g. this global is in a ConstantExpr)
		 * use value, otherwise it's a regular value and you use GV
		 */
		globalsFixupMap.insert(make_pair(GV, Fixup(base, baseName.str(), (value)?value:c)));
		/*
		 * Also add the global to the globals queue
		 */
#ifdef DEBUG_GLOBAL_DEPS
		globalsQueue.insert(make_pair(GV,base));
#else
		globalsQueue.insert(GV);
#endif
	}
	else if(Function::classof(c))
	{
		const Function* F=cast<const Function>(c);
		if(globalsDone.count(F))
			return;
#ifdef DEBUG_GLOBAL_DEPS
		globalsQueue.insert(make_pair(F,base));
#else
		globalsQueue.insert(F);
#endif
	}
}

void DuettoWriter::compileGlobal(const GlobalVariable& G)
{
	if(globalsDone.count(&G))
		return;

	assert(G.hasName());
	if(isClientGlobal(G.getName().data()))
	{
		//Global objects in the client namespace are only
		//placeholders for JS calls
		return;
	}
	stream  << "var ";
	printLLVMName(G.getName(), GLOBAL);
	if(G.hasInitializer())
	{
		stream << " = ";
		const Constant* C=G.getInitializer();

		//Gather the needed fixups
		gatherDependencies(C, &G, "", NULL);

		Type* t=C->getType();
		if(isImmutableType(t))
			stream << '[';
		compileOperand(C, OPERAND_EXPAND_COMPLETE_OBJECTS);
		if(isImmutableType(t))
			stream << ']';
	}
	stream << ";\n";
	globalsDone.insert(&G);
	//Now we have defined a new global, check if there are fixups for previously defined globals
	std::pair<FixupMapType::iterator, FixupMapType::iterator> f=globalsFixupMap.equal_range(&G);
	for(FixupMapType::iterator it=f.first;it!=f.second;++it)
	{
		const GlobalVariable* otherGV=it->second.base;
		if(!otherGV->hasInitializer())
		{
			llvm::errs() << "Expected initializer for ";
			otherGV->dump();
			llvm::errs() << "\n";
			llvm::report_fatal_error("Unsupported code found, please report a bug", false);
			continue;
		}
		const Constant* C=otherGV->getInitializer();

		printLLVMName(otherGV->getName(), GLOBAL);
		if(isImmutableType(C->getType()))
			stream << "[0]";
		stream << it->second.baseName << " = ";
		compileOperand(it->second.value, OPERAND_EXPAND_COMPLETE_OBJECTS);
		stream << ";\n";
	}
	if(f.first!=f.second)
		globalsFixupMap.erase(f.first,f.second);
}

uint32_t DuettoWriter::compileClassTypeRecursive(const std::string& baseName, StructType* currentType, uint32_t baseCount)
{
	stream << "a[" << baseCount << "] = " << baseName << ";\n";
	stream << baseName << ".o=" << baseCount << ";\n";
	stream << baseName << ".a=a;\n";
	baseCount++;

	NamedMDNode* basesNamedMeta=module.getNamedMetadata(Twine(currentType->getName(),"_bases"));
	if(!basesNamedMeta)
		return baseCount;

	MDNode* basesMeta=basesNamedMeta->getOperand(0);
	assert(basesMeta->getNumOperands()==2);
	uint32_t firstBase=getIntFromValue(basesMeta->getOperand(0));
	//baseCount has been already incremented above
	uint32_t baseMax=getIntFromValue(basesMeta->getOperand(1))+(baseCount-1);
	uint32_t offset=0;

	StructType::element_iterator E=currentType->element_begin();
	StructType::element_iterator EE=currentType->element_end();

	for(;E!=EE;++E)
	{
		if(offset >= firstBase)
		{
			char buf[12];
			snprintf(buf,12,".a%u",offset);
			baseCount=compileClassTypeRecursive(baseName + buf, cast<StructType>(*E), baseCount);
			if(baseMax==baseCount)
				break;
		}
		offset++;
	}
	assert(baseMax==baseCount);
	return baseCount;
}

void DuettoWriter::compileClassType(StructType* T)
{
	if(!T->hasName())
	{
		llvm::errs() << "Expected name for struct " << *T << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
		return;
	}
	stream << "function create";
	printLLVMName(T->getName(), GLOBAL);
	stream << "(){\n";

	stream << "var t=";
	//TODO: Currently base classes are initialized also during compileTypeImpl
	//find a way to skip it. It's also necessary to initialize members that require
	//downcast support
	compileTypeImpl(T);
	stream << "\n";

	NamedMDNode* basesNamedMeta=module.getNamedMetadata(Twine(T->getName(),"_bases"));
	if(basesNamedMeta)
	{
		MDNode* basesMeta=basesNamedMeta->getOperand(0);
		assert(basesMeta->getNumOperands()==2);
		uint32_t baseMax=getIntFromValue(basesMeta->getOperand(1));
		stream << "var a=new Array(" << baseMax << ");\n";

		compileClassTypeRecursive("t", T, 0);
	}
	stream << "return t;\n}\n";
}

void DuettoWriter::compileArrayClassType(StructType* T)
{
	if(!T->hasName())
	{
		llvm::errs() << "Expected name for struct " << *T << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
		return;
	}
	stream << "function createArray";
	printLLVMName(T->getName(), GLOBAL);
	stream << "(size){\n";
	stream << "var ret=new Array(size);\nfor(var __i__=0;__i__<size;__i__++)\n";
	stream << "ret[__i__]=";
	compileType(T);
	stream << ";\nreturn ret;\n}\n";
}

void DuettoWriter::handleConstructors(GlobalVariable* GV, CONSTRUCTOR_ACTION action)
{
	assert(GV->hasInitializer());
	Constant* C=GV->getInitializer();
	if(ConstantAggregateZero::classof(C))
		return;
	assert(ConstantArray::classof(C));
	ConstantArray* CA=cast<ConstantArray>(C);
	uint32_t numElements=CA->getType()->getNumElements();
	uint32_t lastPriority=0;
	for(uint32_t i=0;i<numElements;i++)
	{
		if(action==ADD_TO_QUEUE)
		{
			Constant* E=CA->getAggregateElement(i);
			Constant* FA=E->getAggregateElement((unsigned)1);
			assert(Function::classof(FA));
			Function* F=cast<Function>(FA);
			if(globalsDone.count(F))
				continue;
#ifdef DEBUG_GLOBAL_DEPS
			globalsQueue.insert(make_pair(F,GV));
#else
			globalsQueue.insert(F);
#endif
		}
		else if(action==COMPILE)
		{
			Constant* E=CA->getAggregateElement(i);
			Constant* PS=E->getAggregateElement((unsigned)0);
			uint32_t priority = getIntFromValue(PS);
			//TODO: handle priority
			assert(priority>=lastPriority);
			lastPriority=priority;
			Constant* F=E->getAggregateElement((unsigned)1);
			assert(Function::classof(F) && F->hasName());
			printLLVMName(F->getName(), GLOBAL);
			stream << "();\n";
		}
	}
}

void DuettoWriter::compileNullPtrs()
{
	stream << "var nullArray = [null];\nvar nullObj = { d: nullArray, o: 0 };\n";
}

void DuettoWriter::makeJS()
{
	Function* webMain=module.getFunction("_Z7webMainv");
	if(webMain==NULL)
	{
		llvm::report_fatal_error("No webMain entry point found", false);
		return;
	}
#ifdef DEBUG_GLOBAL_DEPS
	globalsQueue.insert(make_pair(webMain,(const GlobalValue*)NULL));
#else
	globalsQueue.insert(webMain);
#endif
	//Add the constructors
	GlobalVariable* constructors=module.getGlobalVariable("llvm.global_ctors");
	if(constructors)
		handleConstructors(constructors, ADD_TO_QUEUE);

	compileNullPtrs();

	while(!globalsQueue.empty())
	{
#ifdef DEBUG_GLOBAL_DEPS
		std::map<const GlobalValue*, const GlobalValue*>::iterator it=globalsQueue.begin();
		const GlobalValue* v=it->first;
#else
		std::set<const GlobalValue*>::iterator it=globalsQueue.begin();
		const GlobalValue* v=*it;
#endif
		//printMethodNames=true;
		if(printMethodNames)
		{
			llvm::errs() << v->getName();
#ifdef DEBUG_GLOBAL_DEPS
			if(it->second)
				llvm::errs() << " included by " << it->second->getName();
#endif
			llvm::errs() << "\n";
		}
		globalsQueue.erase(it);
		if(GlobalVariable::classof(v))
		{
			const GlobalVariable* GV=cast<const GlobalVariable>(v);
			compileGlobal(*GV);
		}
		else if(Function::classof(v))
		{
			const Function* F=cast<const Function>(v);
			compileMethod(*F);
		}
	}

	std::set<StructType*> classesDone;
	while(!classesNeeded.empty())
	{
		std::set<StructType*>::const_iterator CN=classesNeeded.begin();
		StructType* st=*CN;
		classesNeeded.erase(CN);
		if(!classesDone.count(st))
		{
			compileClassType(st);
			classesDone.insert(st);
		}
	}

	std::set<StructType*>::const_iterator T=arraysNeeded.begin();
	std::set<StructType*>::const_iterator TE=arraysNeeded.end();
	for (; T != TE; ++T)
	{
		compileArrayClassType(*T);
	}
	//Execute constructors
	if(constructors)
		handleConstructors(constructors, COMPILE);
	//Invoke the webMain function
	stream << "__Z7webMainv();\n";
}
