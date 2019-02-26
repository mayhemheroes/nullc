#include "ExpressionTree.h"

#include "BinaryCache.h"
#include "Bytecode.h"
#include "ExpressionEval.h"

#define FMT_ISTR(x) unsigned(x.end - x.begin), x.begin

#if defined(_MSC_VER)
#pragma warning(disable: 4702) // unreachable code
#endif

void AddErrorLocationInfo(const char *codeStart, const char *errorPos, char *errorBuf, unsigned errorBufSize);
InplaceStr FindModuleNameWithSourceLocation(ExpressionContext &ctx, const char *position);
const char* FindModuleCodeWithSourceLocation(ExpressionContext &ctx, const char *position);

namespace
{
	void Report(ExpressionContext &ctx, const char *pos, const char *msg, va_list args)
	{
		if(ctx.errorBuf && ctx.errorBufSize)
		{
			if(ctx.errorCount == 0)
			{
				ctx.errorPos = pos;
				ctx.errorBufLocation = ctx.errorBuf;
			}

			vsnprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), msg, args);
			ctx.errorBuf[ctx.errorBufSize - 1] = '\0';

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);

			if(const char *code = FindModuleCodeWithSourceLocation(ctx, pos))
			{
				AddErrorLocationInfo(code, pos, ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf));

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);

				if(code != ctx.code)
				{
					InplaceStr parentModule = FindModuleNameWithSourceLocation(ctx, pos);

					if(!parentModule.empty())
					{
						SafeSprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), " [in module '%.*s']\n", FMT_ISTR(parentModule));

						ctx.errorBufLocation += strlen(ctx.errorBufLocation);
					}
				}
			}
		}

		if(ctx.errorHandlerNested)
		{
			assert(ctx.errorHandlerActive);

			longjmp(ctx.errorHandler, 1);
		}

		ctx.errorCount++;

		if(ctx.errorCount == 100)
		{
			SafeSprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), "ERROR: error limit reached");

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);

			assert(ctx.errorHandlerActive);

			longjmp(ctx.errorHandler, 1);
		}
	}

	NULLC_PRINT_FORMAT_CHECK(3, 4) void Report(ExpressionContext &ctx, const char *pos, const char *msg, ...)
	{
		va_list args;
		va_start(args, msg);

		Report(ctx, pos, msg, args);
	}

	NULLC_PRINT_FORMAT_CHECK(3, 4) void Report(ExpressionContext &ctx, InplaceStr pos, const char *msg, ...)
	{
		va_list args;
		va_start(args, msg);

		Report(ctx, pos.begin, msg, args);
	}

	NULLC_PRINT_FORMAT_CHECK(4, 5) ExprError* ReportExpected(ExpressionContext &ctx, SynBase *source, TypeBase *type, const char *msg, ...)
	{
		va_list args;
		va_start(args, msg);

		Report(ctx, source->pos.begin, msg, args);

		return new (ctx.get<ExprError>()) ExprError(source, type);
	}

	void Stop(ExpressionContext &ctx, const char *pos, const char *msg, va_list args)
	{
		Report(ctx, pos, msg, args);

		assert(ctx.errorHandlerActive);

		longjmp(ctx.errorHandler, 1);
	}

	NULLC_PRINT_FORMAT_CHECK(3, 4) void Stop(ExpressionContext &ctx, const char *pos, const char *msg, ...)
	{
		va_list args;
		va_start(args, msg);

		Stop(ctx, pos, msg, args);
	}

	NULLC_PRINT_FORMAT_CHECK(3, 4) void Stop(ExpressionContext &ctx, InplaceStr pos, const char *msg, ...)
	{
		va_list args;
		va_start(args, msg);

		Stop(ctx, pos.begin, msg, args);
	}

	unsigned char ParseEscapeSequence(ExpressionContext &ctx, const char* str)
	{
		assert(str[0] == '\\');

		switch(str[1])
		{
		case 'n':
			return '\n';
		case 'r':
			return '\r';
		case 't':
			return '\t';
		case '0':
			return '\0';
		case '\'':
			return '\'';
		case '\"':
			return '\"';
		case '\\':
			return '\\';
		}

		Report(ctx, str, "ERROR: unknown escape sequence");

		return 0;
	}

	int ParseInteger(ExpressionContext &ctx, const char* str)
	{
		(void)ctx;

		unsigned digit;
		int a = 0;

		while((digit = *str - '0') < 10)
		{
			a = a * 10 + digit;
			str++;
		}

		return a;
	}

	unsigned long long ParseLong(ExpressionContext &ctx, const char* s, const char* e, int base)
	{
		unsigned long long res = 0;

		for(const char *p = s; p < e; p++)
		{
			int digit = ((*p >= '0' && *p <= '9') ? *p - '0' : (*p & ~0x20) - 'A' + 10);

			if(digit < 0 || digit >= base)
				Report(ctx, p, "ERROR: digit %d is not allowed in base %d", digit, base);

			unsigned long long prev = res;

			res = res * base + digit;

			if(res < prev)
				Stop(ctx, s, "ERROR: overflow in integer constant");
		}

		return res;
	}

	double ParseDouble(ExpressionContext &ctx, const char *str)
	{
		unsigned digit;
		double integer = 0.0;

		while((digit = *str - '0') < 10)
		{
			integer = integer * 10.0 + digit;
			str++;
		}

		double fractional = 0.0;
	
		if(*str == '.')
		{
			double power = 0.1f;
			str++;

			while((digit = *str - '0') < 10)
			{
				fractional = fractional + power * digit;
				power /= 10.0;
				str++;
			}
		}

		if(*str == 'e')
		{
			str++;

			if(*str == '-')
				return (integer + fractional) * pow(10.0, (double)-ParseInteger(ctx, str + 1));
			else
				return (integer + fractional) * pow(10.0, (double)ParseInteger(ctx, str));
		}

		return integer + fractional;
	}

	bool IsBinaryOp(SynUnaryOpType type)
	{
		return type == SYN_UNARY_OP_BIT_NOT;
	}

	bool IsLogicalOp(SynUnaryOpType type)
	{
		return type == SYN_UNARY_OP_LOGICAL_NOT;
	}

	bool IsBinaryOp(SynBinaryOpType type)
	{
		switch(type)
		{
		case SYN_BINARY_OP_SHL:
		case SYN_BINARY_OP_SHR:
		case SYN_BINARY_OP_BIT_AND:
		case SYN_BINARY_OP_BIT_OR:
		case SYN_BINARY_OP_BIT_XOR:
			return true;
		default:
			break;
		}

		return false;
	}

	bool IsComparisonOp(SynBinaryOpType type)
	{
		switch(type)
		{
		case SYN_BINARY_OP_LESS:
		case SYN_BINARY_OP_LESS_EQUAL:
		case SYN_BINARY_OP_GREATER:
		case SYN_BINARY_OP_GREATER_EQUAL:
		case SYN_BINARY_OP_EQUAL:
		case SYN_BINARY_OP_NOT_EQUAL:
			return true;
		default:
			break;
		}

		return false;
	}

	bool IsLogicalOp(SynBinaryOpType type)
	{
		switch(type)
		{
		case SYN_BINARY_OP_LOGICAL_AND:
		case SYN_BINARY_OP_LOGICAL_OR:
		case SYN_BINARY_OP_LOGICAL_XOR:
			return true;
		default:
			break;
		}

		return false;
	}

	SynBinaryOpType GetBinaryOpType(SynModifyAssignType type)
	{
		switch(type)
		{
		case SYN_MODIFY_ASSIGN_ADD:
			return SYN_BINARY_OP_ADD;
		case SYN_MODIFY_ASSIGN_SUB:
			return SYN_BINARY_OP_SUB;
		case SYN_MODIFY_ASSIGN_MUL:
			return SYN_BINARY_OP_MUL;
		case SYN_MODIFY_ASSIGN_DIV:
			return SYN_BINARY_OP_DIV;
		case SYN_MODIFY_ASSIGN_POW:
			return SYN_BINARY_OP_POW;
		case SYN_MODIFY_ASSIGN_MOD:
			return SYN_BINARY_OP_MOD;
		case SYN_MODIFY_ASSIGN_SHL:
			return SYN_BINARY_OP_SHL;
		case SYN_MODIFY_ASSIGN_SHR:
			return SYN_BINARY_OP_SHR;
		case SYN_MODIFY_ASSIGN_BIT_AND:
			return SYN_BINARY_OP_BIT_AND;
		case SYN_MODIFY_ASSIGN_BIT_OR:
			return SYN_BINARY_OP_BIT_OR;
		case SYN_MODIFY_ASSIGN_BIT_XOR:
			return SYN_BINARY_OP_BIT_XOR;
		default:
			break;
		}

		return SYN_BINARY_OP_UNKNOWN;
	}

	ScopeData* NamedOrGlobalScopeFrom(ScopeData *scope)
	{
		if(!scope || scope->ownerNamespace || scope->scope == NULL)
			return scope;

		return NamedOrGlobalScopeFrom(scope->scope);
	}

	TypeBase* FindNextTypeFromScope(ScopeData *scope)
	{
		if(!scope)
			return NULL;

		if(scope->ownerType)
			return scope->ownerType;

		return FindNextTypeFromScope(scope->scope);
	}

	unsigned AllocateGlobalVariable(ExpressionContext &ctx, SynBase *source, unsigned alignment, long long size)
	{
		assert((alignment & (alignment - 1)) == 0 && alignment <= 16);

		ScopeData *scope = ctx.globalScope;

		scope->dataSize += GetAlignmentOffset(scope->dataSize, alignment);

		unsigned result = unsigned(scope->dataSize);

		if(result + size > (1 << 24))
			ctx.Stop(source->pos, "ERROR: variable size limit exceeded");

		scope->dataSize += size;

		return result;
	}

	unsigned AllocateVariableInScope(ExpressionContext &ctx, SynBase *source, unsigned alignment, long long size)
	{
		assert((alignment & (alignment - 1)) == 0 && alignment <= 16);

		ScopeData *scope = ctx.scope;

		while(scope->scope)
		{
			// Don't let allocations escape the temporary scope
			if(scope->type == SCOPE_TEMPORARY)
			{
				return 0;
			}

			if(scope->ownerFunction)
			{
				scope->dataSize += GetAlignmentOffset(scope->dataSize, alignment);

				unsigned result = unsigned(scope->dataSize);

				if(result + size > (1 << 24))
					ctx.Stop(source->pos, "ERROR: variable size limit exceeded");

				scope->dataSize += size;

				scope->ownerFunction->stackSize = scope->dataSize;

				return result;
			}

			if(scope->ownerType)
			{
				scope->dataSize += GetAlignmentOffset(scope->dataSize, alignment);

				unsigned result = unsigned(scope->dataSize);

				if(result + size > (1 << 24))
					ctx.Stop(source->pos, "ERROR: variable size limit exceeded");

				scope->dataSize += size;

				scope->ownerType->size = scope->dataSize;

				return result;
			}

			scope = scope->scope;
		}

		assert(scope == ctx.globalScope);

		return AllocateGlobalVariable(ctx, source, alignment, size);
	}

	unsigned AllocateVariableInScope(ExpressionContext &ctx, SynBase *source, unsigned alignment, TypeBase *type)
	{
		if(TypeClass *typeClass = getType<TypeClass>(type))
		{
			if(!typeClass->completed)
				Stop(ctx, source->pos, "ERROR: type '%.*s' is not fully defined", FMT_ISTR(type->name));
		}

		return AllocateVariableInScope(ctx, source, alignment, type->size);
	}

	unsigned AllocateArgumentInScope(ExpressionContext &ctx, SynBase *source, unsigned alignment, TypeBase *type)
	{
		if(TypeClass *typeClass = getType<TypeClass>(type))
		{
			if(!typeClass->completed)
				Stop(ctx, source->pos, "ERROR: type '%.*s' is not fully defined", FMT_ISTR(type->name));
		}

		return AllocateVariableInScope(ctx, source, alignment, type->size >= 4 ? type->size : 4);
	}
	
	NamespaceData* FindNamespaceInCurrentScope(ExpressionContext &ctx, InplaceStr name)
	{
		ArrayView<NamespaceData*> namespaces;

		if(NamespaceData *ns = ctx.GetCurrentNamespace())
			namespaces = ns->children;
		else
			namespaces = ctx.globalNamespaces;

		for(unsigned i = 0; i < namespaces.size(); i++)
		{
			if(namespaces[i]->name == name)
				return namespaces[i];
		}

		return NULL;
	}

	void CheckVariableConflict(ExpressionContext &ctx, SynBase *source, InplaceStr name)
	{
		if(ctx.typeMap.find(name.hash()))
			Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a class", FMT_ISTR(name));

		if(VariableData **variable = ctx.variableMap.find(name.hash()))
		{
			if((*variable)->scope == ctx.scope)
				Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a variable in current scope", FMT_ISTR(name));
		}

		if(FunctionData **functions = ctx.functionMap.find(name.hash()))
		{
			if((*functions)->scope == ctx.scope)
				Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a function", FMT_ISTR(name));
		}

		if(FindNamespaceInCurrentScope(ctx, name))
			Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a namespace", FMT_ISTR(name));
	}

	void CheckFunctionConflict(ExpressionContext &ctx, SynBase *source, InplaceStr name)
	{
		if(FunctionData **function = ctx.functionMap.find(name.hash()))
		{
			if((*function)->isInternal)
				Stop(ctx, source->pos, "ERROR: function '%.*s' is reserved", FMT_ISTR(name));
		}
	}

	void CheckTypeConflict(ExpressionContext &ctx, SynBase *source, InplaceStr name)
	{
		if(VariableData **variable = ctx.variableMap.find(name.hash()))
		{
			if((*variable)->scope == ctx.scope)
				Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a variable in current scope", FMT_ISTR(name));
		}

		if(FindNamespaceInCurrentScope(ctx, name))
			Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a namespace", FMT_ISTR(name));
	}

	void CheckNamespaceConflict(ExpressionContext &ctx, SynBase *source, NamespaceData *ns)
	{
		if(ctx.typeMap.find(ns->fullNameHash))
			Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a class", FMT_ISTR(ns->name));

		if(VariableData **variable = ctx.variableMap.find(ns->nameHash))
		{
			if((*variable)->scope == ctx.scope)
				Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a variable in current scope", FMT_ISTR(ns->name));
		}

		if(FunctionData **functions = ctx.functionMap.find(ns->nameHash))
		{
			if((*functions)->scope == ctx.scope)
				Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a function", FMT_ISTR(ns->name));
		}
	}

	bool IsArgumentVariable(FunctionData *function, VariableData *data)
	{
		for(VariableHandle *curr = function->argumentVariables.head; curr; curr = curr->next)
		{
			if(data == curr->variable)
				return true;
		}

		if(data == function->contextArgument)
			return true;

		return false;
	}

	bool IsLookupOnlyVariable(ExpressionContext &ctx, VariableData *variable)
	{
		FunctionData *currentFunction = ctx.GetCurrentFunction();
		FunctionData *variableFunctionOwner = ctx.GetFunctionOwner(variable->scope);

		if(currentFunction && variableFunctionOwner)
		{
			if(variableFunctionOwner != currentFunction)
				return true;

			if(currentFunction->coroutine && !IsArgumentVariable(currentFunction, variable))
				return true;
		}

		return false;
	}

	VariableData* AllocateClassMember(ExpressionContext &ctx, SynBase *source, unsigned alignment, TypeBase *type, InplaceStr name, bool readonly, unsigned uniqueId)
	{
		if(alignment == 0)
			alignment = type->alignment;

		unsigned offset = AllocateVariableInScope(ctx, source, alignment, type);

		assert(!type->isGeneric);

		VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, alignment, type, name, offset, uniqueId);

		variable->isReadonly = readonly;

		ctx.AddVariable(variable);

		return variable;
	}

	VariableData* AllocateTemporary(ExpressionContext &ctx, SynBase *source, TypeBase *type)
	{
		char *name = (char*)ctx.allocator->alloc(16);
		sprintf(name, "$temp%d", ctx.unnamedVariableCount++);

		assert(!type->isGeneric);

		VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, type->alignment, type, InplaceStr(name), 0, ctx.uniqueVariableId++);

		if (IsLookupOnlyVariable(ctx, variable))
			variable->lookupOnly = true;

		variable->isAlloca = true;
		variable->offset = ~0u;

		ctx.AddVariable(variable);

		return variable;
	}

	void FinalizeAlignment(TypeStruct *type)
	{
		unsigned maximumAlignment = 0;

		// Additional padding may apply to preserve the alignment of members
		for(VariableHandle *curr = type->members.head; curr; curr = curr->next)
		{
			maximumAlignment = maximumAlignment > curr->variable->alignment ? maximumAlignment : curr->variable->alignment;

			if(curr->variable->type->hasPointers)
				type->hasPointers = true;
		}

		// If explicit alignment is not specified, then class must be aligned to the maximum alignment of the members
		if(type->alignment == 0)
			type->alignment = maximumAlignment;

		// In NULLC, all classes have sizes multiple of 4, so add additional padding if necessary
		maximumAlignment = type->alignment < 4 ? 4 : type->alignment;

		if(type->size % maximumAlignment != 0)
		{
			type->padding = maximumAlignment - (type->size % maximumAlignment);

			type->size += type->padding;
			type->typeScope->dataSize += type->padding;
		}
	}

	FunctionData* ImplementPrototype(ExpressionContext &ctx, FunctionData *function)
	{
		ArrayView<FunctionData*> functions = ctx.scope->functions;

		for(unsigned i = 0, e = functions.count; i < e; i++)
		{
			FunctionData *curr = functions.data[i];

			// Skip current function
			if(curr == function)
				continue;

			// TODO: generic function list

			if(curr->isPrototype && curr->type == function->type && curr->name.name == function->name.name)
			{
				curr->implementation = function;

				ctx.HideFunction(curr);

				return curr;
			}
		}

		if(function->scope->ownerType)
		{
			HashMap<FunctionData*>::Node *curr = ctx.functionMap.first(function->nameHash);

			while(curr)
			{
				// Skip current function
				if(curr->value == function)
				{
					curr = ctx.functionMap.next(curr);
					continue;
				}

				if(curr->value->isPrototype && /*SameGenerics(curr->value->generics, function->generics) &&*/ curr->value->type == function->type)
				{
					curr->value->implementation = function;

					ctx.HideFunction(curr->value);

					return curr->value;
				}

				curr = ctx.functionMap.next(curr);
			}
		}

		return NULL;
	}

	bool SameGenerics(IntrusiveList<MatchData> a, IntrusiveList<TypeHandle> b)
	{
		if(a.size() != b.size())
			return false;

		MatchData *ca = a.head;
		TypeHandle *cb = b.head;

		for(; ca && cb; ca = ca->next, cb = cb->next)
		{
			if(ca->type != cb->type)
				return false;
		}

		return true;
	}

	bool SameGenerics(IntrusiveList<MatchData> a, IntrusiveList<MatchData> b)
	{
		if(a.size() != b.size())
			return false;

		MatchData *ca = a.head;
		MatchData *cb = b.head;

		for(; ca && cb; ca = ca->next, cb = cb->next)
		{
			if(ca->type != cb->type)
				return false;
		}

		return true;
	}

	bool SameArguments(TypeFunction *a, TypeFunction *b)
	{
		TypeHandle *ca = a->arguments.head;
		TypeHandle *cb = b->arguments.head;

		for(; ca && cb; ca = ca->next, cb = cb->next)
		{
			if(ca->type != cb->type)
				return false;
		}

		return ca == cb;
	}

	FunctionData* CheckUniqueness(ExpressionContext &ctx, FunctionData* function)
	{
		HashMap<FunctionData*>::Node *curr = ctx.functionMap.first(function->nameHash);

		while(curr)
		{
			// Skip current function
			if(curr->value == function)
			{
				curr = ctx.functionMap.next(curr);
				continue;
			}

			if(SameGenerics(curr->value->generics, function->generics) && curr->value->type == function->type)
				return curr->value;

			curr = ctx.functionMap.next(curr);
		}

		return NULL;
	}

	unsigned GetDerivedFromDepth(TypeClass *type, TypeClass *target)
	{
		unsigned depth = 0;

		while(type)
		{
			if(target == type)
				return depth;

			depth++;

			type = type->baseClass;
		}

		return ~0u;
	}

	bool IsDerivedFrom(TypeClass *type, TypeClass *target)
	{
		return GetDerivedFromDepth(type, target) != ~0u;
	}

	ExprBase* EvaluateExpression(ExpressionContext &ctx, ExprBase *expression)
	{
		ExpressionEvalContext evalCtx(ctx, ctx.allocator);

		if(ctx.errorBuf && ctx.errorBufSize)
		{
			evalCtx.errorBuf = ctx.errorBufLocation ? ctx.errorBufLocation : ctx.errorBuf;
			evalCtx.errorBufSize = ctx.errorBufSize - (ctx.errorBufLocation ? unsigned(ctx.errorBufLocation - ctx.errorBuf) : 0);
		}

		evalCtx.globalFrame = new (ctx.get<ExpressionEvalContext::StackFrame>()) ExpressionEvalContext::StackFrame(ctx.allocator, NULL);
		evalCtx.stackFrames.push_back(evalCtx.globalFrame);

		ExprBase *result = Evaluate(evalCtx, expression);

		if(evalCtx.errorCritical)
		{
			if(ctx.errorBuf && ctx.errorBufSize)
			{
				if(ctx.errorCount == 0)
				{
					ctx.errorPos = expression->source->pos.begin;
					ctx.errorBufLocation = ctx.errorBuf;
				}

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);
			}

			longjmp(ctx.errorHandler, 1);
		}

		return result;
	}
}

ExpressionContext::ExpressionContext(Allocator *allocator): allocator(allocator)
{
	code = NULL;

	baseModuleFunctionCount = 0;

	namespaces.set_allocator(allocator);
	types.set_allocator(allocator);
	functions.set_allocator(allocator);
	variables.set_allocator(allocator);
	definitions.set_allocator(allocator);
	vtables.set_allocator(allocator);
	upvalues.set_allocator(allocator);

	functionTypes.set_allocator(allocator);
	functionSetTypes.set_allocator(allocator);
	genericAliasTypes.set_allocator(allocator);
	genericClassTypes.set_allocator(allocator);

	genericTypeMap.set_allocator(allocator);
	typeMap.set_allocator(allocator);
	functionMap.set_allocator(allocator);
	variableMap.set_allocator(allocator);

	errorHandlerActive = false;
	errorHandlerNested = false;
	errorPos = NULL;
	errorCount = 0;
	errorBuf = NULL;
	errorBufSize = 0;
	errorBufLocation = NULL;

	typeVoid = NULL;

	typeBool = NULL;

	typeChar = NULL;
	typeShort = NULL;
	typeInt = NULL;
	typeLong = NULL;

	typeFloat = NULL;
	typeDouble = NULL;

	typeTypeID = NULL;
	typeFunctionID = NULL;
	typeNullPtr = NULL;

	typeAuto = NULL;
	typeAutoRef = NULL;
	typeAutoArray = NULL;

	typeMap.init();
	functionMap.init();
	variableMap.init();

	scope = NULL;

	globalScope = NULL;

	instanceDepth = 0;

	genericTypeMap.init();

	uniqueNamespaceId = 0;
	uniqueVariableId = 0;
	uniqueFunctionId = 0;
	uniqueAliasId = 0;
	uniqueScopeId = 0;

	unnamedFuncCount = 0;
	unnamedVariableCount = 0;
}

void ExpressionContext::Stop(const char *pos, const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	::Stop(*this, pos, msg, args);

	va_end(args);
}

void ExpressionContext::Stop(InplaceStr pos, const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	::Stop(*this, pos.begin, msg, args);
}

void ExpressionContext::PushScope(ScopeType type)
{
	ScopeData *next = new (get<ScopeData>()) ScopeData(allocator, scope, uniqueScopeId++, type);

	if(scope)
	{
		scope->scopes.push_back(next);

		next->startOffset = scope->dataSize;
	}

	scope = next;
}

void ExpressionContext::PushScope(NamespaceData *nameSpace)
{
	ScopeData *next = new (get<ScopeData>()) ScopeData(allocator, scope, uniqueScopeId++, nameSpace);

	if(scope)
	{
		scope->scopes.push_back(next);

		next->startOffset = scope->dataSize;
	}

	scope = next;
}

void ExpressionContext::PushScope(FunctionData *function)
{
	ScopeData *next = new (get<ScopeData>()) ScopeData(allocator, scope, uniqueScopeId++, function);

	if(scope)
		scope->scopes.push_back(next);

	scope = next;
}

void ExpressionContext::PushScope(TypeBase *type)
{
	ScopeData *next = new (get<ScopeData>()) ScopeData(allocator, scope, uniqueScopeId++, type);

	if(scope)
		scope->scopes.push_back(next);

	scope = next;
}

void ExpressionContext::PushLoopScope(bool allowBreak, bool allowContinue)
{
	ScopeData *next = new (get<ScopeData>()) ScopeData(allocator, scope, uniqueScopeId++, SCOPE_LOOP);

	if(scope)
	{
		scope->scopes.push_back(next);

		next->startOffset = scope->dataSize;
	}

	if(allowBreak)
		next->breakDepth++;

	if(allowContinue)
		next->contiueDepth++;

	scope = next;
}

void ExpressionContext::PushTemporaryScope()
{
	scope = new (get<ScopeData>()) ScopeData(allocator, scope, 0, SCOPE_TEMPORARY);
}

void ExpressionContext::PopScope(ScopeType scopeType, SynBase *location, bool keepFunctions)
{
	(void)scopeType;
	assert(scope->type == scopeType);

	// When namespace scope ends, all the contents remain accessible through an outer namespace/global scope
	if(!location && scope->ownerNamespace)
	{
		ScopeData *adopter = scope->scope;

		while(!adopter->ownerNamespace && adopter->scope)
			adopter = adopter->scope;

		adopter->variables.push_back(scope->variables.data, scope->variables.size());
		adopter->functions.push_back(scope->functions.data, scope->functions.size());
		adopter->types.push_back(scope->types.data, scope->types.size());
		adopter->aliases.push_back(scope->aliases.data, scope->aliases.size());

		adopter->allVariables.push_back(scope->allVariables.data, scope->allVariables.size());

		scope->variables.clear();
		scope->functions.clear();
		scope->types.clear();
		scope->aliases.clear();

		scope->allVariables.clear();

		scope = scope->scope;
		return;
	}

	// Full set of scope variables is moved to the outer scope untill we reach function, namespace or a global scope
	if(!location && scope->scope && (scope->type == SCOPE_EXPLICIT || scope->type == SCOPE_LOOP))
	{
		scope->scope->allVariables.push_back(scope->allVariables.data, scope->allVariables.size());

		scope->allVariables.clear();
	}

	// Remove scope members from lookup maps
	for(int i = int(scope->variables.count) - 1; i >= 0; i--)
	{
		VariableData *variable = scope->variables[i];

		if(variableMap.find(variable->nameHash, variable))
			variableMap.remove(variable->nameHash, variable);
	}

	if(!keepFunctions)
	{
		for(int i = int(scope->functions.count) - 1; i >= 0; i--)
		{
			FunctionData *function = scope->functions[i];

			// Keep class functions visible
			if(function->scope->ownerType)
				continue;

			if(scope->scope && function->isPrototype && !function->implementation)
				Stop(function->source->pos, "ERROR: local function '%.*s' went out of scope unimplemented", FMT_ISTR(function->name.name));

			if(functionMap.find(function->nameHash, function))
				functionMap.remove(function->nameHash, function);
		}
	}

	for(int i = int(scope->types.count) - 1; i >= 0; i--)
	{
		TypeBase *type = scope->types[i];

		if(typeMap.find(type->nameHash, type))
			typeMap.remove(type->nameHash, type);
	}

	for(int i = int(scope->aliases.count) - 1; i >= 0; i--)
	{
		AliasData *alias = scope->aliases[i];

		if(typeMap.find(alias->nameHash, alias->type))
			typeMap.remove(alias->nameHash, alias->type);
	}

	for(int i = int(scope->shadowedVariables.count) - 1; i >= 0; i--)
	{
		VariableData *variable = scope->shadowedVariables[i];

		variableMap.insert(variable->nameHash, variable);
	}

	scope = scope->scope;
}

void ExpressionContext::PopScope(ScopeType type)
{
	PopScope(type, 0, false);
}

void ExpressionContext::RestoreScopesAtPoint(ScopeData *target, SynBase *location)
{
	// Restore parent first, up to the current scope
	if(target->scope != scope)
		RestoreScopesAtPoint(target->scope, location);

	for(unsigned i = 0, e = target->variables.count; i < e; i++)
	{
		VariableData *variable = target->variables.data[i];

		if(!location || variable->importModule != NULL || variable->source->pos.begin <= location->pos.begin)
			variableMap.insert(variable->nameHash, variable);
	}

	// For functions, restore only the variable shadowing state
	for(unsigned i = 0, e = target->functions.count; i < e; i++)
	{
		FunctionData *function = target->functions.data[i];

		if(!location || function->importModule != NULL || function->source->pos.begin <= location->pos.begin)
		{
			while(VariableData **variable = variableMap.find(function->nameHash))
				variableMap.remove(function->nameHash, *variable);
		}
	}

	for(unsigned i = 0, e = target->types.count; i < e; i++)
	{
		TypeBase *type = target->types.data[i];

		if(TypeClass *exact = getType<TypeClass>(type))
		{
			if(!location || exact->importModule != NULL || exact->source->pos.begin <= location->pos.begin)
				typeMap.insert(type->nameHash, type);
		}
		else if(TypeGenericClassProto *exact = getType<TypeGenericClassProto>(type))
		{
			if(!location || exact->definition->imported || exact->definition->pos.begin <= location->pos.begin)
				typeMap.insert(type->nameHash, type);
		}
		else
		{
			typeMap.insert(type->nameHash, type);
		}
	}

	for(unsigned i = 0, e = target->aliases.count; i < e; i++)
	{
		AliasData *alias = target->aliases.data[i];

		if(!location || alias->importModule != NULL || alias->source->pos.begin <= location->pos.begin)
			typeMap.insert(alias->nameHash, alias->type);
	}

	scope = target;
}

void ExpressionContext::SwitchToScopeAtPoint(SynBase *currLocation, ScopeData *target, SynBase *targetLocation)
{
	// Reach the same depth
	while(scope->scopeDepth > target->scopeDepth)
		PopScope(scope->type, NULL, true);

	// Reach the same parent
	ScopeData *curr = target;

	while(curr->scopeDepth > scope->scopeDepth)
		curr = curr->scope;

	while(scope->scope != curr->scope)
	{
		PopScope(scope->type, NULL, true);

		curr = curr->scope;
	}

	// We have common parent, but we are in different scopes, go to common parent
	if(scope != curr)
		PopScope(scope->type, NULL, true);

	// When the common parent is reached, remove it without ejecting namespace variables into the outer scope
	PopScope(scope->type, currLocation, true);

	// Now restore each namespace data up to the source location
	RestoreScopesAtPoint(target, targetLocation);
}

NamespaceData* ExpressionContext::GetCurrentNamespace()
{
	// Simply walk up the scopes and find the current one
	for(ScopeData *curr = scope; curr; curr = curr->scope)
	{
		if(NamespaceData *ns = curr->ownerNamespace)
			return ns;
	}

	return NULL;
}

FunctionData* ExpressionContext::GetCurrentFunction()
{
	// Walk up, but if we reach a type owner, stop - we're not in a context of a function
	for(ScopeData *curr = scope; curr; curr = curr->scope)
	{
		if(curr->ownerType)
			return NULL;

		if(FunctionData *function = curr->ownerFunction)
			return function;
	}

	return NULL;
}

TypeBase* ExpressionContext::GetCurrentType()
{
	// Simply walk up the scopes and find the current one
	for(ScopeData *curr = scope; curr; curr = curr->scope)
	{
		if(TypeBase *type = curr->ownerType)
			return type;
	}

	return NULL;
}

FunctionData* ExpressionContext::GetFunctionOwner(ScopeData *scopeData)
{
	// Temporary scopes have no owner
	if(scopeData->type == SCOPE_TEMPORARY)
		return NULL;

	// Walk up, but if we reach a type or namespace owner, stop - we're not in a context of a function
	for(ScopeData *curr = scopeData; curr; curr = curr->scope)
	{
		if(curr->ownerType)
			return NULL;

		if(curr->ownerNamespace)
			return NULL;

		if(FunctionData *function = curr->ownerFunction)
			return function;
	}

	return NULL;
}

ScopeData* ExpressionContext::NamespaceScopeFrom(ScopeData *scopeData)
{
	if(!scopeData || scopeData->ownerNamespace)
		return scopeData;

	return NamespaceScopeFrom(scopeData->scope);
}

ScopeData* ExpressionContext::GlobalScopeFrom(ScopeData *scopeData)
{
	if(!scopeData)
		return NULL;

	if(scopeData->type == SCOPE_TEMPORARY)
		return NULL;

	if(scopeData->ownerFunction)
		return NULL;

	if(scopeData->ownerType)
		return NULL;

	if(scopeData->scope)
		return GlobalScopeFrom(scopeData->scope);

	return scopeData;
}

unsigned ExpressionContext::GetGenericClassInstantiationDepth()
{
	unsigned depth = 0;

	for(ScopeData *curr = scope; curr; curr = curr->scope)
	{
		if(TypeClass *type = getType<TypeClass>(curr->ownerType))
		{
			if(!type->generics.empty())
				depth++;
		}
	}

	return depth;
}

bool ExpressionContext::IsGenericInstance(FunctionData *function)
{
	if(function->isGenericInstance)
		return true;

	if(function->proto != NULL)
		return true;

	if(function->scope->ownerType)
	{
		if(TypeClass *typeClass = getType<TypeClass>(function->scope->ownerType))
		{
			if(typeClass->proto)
				return true;
		}
	}

	return false;
}

void ExpressionContext::AddType(TypeBase *type)
{
	scope->types.push_back(type);

	types.push_back(type);
	typeMap.insert(type->nameHash, type);
}

void ExpressionContext::AddFunction(FunctionData *function)
{
	scope->functions.push_back(function);

	functions.push_back(function);

	// Don't add internal functions to named lookup
	if(*function->name.name.begin != '$')
		functionMap.insert(function->nameHash, function);

	while(VariableData **variable = variableMap.find(function->nameHash))
	{
		variableMap.remove(function->nameHash, *variable);

		scope->shadowedVariables.push_back(*variable);
	}
}

void ExpressionContext::AddVariable(VariableData *variable)
{
	scope->variables.push_back(variable);
	scope->allVariables.push_back(variable);

	variables.push_back(variable);
	variableMap.insert(variable->nameHash, variable);
}

void ExpressionContext::AddAlias(AliasData *alias)
{
	scope->aliases.push_back(alias);

	typeMap.insert(alias->nameHash, alias->type);
}

unsigned ExpressionContext::GetTypeIndex(TypeBase *type)
{
	unsigned index = ~0u;

	for(unsigned i = 0, e = types.count; i < e; i++)
	{
		if(types.data[i] == type)
		{
			index = i;
			break;
		}
	}

	assert(index != ~0u);

	return index;
}

unsigned ExpressionContext::GetFunctionIndex(FunctionData *data)
{
	unsigned index = ~0u;

	for(unsigned i = 0, e = functions.count; i < e; i++)
	{
		if(functions.data[i] == data)
		{
			index = i;
			break;
		}
	}

	assert(index != ~0u);

	return index;
}

void ExpressionContext::HideFunction(FunctionData *function)
{
	// Don't have to remove internal functions since they are never added to lookup
	if(*function->name.name.begin != '$')
		functionMap.remove(function->nameHash, function);

	SmallArray<FunctionData*, 2> &scopeFunctions = function->scope->functions;

	for(unsigned i = 0; i < scopeFunctions.size(); i++)
	{
		if(scopeFunctions[i] == function)
		{
			scopeFunctions[i] = scopeFunctions.back();
			scopeFunctions.pop_back();
		}
	}
}

bool ExpressionContext::IsGenericFunction(FunctionData *function)
{
	if(function->type->isGeneric)
		return true;

	if(function->scope->ownerType && function->scope->ownerType->isGeneric)
		return true;

	for(MatchData *curr = function->generics.head; curr; curr = curr->next)
	{
		if(curr->type->isGeneric)
			return true;
	}

	return false;
}

bool ExpressionContext::IsIntegerType(TypeBase* type)
{
	if(type == typeBool)
		return true;

	if(type == typeChar)
		return true;

	if(type == typeShort)
		return true;

	if(type == typeInt)
		return true;

	if(type == typeLong)
		return true;

	return false;
}

bool ExpressionContext::IsFloatingPointType(TypeBase* type)
{
	if(type == typeFloat)
		return true;

	if(type == typeDouble)
		return true;

	return false;
}

bool ExpressionContext::IsNumericType(TypeBase* type)
{
	return IsIntegerType(type) || IsFloatingPointType(type);
}

TypeBase* ExpressionContext::GetBinaryOpResultType(TypeBase* a, TypeBase* b)
{
	if(a == typeDouble || b == typeDouble)
		return typeDouble;

	if(a == typeFloat || b == typeFloat)
		return typeFloat;

	if(a == typeLong || b == typeLong)
		return typeLong;

	if(a == typeInt || b == typeInt)
		return typeInt;

	if(a == typeShort || b == typeShort)
		return typeShort;

	if(a == typeChar || b == typeChar)
		return typeChar;

	if(a == typeBool || b == typeBool)
		return typeBool;

	return NULL;
}

TypeError* ExpressionContext::GetErrorType()
{
	return new (get<TypeError>()) TypeError();
}

TypeRef* ExpressionContext::GetReferenceType(TypeBase* type)
{
	// Can't derive from pseudo types
	assert(!isType<TypeArgumentSet>(type) && !isType<TypeMemberSet>(type) && !isType<TypeFunctionSet>(type));

	// Can't create reference to auto this way
	assert(type != typeAuto);

	if(type->refType)
		return type->refType;

	// Create new type
	TypeRef* result = new (get<TypeRef>()) TypeRef(GetReferenceTypeName(*this, type), type);

	// Save it for future use
	type->refType = result;

	types.push_back(result);

	return result;
}

TypeArray* ExpressionContext::GetArrayType(TypeBase* type, long long length)
{
	// Can't have array of void
	assert(type != typeVoid);

	// Can't have array of auto
	assert(type != typeAuto);

	// Can't derive from pseudo types
	assert(!isType<TypeArgumentSet>(type) && !isType<TypeMemberSet>(type) && !isType<TypeFunctionSet>(type));

	if(TypeClass *typeClass = getType<TypeClass>(type))
		assert(typeClass->completed && !typeClass->hasFinalizer);

	for(TypeHandle *curr = type->arrayTypes.head; curr; curr = curr->next)
	{
		if(TypeArray *typeArray = getType<TypeArray>(curr->type))
		{
			if(typeArray->length == length)
				return typeArray;
		}
	}

	assert(type->size < 64 * 1024);

	// Create new type
	TypeArray* result = new (get<TypeArray>()) TypeArray(GetArrayTypeName(*this, type, length), type, length);

	result->alignment = type->alignment;

	unsigned maximumAlignment = result->alignment < 4 ? 4 : result->alignment;

	if(result->size % maximumAlignment != 0)
	{
		result->padding = maximumAlignment - (result->size % maximumAlignment);
		result->size += result->padding;
	}

	// Save it for future use
	type->arrayTypes.push_back(new (get<TypeHandle>()) TypeHandle(result));

	types.push_back(result);

	return result;
}

TypeUnsizedArray* ExpressionContext::GetUnsizedArrayType(TypeBase* type)
{
	// Can't have array of void
	assert(type != typeVoid);

	// Can't create array of auto types this way
	assert(type != typeAuto);

	// Can't derive from pseudo types
	assert(!isType<TypeArgumentSet>(type) && !isType<TypeMemberSet>(type) && !isType<TypeFunctionSet>(type));

	if(type->unsizedArrayType)
		return type->unsizedArrayType;

	assert(type->size < 64 * 1024);

	// Create new type
	TypeUnsizedArray* result = new (get<TypeUnsizedArray>()) TypeUnsizedArray(GetUnsizedArrayTypeName(*this, type), type);

	PushScope(result);

	result->typeScope = scope;

	result->members.push_back(new (get<VariableHandle>()) VariableHandle(new (get<VariableData>()) VariableData(allocator, NULL, scope, 4, typeInt, InplaceStr("size"), NULLC_PTR_SIZE, uniqueVariableId++)));
	result->members.tail->variable->isReadonly = true;

	result->alignment = 4;
	result->size = NULLC_PTR_SIZE + 4;

	PopScope(SCOPE_TYPE);

	// Save it for future use
	type->unsizedArrayType = result;

	types.push_back(result);

	return result;
}

TypeFunction* ExpressionContext::GetFunctionType(SynBase *source, TypeBase* returnType, IntrusiveList<TypeHandle> arguments)
{
	// Can't derive from pseudo types
	assert(!isType<TypeArgumentSet>(returnType) && !isType<TypeMemberSet>(returnType) && !isType<TypeFunctionSet>(returnType));

	for(TypeHandle *curr = arguments.head; curr; curr = curr->next)
	{
		assert(!isType<TypeArgumentSet>(curr->type) && !isType<TypeMemberSet>(curr->type) && !isType<TypeFunctionSet>(curr->type));

		// Can't have auto as argument
		assert(curr->type != typeAuto);
	}

	for(unsigned i = 0, e = functionTypes.count; i < e; i++)
	{
		if(TypeFunction *type = functionTypes.data[i])
		{
			if(type->returnType != returnType)
				continue;

			TypeHandle *leftArg = type->arguments.head;
			TypeHandle *rightArg = arguments.head;

			while(leftArg && rightArg && leftArg->type == rightArg->type)
			{
				leftArg = leftArg->next;
				rightArg = rightArg->next;
			}

			if(leftArg != rightArg)
				continue;

			return type;
		}
	}

	// Create new type
	TypeFunction* result = new (get<TypeFunction>()) TypeFunction(GetFunctionTypeName(*this, returnType, arguments), returnType, arguments);

	if(result->name.length() > NULLC_MAX_TYPE_NAME_LENGTH)
		Stop(source->pos, "ERROR: generated function type name exceeds maximum type length '%d'", NULLC_MAX_TYPE_NAME_LENGTH);

	result->alignment = 4;

	functionTypes.push_back(result);
	types.push_back(result);

	return result;
}

TypeFunction* ExpressionContext::GetFunctionType(SynBase *source, TypeBase* returnType, ArrayView<ArgumentData> arguments)
{
	IntrusiveList<TypeHandle> argumentTypes;

	for(unsigned i = 0; i < arguments.size(); i++)
		argumentTypes.push_back(new (get<TypeHandle>()) TypeHandle(arguments[i].type));

	return GetFunctionType(source, returnType, argumentTypes);
}

TypeFunctionSet* ExpressionContext::GetFunctionSetType(IntrusiveList<TypeHandle> setTypes)
{
	for(unsigned i = 0, e = functionSetTypes.count; i < e; i++)
	{
		if(TypeFunctionSet *type = functionSetTypes.data[i])
		{
			TypeHandle *leftArg = type->types.head;
			TypeHandle *rightArg = setTypes.head;

			while(leftArg && rightArg && leftArg->type == rightArg->type)
			{
				leftArg = leftArg->next;
				rightArg = rightArg->next;
			}

			if(leftArg != rightArg)
				continue;

			return type;
		}
	}

	// Create new type
	TypeFunctionSet* result = new (get<TypeFunctionSet>()) TypeFunctionSet(GetFunctionSetTypeName(*this, setTypes), setTypes);

	functionSetTypes.push_back(result);

	// This type is not added to export list

	return result;
}

TypeGenericAlias* ExpressionContext::GetGenericAliasType(InplaceStr baseName)
{
	for(unsigned i = 0, e = genericAliasTypes.count; i < e; i++)
	{
		if(TypeGenericAlias *type = genericAliasTypes.data[i])
		{
			if(type->baseName == baseName)
				return type;
		}
	}

	// Create new type
	TypeGenericAlias* result = new (get<TypeGenericAlias>()) TypeGenericAlias(GetGenericAliasTypeName(*this, baseName), baseName);

	genericAliasTypes.push_back(result);
	types.push_back(result);

	return result;
}

TypeGenericClass* ExpressionContext::GetGenericClassType(SynBase *source, TypeGenericClassProto *proto, IntrusiveList<TypeHandle> generics)
{
	for(unsigned i = 0, e = genericClassTypes.count; i < e; i++)
	{
		if(TypeGenericClass *type = genericClassTypes.data[i])
		{
			if(type->proto != proto)
				continue;

			TypeHandle *leftArg = type->generics.head;
			TypeHandle *rightArg = generics.head;

			while(leftArg && rightArg && leftArg->type == rightArg->type)
			{
				leftArg = leftArg->next;
				rightArg = rightArg->next;
			}

			if(leftArg != rightArg)
				continue;

			return type;
		}
	}

	// Create new type
	TypeGenericClass *result = new (get<TypeGenericClass>()) TypeGenericClass(GetGenericClassTypeName(*this, proto, generics), proto, generics);

	if(result->name.length() > NULLC_MAX_TYPE_NAME_LENGTH)
		Stop(source->pos, "ERROR: generated generic type name exceeds maximum type length '%d'", NULLC_MAX_TYPE_NAME_LENGTH);

	genericClassTypes.push_back(result);
	types.push_back(result);

	return result;
}

ExprBase* AnalyzeNumber(ExpressionContext &ctx, SynNumber *syntax);
ExprBase* AnalyzeExpression(ExpressionContext &ctx, SynBase *syntax);
ExprBase* AnalyzeStatement(ExpressionContext &ctx, SynBase *syntax);
ExprBlock* AnalyzeBlock(ExpressionContext &ctx, SynBlock *syntax, bool createScope);
ExprAliasDefinition* AnalyzeTypedef(ExpressionContext &ctx, SynTypedef *syntax);
ExprBase* AnalyzeClassDefinition(ExpressionContext &ctx, SynClassDefinition *syntax, TypeGenericClassProto *proto, IntrusiveList<TypeHandle> generics);
void AnalyzeClassElements(ExpressionContext &ctx, ExprClassDefinition *classDefinition, SynClassElements *syntax);
ExprBase* AnalyzeFunctionDefinition(ExpressionContext &ctx, SynFunctionDefinition *syntax, TypeFunction *instance, TypeBase *instanceParent, IntrusiveList<MatchData> matches, bool createAccess, bool isLocal, bool checkParent);
ExprBase* AnalyzeShortFunctionDefinition(ExpressionContext &ctx, SynShortFunctionDefinition *syntax, TypeFunction *argumentType);
ExprBase* AnalyzeShortFunctionDefinition(ExpressionContext &ctx, SynShortFunctionDefinition *syntax, TypeBase *type, ArrayView<ArgumentData> arguments, IntrusiveList<MatchData> aliases);

ExprBase* CreateTypeidMemberAccess(ExpressionContext &ctx, SynBase *source, TypeBase *type, InplaceStr member);

ExprBase* CreateBinaryOp(ExpressionContext &ctx, SynBase *source, SynBinaryOpType op, ExprBase *lhs, ExprBase *rhs);

ExprBase* CreateVariableAccess(ExpressionContext &ctx, SynBase *source, VariableData *variable, bool handleReference);
ExprBase* CreateVariableAccess(ExpressionContext &ctx, SynBase *source, IntrusiveList<SynIdentifier> path, InplaceStr name, bool allowInternal);

ExprBase* CreateGetAddress(ExpressionContext &ctx, SynBase *source, ExprBase *value);

ExprBase* CreateMemberAccess(ExpressionContext &ctx, SynBase *source, ExprBase *value, InplaceStr name, bool allowFailure);

ExprBase* CreateAssignment(ExpressionContext &ctx, SynBase *source, ExprBase *lhs, ExprBase *rhs);

ExprBase* AssertResolvableTypeLiteral(ExpressionContext &ctx, SynBase *source, ExprBase *expr);
ExprBase* AssertValueExpression(ExpressionContext &ctx, SynBase *source, ExprBase *expr);

InplaceStr GetTypeConstructorName(TypeClass *classType);
bool GetTypeConstructorFunctions(ExpressionContext &ctx, TypeBase *type, bool noArguments, SmallArray<FunctionData*, 32> &functions);
ExprBase* CreateConstructorAccess(ExpressionContext &ctx, SynBase *source, ArrayView<FunctionData*> functions, ExprBase *context);
ExprBase* CreateConstructorAccess(ExpressionContext &ctx, SynBase *source, TypeBase *type, bool noArguments, ExprBase *context);
bool HasDefautConstructor(ExpressionContext &ctx, SynBase *source, TypeBase *type);
ExprBase* CreateDefaultConstructorCall(ExpressionContext &ctx, SynBase *source, TypeBase *type, ExprBase *pointer);
void CreateDefaultConstructorCode(ExpressionContext &ctx, SynBase *source, TypeClass *classType, IntrusiveList<ExprBase> &expressions);

InplaceStr GetTemporaryFunctionName(ExpressionContext &ctx);
InplaceStr GetDefaultArgumentWrapperFunctionName(ExpressionContext &ctx, FunctionData *function, InplaceStr argumentName);
TypeBase* CreateFunctionContextType(ExpressionContext &ctx, SynBase *source, InplaceStr functionName);
ExprVariableDefinition* CreateFunctionContextArgument(ExpressionContext &ctx, SynBase *source, FunctionData *function);
ExprVariableDefinition* CreateFunctionContextVariable(ExpressionContext &ctx, SynBase *source, FunctionData *function, FunctionData *prototype);
ExprBase* CreateFunctionContextAccess(ExpressionContext &ctx, SynBase *source, FunctionData *function);
ExprBase* CreateFunctionAccess(ExpressionContext &ctx, SynBase *source, HashMap<FunctionData*>::Node *function, ExprBase *context);
ExprBase* CreateFunctionCoroutineStateUpdate(ExpressionContext &ctx, SynBase *source, FunctionData *function, int state);

TypeBase* MatchGenericType(ExpressionContext &ctx, SynBase *source, TypeBase *matchType, TypeBase *argType, IntrusiveList<MatchData> &aliases, bool strict);
TypeBase* ResolveGenericTypeAliases(ExpressionContext &ctx, SynBase *source, TypeBase *type, IntrusiveList<MatchData> aliases);

FunctionValue SelectBestFunction(ExpressionContext &ctx, SynBase *source, ArrayView<FunctionValue> functions, IntrusiveList<TypeHandle> generics, ArrayView<ArgumentData> arguments, SmallArray<unsigned, 32> &ratings);
FunctionValue CreateGenericFunctionInstance(ExpressionContext &ctx, SynBase *source, FunctionValue proto, IntrusiveList<TypeHandle> generics, ArrayView<ArgumentData> arguments, bool standalone);
void GetNodeFunctions(ExpressionContext &ctx, SynBase *source, ExprBase *function, SmallArray<FunctionValue, 32> &functions);
void ReportOnFunctionSelectError(ExpressionContext &ctx, SynBase *source, char* errorBuf, unsigned errorBufSize, ArrayView<FunctionValue> functions);
void ReportOnFunctionSelectError(ExpressionContext &ctx, SynBase *source, char* errorBuf, unsigned errorBufSize, InplaceStr functionName, ArrayView<FunctionValue> functions, ArrayView<ArgumentData> arguments, ArrayView<unsigned> ratings, unsigned bestRating, bool showInstanceInfo);
ExprBase* CreateFunctionCall0(ExpressionContext &ctx, SynBase *source, InplaceStr name, bool allowFailure, bool allowInternal);
ExprBase* CreateFunctionCall1(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, bool allowFailure, bool allowInternal);
ExprBase* CreateFunctionCall2(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, ExprBase *arg1, bool allowFailure, bool allowInternal);
ExprBase* CreateFunctionCall3(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, ExprBase *arg1, ExprBase *arg2, bool allowFailure, bool allowInternal);
ExprBase* CreateFunctionCall4(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, ExprBase *arg1, ExprBase *arg2, ExprBase *arg3, bool allowFailure, bool allowInternal);
ExprBase* CreateFunctionCallByName(ExpressionContext &ctx, SynBase *source, InplaceStr name, ArrayView<ArgumentData> arguments, bool allowFailure, bool allowInternal);
ExprBase* CreateFunctionCall(ExpressionContext &ctx, SynBase *source, ExprBase *value, ArrayView<ArgumentData> arguments, bool allowFailure);
ExprBase* CreateFunctionCall(ExpressionContext &ctx, SynBase *source, ExprBase *value, IntrusiveList<TypeHandle> generics, SynCallArgument *argumentHead, bool allowFailure);
ExprBase* CreateFunctionCallOverloaded(ExpressionContext &ctx, SynBase *source, ExprBase *value, ArrayView<FunctionValue> functions, IntrusiveList<TypeHandle> generics, SynCallArgument *argumentHead, bool allowFailure);
ExprBase* CreateFunctionCallFinal(ExpressionContext &ctx, SynBase *source, ExprBase *value, ArrayView<FunctionValue> functions, IntrusiveList<TypeHandle> generics, ArrayView<ArgumentData> arguments, bool allowFailure);
ExprBase* CreateObjectAllocation(ExpressionContext &ctx, SynBase *source, TypeBase *type);
ExprBase* CreateArrayAllocation(ExpressionContext &ctx, SynBase *source, TypeBase *type, ExprBase *count);

bool RestoreParentTypeScope(ExpressionContext &ctx, SynBase *source, TypeBase *parentType);
ExprBase* CreateFunctionDefinition(ExpressionContext &ctx, SynBase *source, bool prototype, bool coroutine, TypeBase *parentType, bool accessor, TypeBase *returnType, bool isOperator, SynIdentifier name, IntrusiveList<SynIdentifier> aliases, IntrusiveList<SynFunctionArgument> arguments, IntrusiveList<SynBase> expressions, TypeFunction *instance, IntrusiveList<MatchData> matches);

FunctionValue GetFunctionForType(ExpressionContext &ctx, SynBase *source, ExprBase *value, TypeFunction *type)
{
	// Collect a set of available functions
	SmallArray<FunctionValue, 32> functions(ctx.allocator);

	GetNodeFunctions(ctx, source, value, functions);

	if(!functions.empty())
	{
		FunctionValue bestMatch;
		TypeFunction *bestMatchTarget = NULL;

		FunctionValue bestGenericMatch;
		TypeFunction *bestGenericMatchTarget = NULL;

		for(unsigned i = 0; i < functions.size(); i++)
		{
			TypeFunction *functionType = functions[i].function->type;

			if(type->arguments.size() != functionType->arguments.size())
				continue;

			if(type->isGeneric)
			{
				IntrusiveList<MatchData> aliases;

				TypeBase *returnType = MatchGenericType(ctx, source, type->returnType, functionType->returnType, aliases, true);
				IntrusiveList<TypeHandle> arguments;

				for(TypeHandle *lhs = type->arguments.head, *rhs = functionType->arguments.head; lhs && rhs; lhs = lhs->next, rhs = rhs->next)
				{
					TypeBase *match = MatchGenericType(ctx, source, lhs->type, rhs->type, aliases, true);

					if(match && !match->isGeneric)
						arguments.push_back(new (ctx.get<TypeHandle>()) TypeHandle(match));
				}

				if(returnType && arguments.size() == type->arguments.size())
				{
					if(bestGenericMatch)
						return FunctionValue();

					bestGenericMatch = functions[i];
					bestGenericMatchTarget = ctx.GetFunctionType(source, returnType, arguments);
				}
			}
			else if(functionType->isGeneric)
			{
				unsigned matches = 0;

				IntrusiveList<MatchData> aliases;

				for(TypeHandle *lhs = functionType->arguments.head, *rhs = type->arguments.head; lhs && rhs; lhs = lhs->next, rhs = rhs->next)
				{
					TypeBase *match = MatchGenericType(ctx, source, lhs->type, rhs->type, aliases, true);

					if(match && !match->isGeneric)
						matches++;
				}

				if(functionType->returnType == ctx.typeAuto || functionType->returnType == type->returnType)
					matches++;

				if(matches == type->arguments.size() + 1)
				{
					if(bestGenericMatch)
						return FunctionValue();

					bestGenericMatch = functions[i];
					bestGenericMatchTarget = type;
				}
			}
			else if(functionType == type)
			{
				if(bestMatch)
					return FunctionValue();

				bestMatch = functions[i];
				bestMatchTarget = type;
			}
		}

		FunctionValue bestOverload = bestMatch ? bestMatch : bestGenericMatch;
		TypeFunction *bestTarget = bestMatch ? bestMatchTarget : bestGenericMatchTarget;

		if(bestOverload)
		{
			SmallArray<ArgumentData, 32> arguments(ctx.allocator);

			for(TypeHandle *curr = bestTarget->arguments.head; curr; curr = curr->next)
				arguments.push_back(ArgumentData(source, false, InplaceStr(), curr->type, NULL));

			FunctionData *function = bestOverload.function;

			if(ctx.IsGenericFunction(function))
				bestOverload = CreateGenericFunctionInstance(ctx, source, bestOverload, IntrusiveList<TypeHandle>(), arguments, false);

			if(bestOverload)
			{
				if(bestTarget->returnType == ctx.typeAuto)
					bestTarget = ctx.GetFunctionType(source, bestOverload.function->type->returnType, bestTarget->arguments);

				if(bestOverload.function->type == bestTarget)
					return bestOverload;
			}
		}
	}

	return FunctionValue();
}

ExprBase* CreateSequence(ExpressionContext &ctx, SynBase *source, ExprBase *first, ExprBase *second)
{
	IntrusiveList<ExprBase> expressions;

	expressions.push_back(first);
	expressions.push_back(second);

	return new (ctx.get<ExprSequence>()) ExprSequence(source, second->type, expressions);
}

ExprBase* CreateSequence(ExpressionContext &ctx, SynBase *source, ExprBase *first, ExprBase *second, ExprBase *third)
{
	IntrusiveList<ExprBase> expressions;

	expressions.push_back(first);
	expressions.push_back(second);
	expressions.push_back(third);

	return new (ctx.get<ExprSequence>()) ExprSequence(source, third->type, expressions);
}

ExprBase* CreateLiteralCopy(ExpressionContext &ctx, SynBase *source, ExprBase *value)
{
	if(ExprBoolLiteral *node = getType<ExprBoolLiteral>(value))
		return new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(node->source, node->type, node->value);

	if(ExprCharacterLiteral *node = getType<ExprCharacterLiteral>(value))
		return new (ctx.get<ExprCharacterLiteral>()) ExprCharacterLiteral(node->source, node->type, node->value);

	if(ExprIntegerLiteral *node = getType<ExprIntegerLiteral>(value))
		return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(node->source, node->type, node->value);

	if(ExprRationalLiteral *node = getType<ExprRationalLiteral>(value))
		return new (ctx.get<ExprRationalLiteral>()) ExprRationalLiteral(node->source, node->type, node->value);

	Stop(ctx, source->pos, "ERROR: unknown literal type");

	return NULL;
}

ExprBase* CreateFunctionPointer(ExpressionContext &ctx, SynBase *source, ExprFunctionDefinition *definition, bool hideFunction)
{
	if(hideFunction)
	{
		ctx.HideFunction(definition->function);

		definition->function->isHidden = true;
	}

	IntrusiveList<ExprBase> expressions;

	expressions.push_back(definition);

	if(definition->contextVariable)
		expressions.push_back(definition->contextVariable);

	expressions.push_back(new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(source, definition->function->type, definition->function, CreateFunctionContextAccess(ctx, source, definition->function)));

	return new (ctx.get<ExprSequence>()) ExprSequence(source, definition->function->type, expressions);
}

ExprBase* CreateCast(ExpressionContext &ctx, SynBase *source, ExprBase *value, TypeBase *type, bool isFunctionArgument)
{
	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(source, ctx.GetErrorType());

	// When function is used as value, hide its visibility immediately after use
	if(ExprFunctionDefinition *definition = getType<ExprFunctionDefinition>(value))
		return CreateFunctionPointer(ctx, source, definition, true);

	if(value->type == type)
	{
		AssertValueExpression(ctx, source, value);

		return value;
	}

	if(TypeFunction *target = getType<TypeFunction>(type))
	{
		if(FunctionValue function = GetFunctionForType(ctx, source, value, target))
		{
			ExprBase *access = new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(function.source, type, function.function, function.context);

			if(isType<ExprFunctionDefinition>(value) || isType<ExprGenericFunctionPrototype>(value))
				return CreateSequence(ctx, source, value, access);

			return access;
		}

		if(value->type->isGeneric)
			Stop(ctx, source->pos, "ERROR: can't resolve generic type '%.*s' instance for '%.*s'", FMT_ISTR(value->type->name), FMT_ISTR(type->name));
	}

	AssertValueExpression(ctx, source, value);

	if(ctx.IsNumericType(value->type) && ctx.IsNumericType(type))
		return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_NUMERICAL);

	if(type == ctx.typeBool)
	{
		if(isType<TypeRef>(value->type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_PTR_TO_BOOL);

		if(isType<TypeUnsizedArray>(value->type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_UNSIZED_TO_BOOL);

		if(isType<TypeFunction>(value->type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_FUNCTION_TO_BOOL);
	}

	if(value->type == ctx.typeNullPtr)
	{
		// nullptr to type ref conversion
		if(isType<TypeRef>(type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_NULL_TO_PTR);

		// nullptr to auto ref conversion
		if(type == ctx.typeAutoRef)
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_NULL_TO_AUTO_PTR);

		// nullptr to type[] conversion
		if(isType<TypeUnsizedArray>(type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_NULL_TO_UNSIZED);

		// nullptr to auto[] conversion
		if(type == ctx.typeAutoArray)
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_NULL_TO_AUTO_ARRAY);

		// nullptr to function type conversion
		if(isType<TypeFunction>(type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_NULL_TO_FUNCTION);
	}

	if(TypeUnsizedArray *target = getType<TypeUnsizedArray>(type))
	{
		// type[N] to type[] conversion
		if(TypeArray *valueType = getType<TypeArray>(value->type))
		{
			if(target->subType == valueType->subType)
			{
				if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
				{
					ExprBase *address = new (ctx.get<ExprGetAddress>()) ExprGetAddress(source, ctx.GetReferenceType(value->type), node->variable);

					return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, address, EXPR_CAST_ARRAY_PTR_TO_UNSIZED);
				}
				else if(ExprDereference *node = getType<ExprDereference>(value))
				{
					return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, node->value, EXPR_CAST_ARRAY_PTR_TO_UNSIZED);
				}

				// Allocate storage in heap and copy literal data into it
				VariableData *storage = AllocateTemporary(ctx, source, ctx.GetReferenceType(valueType));

				ExprBase *alloc = CreateObjectAllocation(ctx, source, valueType);

				ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(value->source, ctx.typeVoid, storage, CreateAssignment(ctx, source, CreateVariableAccess(ctx, source, storage, false), alloc));

				ExprBase *assignment = CreateAssignment(ctx, source, new (ctx.get<ExprDereference>()) ExprDereference(source, valueType, CreateVariableAccess(ctx, source, storage, false)), value);

				ExprBase *result = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, CreateVariableAccess(ctx, source, storage, false), EXPR_CAST_ARRAY_PTR_TO_UNSIZED);

				return CreateSequence(ctx, source, definition, assignment, result);
			}
		}
	}

	if(TypeRef *target = getType<TypeRef>(type))
	{
		if(TypeRef *valueType = getType<TypeRef>(value->type))
		{
			// type[N] ref to type[] ref conversion
			if(isType<TypeUnsizedArray>(target->subType) && isType<TypeArray>(valueType->subType))
			{
				TypeUnsizedArray *targetSub = getType<TypeUnsizedArray>(target->subType);
				TypeArray *sourceSub = getType<TypeArray>(valueType->subType);

				if(targetSub->subType == sourceSub->subType)
				{
					VariableData *storage = AllocateTemporary(ctx, source, targetSub);

					ExprBase *assignment = CreateAssignment(ctx, source, CreateVariableAccess(ctx, source, storage, false), new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, targetSub, value, EXPR_CAST_ARRAY_PTR_TO_UNSIZED));

					ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, storage, assignment);

					ExprBase *result = CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, storage, false));

					return CreateSequence(ctx, source, definition, result);
				}
			}

			if(isType<TypeClass>(target->subType) && isType<TypeClass>(valueType->subType))
			{
				TypeClass *targetClass = getType<TypeClass>(target->subType);
				TypeClass *valueClass = getType<TypeClass>(valueType->subType);

				if(IsDerivedFrom(valueClass, targetClass))
					return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_REINTERPRET);

				if(IsDerivedFrom(targetClass, valueClass))
				{
					ExprBase *untyped = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.GetReferenceType(ctx.typeVoid), value, EXPR_CAST_REINTERPRET);
					ExprBase *typeID = new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, targetClass);

					ExprBase *checked = CreateFunctionCall2(ctx, source, InplaceStr("assert_derived_from_base"), untyped, typeID, false, false);

					return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, checked, EXPR_CAST_REINTERPRET);
				}
			}
		}
		else if(value->type == ctx.typeAutoRef)
		{
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_AUTO_PTR_TO_PTR);
		}
		else if(isFunctionArgument)
		{
			// type to type ref conversion
			if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
			{
				ExprBase *address = new (ctx.get<ExprGetAddress>()) ExprGetAddress(source, ctx.GetReferenceType(value->type), node->variable);

				return address;
			}
			else if(ExprDereference *node = getType<ExprDereference>(value))
			{
				return node->value;
			}
			else if(target->subType == value->type)
			{
				VariableData *storage = AllocateTemporary(ctx, source, target->subType);

				ExprBase *assignment = new (ctx.get<ExprAssignment>()) ExprAssignment(source, storage->type, CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, storage, false)), value);

				ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, storage, assignment);

				ExprBase *result = CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, storage, false));

				return CreateSequence(ctx, source, definition, result);
			}
		}
	}

	if(type == ctx.typeAutoRef)
	{
		// type ref to auto ref conversion
		if(isType<TypeRef>(value->type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_PTR_TO_AUTO_PTR);

		if(isFunctionArgument)
		{
			// type to auto ref conversion
			if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
			{
				ExprBase *address = new (ctx.get<ExprGetAddress>()) ExprGetAddress(source, ctx.GetReferenceType(value->type), node->variable);

				return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, address, EXPR_CAST_PTR_TO_AUTO_PTR);
			}
			else if(ExprDereference *node = getType<ExprDereference>(value))
			{
				return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, node->value, EXPR_CAST_PTR_TO_AUTO_PTR);
			}

			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, CreateCast(ctx, source, value, ctx.GetReferenceType(value->type), true), EXPR_CAST_PTR_TO_AUTO_PTR);
		}
		else
		{
			// type to auto ref conversion (boxing)
			return CreateFunctionCall1(ctx, source, InplaceStr("duplicate"), value, false, false);
		}
	}

	if(type == ctx.typeAutoArray)
	{
		// type[] to auto[] conversion
		if(isType<TypeUnsizedArray>(value->type))
			return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, type, value, EXPR_CAST_UNSIZED_TO_AUTO_ARRAY);
		
		if(TypeArray *valueType = getType<TypeArray>(value->type))
		{
			ExprBase *unsized = CreateCast(ctx, source, value, ctx.GetUnsizedArrayType(valueType->subType), false);

			return CreateCast(ctx, source, unsized, type, false);
		}
	}

	if(value->type == ctx.typeAutoRef)
	{
		// auto ref to type (unboxing)
		if(!isType<TypeRef>(type))
		{
			ExprBase *ptr = CreateCast(ctx, source, value, ctx.GetReferenceType(type), false);

			return new (ctx.get<ExprDereference>()) ExprDereference(source, type, ptr);
		}
	}

	if(TypeClass *target = getType<TypeClass>(type))
	{
		if(IsDerivedFrom(getType<TypeClass>(value->type), target))
		{
			VariableData *storage = AllocateTemporary(ctx, source, value->type);

			ExprBase *assignment = new (ctx.get<ExprAssignment>()) ExprAssignment(source, storage->type, CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, storage, false)), value);

			ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, storage, assignment);

			ExprBase *result = new (ctx.get<ExprDereference>()) ExprDereference(source, type, new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.GetReferenceType(type), CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, storage, false)), EXPR_CAST_REINTERPRET));

			return CreateSequence(ctx, source, definition, result);
		}
	}

	return ReportExpected(ctx, source, type, "ERROR: cannot convert '%.*s' to '%.*s'", FMT_ISTR(value->type->name), FMT_ISTR(type->name));
}

ExprBase* CreateConditionCast(ExpressionContext &ctx, SynBase *source, ExprBase *value)
{
	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(source, ctx.GetErrorType());

	if(!ctx.IsIntegerType(value->type) && !value->type->isGeneric)
	{
		// TODO: function overload

		if(ctx.IsFloatingPointType(value->type))
			return CreateCast(ctx, source, value, ctx.typeBool, false);

		if(isType<TypeRef>(value->type))
			return CreateCast(ctx, source, value, ctx.typeBool, false);

		if(isType<TypeUnsizedArray>(value->type))
			return CreateCast(ctx, source, value, ctx.typeBool, false);

		if(isType<TypeFunction>(value->type))
			return CreateCast(ctx, source, value, ctx.typeBool, false);

		if(value->type == ctx.typeAutoRef)
		{
			ExprBase *nullPtr = new (ctx.get<ExprNullptrLiteral>()) ExprNullptrLiteral(value->source, ctx.typeNullPtr);

			return CreateBinaryOp(ctx, source, SYN_BINARY_OP_NOT_EQUAL, value, nullPtr);
		}
		else
		{
			if(ExprBase *call = CreateFunctionCall1(ctx, source, InplaceStr("bool"), value, true, false))
				return call;

			return ReportExpected(ctx, source, ctx.typeBool, "ERROR: condition type cannot be '%.*s' and function for conversion to bool is undefined", FMT_ISTR(value->type->name));
		}
	}

	AssertValueExpression(ctx, source, value);

	if(value->type == ctx.typeLong)
		value = CreateCast(ctx, source, value, ctx.typeBool, false);

	return value;
}

ExprBase* CreateAssignment(ExpressionContext &ctx, SynBase *source, ExprBase *lhs, ExprBase *rhs)
{
	if(isType<ExprUnboxing>(lhs))
	{
		lhs = CreateCast(ctx, source, lhs, ctx.GetReferenceType(rhs->type), false);
		lhs = new (ctx.get<ExprDereference>()) ExprDereference(source, rhs->type, lhs);
	}

	ExprBase* wrapped = lhs;

	if(ExprVariableAccess *node = getType<ExprVariableAccess>(lhs))
	{
		wrapped = new (ctx.get<ExprGetAddress>()) ExprGetAddress(lhs->source, ctx.GetReferenceType(lhs->type), node->variable);
	}
	else if(ExprDereference *node = getType<ExprDereference>(lhs))
	{
		wrapped = node->value;
	}
	else if(ExprFunctionCall *node = getType<ExprFunctionCall>(lhs))
	{
		// Try to transform 'get' accessor to 'set'
		if(ExprFunctionAccess *access = getType<ExprFunctionAccess>(node->function))
		{
			if(access->function->accessor)
			{
				SmallArray<ArgumentData, 32> arguments(ctx.allocator);
				arguments.push_back(ArgumentData(rhs->source, false, InplaceStr(), rhs->type, rhs));

				if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(access->function->nameHash))
				{
					ExprBase *overloads = CreateFunctionAccess(ctx, source, function, access->context);

					if(ExprBase *call = CreateFunctionCall(ctx, source, overloads, arguments, true))
						return call;
				}

				if(FunctionData *proto = access->function->proto)
				{
					if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(proto->nameHash))
					{
						ExprBase *overloads = CreateFunctionAccess(ctx, source, function, access->context);

						if(ExprBase *call = CreateFunctionCall(ctx, source, overloads, arguments, true))
							return call;
					}
				}
			}
		}

		if(TypeRef *refType = getType<TypeRef>(lhs->type))
			lhs = new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, lhs);
	}
	else if(TypeRef *refType = getType<TypeRef>(lhs->type))
	{
		lhs = new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, lhs);
	}

	if(!isType<TypeRef>(wrapped->type))
		return ReportExpected(ctx, source, lhs->type, "ERROR: cannot change immutable value of type %.*s", FMT_ISTR(lhs->type->name));

	if(rhs->type == ctx.typeVoid)
		return ReportExpected(ctx, source, lhs->type, "ERROR: cannot convert from void to %.*s", FMT_ISTR(lhs->type->name));

	if(lhs->type == ctx.typeVoid)
		return ReportExpected(ctx, source, lhs->type, "ERROR: cannot convert from %.*s to void", FMT_ISTR(rhs->type->name));

	if(ExprBase *result = CreateFunctionCall2(ctx, source, InplaceStr("="), wrapped, rhs, true, false))
		return result;

	if(ExprBase *result = CreateFunctionCall2(ctx, source, InplaceStr("default_assign$_"), wrapped, rhs, true, false))
		return result;

	if((isType<TypeArray>(lhs->type) || isType<TypeUnsizedArray>(lhs->type)) && rhs->type == ctx.typeAutoArray)
		return CreateFunctionCall2(ctx, source, InplaceStr("__aaassignrev"), wrapped, rhs, false, true);

	rhs = CreateCast(ctx, source, rhs, lhs->type, false);

	return new (ctx.get<ExprAssignment>()) ExprAssignment(source, lhs->type, wrapped, rhs);
}

ExprBase* GetFunctionUpvalue(ExpressionContext &ctx, SynBase *source, VariableData *target)
{
	InplaceStr upvalueName = GetFunctionVariableUpvalueName(ctx, target);

	if(VariableData **variable = ctx.variableMap.find(upvalueName.hash()))
	{
		return new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, (*variable)->type, *variable);
	}

	TypeBase *type = ctx.GetReferenceType(ctx.typeVoid);

	unsigned offset = AllocateGlobalVariable(ctx, source, type->alignment, type->size);
	VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.globalScope, type->alignment, type, upvalueName, offset, ctx.uniqueVariableId++);

	ctx.globalScope->variables.push_back(variable);
	ctx.globalScope->allVariables.push_back(variable);

	ctx.variables.push_back(variable);
	ctx.variableMap.insert(variable->nameHash, variable);

	ctx.upvalues.push_back(variable);

	return new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, variable->type, variable);
}

ExprBase* CreateUpvalueClose(ExpressionContext &ctx, SynBase *source, VariableData *variable)
{
	ExprBase *upvalueAddress = CreateGetAddress(ctx, source, GetFunctionUpvalue(ctx, source, variable));

	ExprBase *variableAddress = CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, variable, false));

	variableAddress = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.GetReferenceType(ctx.typeVoid), variableAddress, EXPR_CAST_REINTERPRET);

	// Two pointers before data
	unsigned offset = NULLC_PTR_SIZE + NULLC_PTR_SIZE;

	offset += GetAlignmentOffset(offset, variable->type->alignment);

	ExprBase *copyOffset = new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, offset);

	ExprBase *copySize = new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, variable->type->size);

	return CreateFunctionCall4(ctx, source, InplaceStr("__closeUpvalue"), upvalueAddress, variableAddress, copyOffset, copySize, false, true);
}

ExprBase* CreateFunctionUpvalueClose(ExpressionContext &ctx, SynBase *source, FunctionData *onwerFunction, ScopeData *fromScope)
{
	if(!onwerFunction)
		return NULL;

	ExprSequence *holder = new (ctx.get<ExprSequence>()) ExprSequence(source, ctx.typeVoid, IntrusiveList<ExprBase>());

	onwerFunction->closeUpvalues.push_back(new (ctx.get<CloseUpvaluesData>()) CloseUpvaluesData(holder, CLOSE_UPVALUES_FUNCTION, source, fromScope, 0));

	return holder;
}

ExprBase* CreateBlockUpvalueClose(ExpressionContext &ctx, SynBase *source, FunctionData *onwerFunction, ScopeData *scope)
{
	if(!onwerFunction)
		return NULL;

	ExprSequence *holder = new (ctx.get<ExprSequence>()) ExprSequence(source, ctx.typeVoid, IntrusiveList<ExprBase>());

	onwerFunction->closeUpvalues.push_back(new (ctx.get<CloseUpvaluesData>()) CloseUpvaluesData(holder, CLOSE_UPVALUES_BLOCK, source, scope, 0));

	return holder;
}

ExprBase* CreateBreakUpvalueClose(ExpressionContext &ctx, SynBase *source, FunctionData *onwerFunction, ScopeData *fromScope, unsigned depth)
{
	if(!onwerFunction)
		return NULL;

	ExprSequence *holder = new (ctx.get<ExprSequence>()) ExprSequence(source, ctx.typeVoid, IntrusiveList<ExprBase>());

	onwerFunction->closeUpvalues.push_back(new (ctx.get<CloseUpvaluesData>()) CloseUpvaluesData(holder, CLOSE_UPVALUES_BREAK, source, fromScope, depth));

	return holder;
}

ExprBase* CreateContinueUpvalueClose(ExpressionContext &ctx, SynBase *source, FunctionData *onwerFunction, ScopeData *fromScope, unsigned depth)
{
	if(!onwerFunction)
		return NULL;

	ExprSequence *holder = new (ctx.get<ExprSequence>()) ExprSequence(source, ctx.typeVoid, IntrusiveList<ExprBase>());

	onwerFunction->closeUpvalues.push_back(new (ctx.get<CloseUpvaluesData>()) CloseUpvaluesData(holder, CLOSE_UPVALUES_CONTINUE, source, fromScope, depth));

	return holder;
}

ExprBase* CreateArgumentUpvalueClose(ExpressionContext &ctx, SynBase *source, FunctionData *onwerFunction)
{
	if(!onwerFunction)
		return NULL;

	ExprSequence *holder = new (ctx.get<ExprSequence>()) ExprSequence(source, ctx.typeVoid, IntrusiveList<ExprBase>());

	onwerFunction->closeUpvalues.push_back(new (ctx.get<CloseUpvaluesData>()) CloseUpvaluesData(holder, CLOSE_UPVALUES_ARGUMENT, source, NULL, 0));

	return holder;
}

void ClosePendingUpvalues(ExpressionContext &ctx, FunctionData *function)
{
	for(CloseUpvaluesData *curr = function->closeUpvalues.head; curr; curr = curr->next)
	{
		CloseUpvaluesData &data = *curr;

		assert(function == ctx.GetCurrentFunction());

		switch(data.type)
		{
		case CLOSE_UPVALUES_FUNCTION:
			for(ScopeData *scope = data.scope; scope; scope = scope->scope)
			{
				for(unsigned i = 0; i < scope->variables.size(); i++)
				{
					VariableData *variable = scope->variables[i];

					if(variable->usedAsExternal)
						data.expr->expressions.push_back(CreateUpvalueClose(ctx, data.source, variable));
				}

				if(scope->ownerFunction)
					break;
			}
			break;
		case CLOSE_UPVALUES_BLOCK:
			for(unsigned i = 0; i < data.scope->variables.size(); i++)
			{
				VariableData *variable = data.scope->variables[i];

				if(variable->usedAsExternal)
					data.expr->expressions.push_back(CreateUpvalueClose(ctx, data.source, variable));
			}
			break;
		case CLOSE_UPVALUES_BREAK:
			for(ScopeData *scope = data.scope; scope; scope = scope->scope)
			{
				if(scope->breakDepth == data.scope->breakDepth - data.depth)
					break;

				for(unsigned i = 0; i < scope->variables.size(); i++)
				{
					VariableData *variable = scope->variables[i];

					if(variable->usedAsExternal)
						data.expr->expressions.push_back(CreateUpvalueClose(ctx, data.source, variable));
				}
			}
			break;
		case CLOSE_UPVALUES_CONTINUE:
			for(ScopeData *scope = data.scope; scope; scope = scope->scope)
			{
				if(scope->contiueDepth == data.scope->contiueDepth - data.depth)
					break;

				for(unsigned i = 0; i < scope->variables.size(); i++)
				{
					VariableData *variable = scope->variables[i];

					if(variable->usedAsExternal)
						data.expr->expressions.push_back(CreateUpvalueClose(ctx, data.source, variable));
				}
			}
			break;
		case CLOSE_UPVALUES_ARGUMENT:
			for(VariableHandle *curr = function->argumentVariables.head; curr; curr = curr->next)
			{
				if(curr->variable->usedAsExternal)
					data.expr->expressions.push_back(CreateUpvalueClose(ctx, data.source, curr->variable));
			}

			if(VariableData *variable = function->contextArgument)
			{
				if(variable->usedAsExternal)
					data.expr->expressions.push_back(CreateUpvalueClose(ctx, data.source, variable));
			}
		}
	}
}

ExprFunctionAccess* CreateValueFunctionWrapper(ExpressionContext &ctx, SynBase *source, ExprBase *value, InplaceStr functionName)
{
	SmallArray<ArgumentData, 32> arguments(ctx.allocator);

	TypeBase *contextRefType = NULL;

	if(ctx.GetFunctionOwner(ctx.scope) == NULL)
		contextRefType = ctx.GetReferenceType(ctx.typeVoid);
	else
		contextRefType = ctx.GetReferenceType(CreateFunctionContextType(ctx, source, functionName));

	SynIdentifier functionNameIdentifier = SynIdentifier(source->begin, source->end, functionName);

	FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, source, ctx.scope, false, false, false, ctx.GetFunctionType(source, value->type, arguments), contextRefType, functionNameIdentifier, IntrusiveList<MatchData>(), ctx.uniqueFunctionId++);

	CheckFunctionConflict(ctx, source, function->name.name);

	ctx.AddFunction(function);

	ctx.PushScope(function);

	function->functionScope = ctx.scope;

	ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, source, function);

	function->argumentsSize = function->functionScope->dataSize;

	IntrusiveList<ExprBase> expressions;
	expressions.push_back(new (ctx.get<ExprReturn>()) ExprReturn(source, ctx.typeVoid, value, NULL, CreateFunctionUpvalueClose(ctx, source, function, ctx.scope)));

	ClosePendingUpvalues(ctx, function);

	ctx.PopScope(SCOPE_FUNCTION);

	ExprVariableDefinition *contextVariableDefinition = NULL;

	if(ctx.GetFunctionOwner(ctx.scope) == NULL)
	{
		contextVariableDefinition = NULL;
	}
	else 
	{
		contextVariableDefinition = CreateFunctionContextVariable(ctx, source, function, NULL);
	}

	function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(source, function->type, function, contextArgumentDefinition, IntrusiveList<ExprVariableDefinition>(), NULL, expressions, contextVariableDefinition);

	ctx.definitions.push_back(function->declaration);

	return new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(source, function->type, function, CreateFunctionContextAccess(ctx, source, function));
}

ExprBase* CreateBinaryOp(ExpressionContext &ctx, SynBase *source, SynBinaryOpType op, ExprBase *lhs, ExprBase *rhs)
{
	if(isType<ExprError>(lhs) || isType<ExprError>(rhs))
		return new (ctx.get<ExprError>()) ExprError(source, ctx.GetErrorType());

	if(op == SYN_BINARY_OP_IN)
		return CreateFunctionCall2(ctx, source, InplaceStr("in"), lhs, rhs, false, false);

	bool skipOverload = false;

	// Built-in comparisons
	if(op == SYN_BINARY_OP_EQUAL || op == SYN_BINARY_OP_NOT_EQUAL)
	{
		if(lhs->type != rhs->type)
		{
			if(lhs->type == ctx.typeNullPtr)
				lhs = CreateCast(ctx, source, lhs, rhs->type, false);

			if(rhs->type == ctx.typeNullPtr)
				rhs = CreateCast(ctx, source, rhs, lhs->type, false);
		}

		if(lhs->type == ctx.typeAutoRef && lhs->type == rhs->type)
		{
			return CreateFunctionCall2(ctx, source, InplaceStr(op == SYN_BINARY_OP_EQUAL ? "__rcomp" : "__rncomp"), lhs, rhs, false, true);
		}

		if(isType<TypeFunction>(lhs->type) && lhs->type == rhs->type)
		{
			IntrusiveList<TypeHandle> types;
			types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(ctx.typeInt));
			TypeBase *type = ctx.GetFunctionType(source, ctx.typeVoid, types);

			lhs = new (ctx.get<ExprTypeCast>()) ExprTypeCast(lhs->source, type, lhs, EXPR_CAST_REINTERPRET);
			rhs = new (ctx.get<ExprTypeCast>()) ExprTypeCast(rhs->source, type, rhs, EXPR_CAST_REINTERPRET);

			return CreateFunctionCall2(ctx, source, InplaceStr(op == SYN_BINARY_OP_EQUAL ? "__pcomp" : "__pncomp"), lhs, rhs, false, true);
		}

		if(isType<TypeUnsizedArray>(lhs->type) && lhs->type == rhs->type)
		{
			if(ExprBase *result = CreateFunctionCall2(ctx, source, InplaceStr(GetOpName(op)), lhs, rhs, true, false))
				return result;

			return CreateFunctionCall2(ctx, source, InplaceStr(op == SYN_BINARY_OP_EQUAL ? "__acomp" : "__ancomp"), lhs, rhs, false, true);
		}

		if(lhs->type == ctx.typeTypeID && rhs->type == ctx.typeTypeID)
			skipOverload = true;
	}

	if(!skipOverload)
	{
		// For && and || try to find a function that accepts a wrapped right-hand-side evaluation
		if((op == SYN_BINARY_OP_LOGICAL_AND || op == SYN_BINARY_OP_LOGICAL_OR) && isType<TypeClass>(lhs->type))
		{
			if(ExprBase *result = CreateFunctionCall2(ctx, source, InplaceStr(GetOpName(op)), lhs, CreateValueFunctionWrapper(ctx, source, rhs, GetTemporaryFunctionName(ctx)), true, false))
				return result;
		}
	}

	// Promotion to bool for some types
	if(op == SYN_BINARY_OP_LOGICAL_AND || op == SYN_BINARY_OP_LOGICAL_OR || op == SYN_BINARY_OP_LOGICAL_XOR)
	{
		lhs = CreateConditionCast(ctx, lhs->source, lhs);
		rhs = CreateConditionCast(ctx, rhs->source, rhs);
	}

	if(lhs->type == ctx.typeVoid)
		return ReportExpected(ctx, source, rhs->type, "ERROR: first operand type is 'void'");

	if(rhs->type == ctx.typeVoid)
		return ReportExpected(ctx, source, lhs->type, "ERROR: second operand type is 'void'");

	bool hasBuiltIn = false;

	hasBuiltIn |= ctx.IsNumericType(lhs->type) && ctx.IsNumericType(rhs->type);
	hasBuiltIn |= lhs->type == ctx.typeTypeID && rhs->type == ctx.typeTypeID && (op == SYN_BINARY_OP_EQUAL || op == SYN_BINARY_OP_NOT_EQUAL);
	hasBuiltIn |= isType<TypeRef>(lhs->type) && lhs->type == rhs->type && (op == SYN_BINARY_OP_EQUAL || op == SYN_BINARY_OP_NOT_EQUAL);
	hasBuiltIn |= isType<TypeEnum>(lhs->type) && lhs->type == rhs->type;

	if(!skipOverload)
	{
		if(ExprBase *result = CreateFunctionCall2(ctx, source, InplaceStr(GetOpName(op)), lhs, rhs, hasBuiltIn, false))
			return result;
	}

	AssertValueExpression(ctx, lhs->source, lhs);
	AssertValueExpression(ctx, rhs->source, rhs);

	if(!hasBuiltIn)
		return ReportExpected(ctx, source, lhs->type, "ERROR: operation %s is not supported on '%.*s' and '%.*s'", GetOpName(op), FMT_ISTR(lhs->type->name), FMT_ISTR(rhs->type->name));

	bool binaryOp = IsBinaryOp(op);
	bool comparisonOp = IsComparisonOp(op);
	bool logicalOp = IsLogicalOp(op);

	if(ctx.IsFloatingPointType(lhs->type) || ctx.IsFloatingPointType(rhs->type))
	{
		if(logicalOp || binaryOp)
			return ReportExpected(ctx, source, lhs->type, "ERROR: operation %s is not supported on '%.*s' and '%.*s'", GetOpName(op), FMT_ISTR(lhs->type->name), FMT_ISTR(rhs->type->name));
	}

	if(logicalOp)
	{
		// Logical operations require both operands to be 'bool'
		lhs = CreateCast(ctx, source, lhs, ctx.typeBool, false);
		rhs = CreateCast(ctx, source, rhs, ctx.typeBool, false);
	}
	else if(ctx.IsNumericType(lhs->type) && ctx.IsNumericType(rhs->type))
	{
		// Numeric operations promote both operands to a common type
		TypeBase *commonType = ctx.GetBinaryOpResultType(lhs->type, rhs->type);

		lhs = CreateCast(ctx, source, lhs, commonType, false);
		rhs = CreateCast(ctx, source, rhs, commonType, false);
	}

	if(lhs->type != rhs->type)
		return ReportExpected(ctx, source, lhs->type, "ERROR: operation %s is not supported on '%.*s' and '%.*s'", GetOpName(op), FMT_ISTR(lhs->type->name), FMT_ISTR(rhs->type->name));

	TypeBase *resultType = NULL;

	if(comparisonOp || logicalOp)
		resultType = ctx.typeBool;
	else
		resultType = lhs->type;

	ExprBase *result = new (ctx.get<ExprBinaryOp>()) ExprBinaryOp(source, resultType, op, lhs, rhs);

	// Arithmetic operation on bool results in an int
	if(lhs->type == ctx.typeBool && !binaryOp && !comparisonOp && !logicalOp)
		return CreateCast(ctx, source, result, ctx.typeInt, false);

	return result;
}

// Apply in reverse order
TypeBase* ApplyArraySizesToType(ExpressionContext &ctx, TypeBase *type, SynBase *sizes)
{
	SynBase *size = sizes;

	if(isType<SynNothing>(size))
		size = NULL;

	if(sizes->next)
		type = ApplyArraySizesToType(ctx, type, sizes->next);

	if(isType<TypeAuto>(type))
	{
		if(size)
			Stop(ctx, size->pos, "ERROR: cannot specify array size for auto");

		return ctx.typeAutoArray;
	}

	if(type == ctx.typeVoid)
		Stop(ctx, sizes->pos, "ERROR: cannot specify array size for void");

	if(!size)
	{
		if(type->size >= 64 * 1024)
			Stop(ctx, sizes->pos, "ERROR: array element size cannot exceed 65535 bytes");

		return ctx.GetUnsizedArrayType(type);
	}

	ExprBase *sizeValue = AnalyzeExpression(ctx, size);

	if(ExprIntegerLiteral *number = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateCast(ctx, size, sizeValue, ctx.typeLong, false))))
	{
		if(number->value <= 0)
			Stop(ctx, size->pos, "ERROR: array size can't be negative or zero");

		if(TypeClass *typeClass = getType<TypeClass>(type))
		{
			if(!typeClass->completed)
				Stop(ctx, size->pos, "ERROR: type '%.*s' is not fully defined", FMT_ISTR(type->name));

			if(typeClass->hasFinalizer)
				Stop(ctx, size->pos, "ERROR: class '%.*s' implements 'finalize' so only an unsized array type can be created", FMT_ISTR(type->name));
		}

		if(type->size >= 64 * 1024)
			Stop(ctx, size->pos, "ERROR: array element size cannot exceed 65535 bytes");

		return ctx.GetArrayType(type, number->value);
	}

	Stop(ctx, size->pos, "ERROR: array size cannot be evaluated");

	return NULL;
}

TypeBase* CreateGenericTypeInstance(ExpressionContext &ctx, SynBase *source, TypeGenericClassProto *proto, IntrusiveList<TypeHandle> &types)
{
	InplaceStr className = GetGenericClassTypeName(ctx, proto, types);

	// Check if type already exists
	if(TypeClass **prev = ctx.genericTypeMap.find(className.hash()))
		return *prev;

	// Switch to original type scope
	ScopeData *scope = ctx.scope;

	ctx.SwitchToScopeAtPoint(NULL, proto->scope, proto->source);

	ExprBase *result = NULL;

	jmp_buf prevErrorHandler;
	memcpy(&prevErrorHandler, &ctx.errorHandler, sizeof(jmp_buf));

	bool prevErrorHandlerNested = ctx.errorHandlerNested;
	ctx.errorHandlerNested = true;

	if(!setjmp(ctx.errorHandler))
	{
		result = AnalyzeClassDefinition(ctx, proto->definition, proto, types);
	}
	else
	{
		// Restore old scope
		ctx.SwitchToScopeAtPoint(proto->source, scope, NULL);

		// Additional error info
		if(ctx.errorBuf)
		{
			char *errorCurr = ctx.errorBuf + strlen(ctx.errorBuf);

			errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "while instantiating generic type %.*s<", FMT_ISTR(proto->name));

			for(TypeHandle *curr = types.head; curr; curr = curr->next)
			{
				TypeBase *type = curr->type;

				errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "%s%.*s", curr != types.head ? ", " : "", FMT_ISTR(type->name));
			}

			errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), ">");

			AddErrorLocationInfo(FindModuleCodeWithSourceLocation(ctx, source->pos.begin), source->pos.begin, ctx.errorBuf, ctx.errorBufSize);

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);
		}

		memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
		ctx.errorHandlerNested = prevErrorHandlerNested;

		longjmp(ctx.errorHandler, 1);
	}

	// Restore old scope
	ctx.SwitchToScopeAtPoint(proto->source, scope, NULL);

	memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
	ctx.errorHandlerNested = prevErrorHandlerNested;

	if(ExprClassDefinition *definition = getType<ExprClassDefinition>(result))
	{
		proto->instances.push_back(result);

		return definition->classType;
	}

	Stop(ctx, source->pos, "ERROR: type '%.*s' couldn't be instantiated", FMT_ISTR(proto->name));

	return NULL;
}

TypeBase* AnalyzeType(ExpressionContext &ctx, SynBase *syntax, bool onlyType = true, bool *failed = NULL)
{
	if(isType<SynTypeAuto>(syntax))
	{
		return ctx.typeAuto;
	}

	if(isType<SynTypeGeneric>(syntax))
	{
		return ctx.typeGeneric;
	}

	if(SynTypeAlias *node = getType<SynTypeAlias>(syntax))
	{
		return ctx.GetGenericAliasType(node->name);
	}

	if(SynTypeReference *node = getType<SynTypeReference>(syntax))
	{
		TypeBase *type = AnalyzeType(ctx, node->type, true, failed);

		if(isType<TypeAuto>(type))
			return ctx.typeAutoRef;

		return ctx.GetReferenceType(type);
	}

	if(SynTypeArray *node = getType<SynTypeArray>(syntax))
	{
		TypeBase *type = AnalyzeType(ctx, node->type, onlyType, failed);

		if(!onlyType && !type)
			return NULL;

		return ApplyArraySizesToType(ctx, type, node->sizes.head);
	}

	if(SynArrayIndex *node = getType<SynArrayIndex>(syntax))
	{
		TypeBase *type = AnalyzeType(ctx, node->value, onlyType, failed);

		if(!onlyType && !type)
			return NULL;

		if(isType<TypeAuto>(type))
		{
			if(!node->arguments.empty())
				Stop(ctx, syntax->pos, "ERROR: cannot specify array size for auto");

			return ctx.typeAutoArray;
		}

		if(node->arguments.empty())
		{
			if(type->size >= 64 * 1024)
				Stop(ctx, syntax->pos, "ERROR: array element size cannot exceed 65535 bytes");

			return ctx.GetUnsizedArrayType(type);
		}

		if(node->arguments.size() > 1)
			Stop(ctx, syntax->pos, "ERROR: ',' is not expected in array type size");

		SynCallArgument *argument = node->arguments.head;

		if(!argument->name.empty())
			Stop(ctx, syntax->pos, "ERROR: named argument not expected in array type size");

		ExprBase *size = AnalyzeExpression(ctx, argument->value);

		if(ExprIntegerLiteral *number = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateCast(ctx, node, size, ctx.typeLong, false))))
		{
			if(TypeArgumentSet *lhs = getType<TypeArgumentSet>(type))
			{
				if(number->value < 0)
					Stop(ctx, syntax->pos, "ERROR: argument index can't be negative");

				if(lhs->types.empty())
					Stop(ctx, syntax->pos, "ERROR: function argument set is empty");

				if(number->value >= lhs->types.size())
					Stop(ctx, syntax->pos, "ERROR: this function type '%.*s' has only %d argument(s)", FMT_ISTR(type->name), lhs->types.size());

				return lhs->types[unsigned(number->value)]->type;
			}

			if(number->value <= 0)
				Stop(ctx, syntax->pos, "ERROR: array size can't be negative or zero");

			if(TypeClass *typeClass = getType<TypeClass>(type))
			{
				if(typeClass->hasFinalizer)
					Stop(ctx, syntax->pos, "ERROR: class '%.*s' implements 'finalize' so only an unsized array type can be created", FMT_ISTR(type->name));
			}

			if(type->size >= 64 * 1024)
				Stop(ctx, syntax->pos, "ERROR: array element size cannot exceed 65535 bytes");

			return ctx.GetArrayType(type, number->value);
		}

		if(!onlyType)
			return NULL;

		Stop(ctx, syntax->pos, "ERROR: index must be a constant expression");
	}

	if(SynTypeFunction *node = getType<SynTypeFunction>(syntax))
	{
		TypeBase *returnType = AnalyzeType(ctx, node->returnType, onlyType, failed);

		if(!onlyType && !returnType)
			return NULL;

		if(returnType == ctx.typeAuto)
			Stop(ctx, syntax->pos, "ERROR: return type of a function type cannot be auto");

		IntrusiveList<TypeHandle> arguments;

		for(SynBase *el = node->arguments.head; el; el = el->next)
		{
			TypeBase *argType = AnalyzeType(ctx, el, onlyType, failed);

			if(!onlyType && !argType)
				return NULL;

			if(argType == ctx.typeAuto)
				Stop(ctx, syntax->pos, "ERROR: function argument cannot be an auto type");

			if(argType == ctx.typeVoid)
				Stop(ctx, syntax->pos, "ERROR: function argument cannot be a void type");

			arguments.push_back(new (ctx.get<TypeHandle>()) TypeHandle(argType));
		}

		return ctx.GetFunctionType(syntax, returnType, arguments);
	}

	if(SynTypeof *node = getType<SynTypeof>(syntax))
	{
		jmp_buf prevErrorHandler;
		memcpy(&prevErrorHandler, &ctx.errorHandler, sizeof(jmp_buf));

		bool prevErrorHandlerNested = ctx.errorHandlerNested;
		ctx.errorHandlerNested = true;

		char *errorBuf = ctx.errorBuf;
		unsigned errorBufSize = ctx.errorBufSize;

		if(failed)
		{
			ctx.errorBuf = 0;
			ctx.errorBufSize = 0;
		}

		if(!setjmp(ctx.errorHandler))
		{
			TypeBase *type = AnalyzeType(ctx, node->value, false);

			if(!type)
			{
				ExprBase *value = AnalyzeExpression(ctx, node->value);

				if(value->type == ctx.typeAuto)
					Stop(ctx, syntax->pos, "ERROR: cannot take typeid from auto type");

				AssertValueExpression(ctx, syntax, value);

				type = value->type;
			}

			memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
			ctx.errorHandlerNested = prevErrorHandlerNested;

			ctx.errorBuf = errorBuf;
			ctx.errorBufSize = errorBufSize;

			if(type)
			{
				assert(!isType<TypeArgumentSet>(type) && !isType<TypeMemberSet>(type) && !isType<TypeFunctionSet>(type));

				return type;
			}
		}
		else
		{
			memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
			ctx.errorHandlerNested = prevErrorHandlerNested;

			ctx.errorBuf = errorBuf;
			ctx.errorBufSize = errorBufSize;

			if(failed)
			{
				*failed = true;
				return ctx.typeGeneric;
			}

			longjmp(ctx.errorHandler, 1);
		}
	}

	if(SynTypeSimple *node = getType<SynTypeSimple>(syntax))
	{
		TypeBase **type = NULL;

		for(ScopeData *nsScope = NamedOrGlobalScopeFrom(ctx.scope); nsScope; nsScope = NamedOrGlobalScopeFrom(nsScope->scope))
		{
			unsigned hash = nsScope->ownerNamespace ? StringHashContinue(nsScope->ownerNamespace->fullNameHash, ".") : GetStringHash("");

			for(SynIdentifier *part = node->path.head; part; part = getType<SynIdentifier>(part->next))
			{
				hash = StringHashContinue(hash, part->name.begin, part->name.end);
				hash = StringHashContinue(hash, ".");
			}

			hash = StringHashContinue(hash, node->name.begin, node->name.end);

			type = ctx.typeMap.find(hash);

			if(type)
				return *type;
		}

		// Might be a variable
		if(!onlyType)
			return NULL;

		Stop(ctx, syntax->pos, "ERROR: '%.*s' is not a known type name", FMT_ISTR(node->name));
	}

	if(SynMemberAccess *node = getType<SynMemberAccess>(syntax))
	{
		TypeBase *value = AnalyzeType(ctx, node->value, onlyType, failed);

		if(!onlyType && !value)
			return NULL;

		if(isType<TypeGeneric>(value))
			return ctx.typeGeneric;

		ExprBase *result = CreateTypeidMemberAccess(ctx, syntax, value, node->member);

		if(ExprTypeLiteral *typeLiteral = getType<ExprTypeLiteral>(result))
			return typeLiteral->value;

		// [n]

		if(!onlyType)
			return NULL;

		// isReference/isArray/isFunction/arraySize/hasMember(x)/class member/class typedef

		Stop(ctx, syntax->pos, "ERROR: typeof expression result is not a type");

		return NULL;
	}

	if(SynTypeGenericInstance *node = getType<SynTypeGenericInstance>(syntax))
	{
		TypeBase *baseType = AnalyzeType(ctx, node->baseType, true, failed);

		// TODO: overloads with a different number of generic arguments

		if(TypeGenericClassProto *proto = getType<TypeGenericClassProto>(baseType))
		{
			IntrusiveList<SynIdentifier> aliases = proto->definition->aliases;

			if(node->types.size() < aliases.size())
				Stop(ctx, syntax->pos, "ERROR: there where only '%d' argument(s) to a generic type that expects '%d'", node->types.size(), aliases.size());

			if(node->types.size() > aliases.size())
				Stop(ctx, syntax->pos, "ERROR: type has only '%d' generic argument(s) while '%d' specified", aliases.size(), node->types.size());

			bool isGeneric = false;
			IntrusiveList<TypeHandle> types;

			for(SynBase *el = node->types.head; el; el = el->next)
			{
				TypeBase *type = AnalyzeType(ctx, el, true, failed);

				if(type == ctx.typeAuto)
					Stop(ctx, syntax->pos, "ERROR: 'auto' type cannot be used as template argument");

				isGeneric |= type->isGeneric;

				types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(type));
			}

			if(isGeneric)
				return ctx.GetGenericClassType(syntax, proto, types);
			
			return CreateGenericTypeInstance(ctx, syntax, proto, types);
		}

		Stop(ctx, syntax->pos, "ERROR: type '%.*s' can't have generic arguments", FMT_ISTR(baseType->name));
	}

	if(!onlyType)
		return NULL;

	Stop(ctx, syntax->pos, "ERROR: unknown type");

	return NULL;
}

unsigned AnalyzeAlignment(ExpressionContext &ctx, SynAlign *syntax)
{
	// noalign
	if(!syntax->value)
		return 1;

	ExprBase *align = AnalyzeNumber(ctx, syntax->value);

	if(ExprIntegerLiteral *alignValue = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateCast(ctx, syntax, align, ctx.typeLong, false))))
	{
		if(alignValue->value > 16)
		{
			Report(ctx, syntax->pos, "ERROR: alignment must be less than 16 bytes");

			return 0;
		}

		if(alignValue->value & (alignValue->value - 1))
		{
			Report(ctx, syntax->pos, "ERROR: alignment must be power of two");

			return 0;
		}

		return unsigned(alignValue->value);
	}

	Report(ctx, syntax->pos, "ERROR: alignment must be a constant expression");

	return 0;
}

ExprBase* AnalyzeNumber(ExpressionContext &ctx, SynNumber *syntax)
{
	InplaceStr &value = syntax->value;

	// Hexadecimal
	if(value.length() > 1 && value.begin[1] == 'x')
	{
		if(value.length() == 2)
			Report(ctx, value.begin + 2, "ERROR: '0x' must be followed by number");

		// Skip 0x
		unsigned pos = 2;

		// Skip leading zeros
		while(value.begin[pos] == '0')
			pos++;

		if(int(value.length() - pos) > 16)
			Report(ctx, value.begin, "ERROR: overflow in hexadecimal constant");

		long long num = (long long)ParseLong(ctx, value.begin + pos, value.end, 16);

		// If number overflows integer number, create long number
		if(int(num) == num)
			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeInt, num);

		return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeLong, num);
	}

	bool isFP = false;

	for(unsigned i = 0; i < value.length(); i++)
	{
		if(value.begin[i] == '.' || value.begin[i] == 'e')
			isFP = true;
	}

	if(!isFP)
	{
		if(syntax->suffix == InplaceStr("b"))
		{
			unsigned pos = 0;

			// Skip leading zeros
			while(value.begin[pos] == '0')
				pos++;

			if(int(value.length() - pos) > 64)
				Report(ctx, value.begin, "ERROR: overflow in binary constant");

			long long num = (long long)ParseLong(ctx, value.begin + pos, value.end, 2);

			// If number overflows integer number, create long number
			if(int(num) == num)
				return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeInt, num);

			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeLong, num);
		}
		else if(syntax->suffix == InplaceStr("l"))
		{
			unsigned long long num = ParseLong(ctx, value.begin, value.end, 10);

			if(num > 9223372036854775807ull)
				Stop(ctx, value.begin, "ERROR: overflow in integer constant");

			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeLong, (long long)num);
		}
		else if(!syntax->suffix.empty())
		{
			Report(ctx, syntax->suffix.begin, "ERROR: unknown number suffix '%.*s'", syntax->suffix.length(), syntax->suffix.begin);
		}

		if(value.length() > 1 && value.begin[0] == '0' && isDigit(value.begin[1]))
		{
			unsigned pos = 0;

			// Skip leading zeros
			while(value.begin[pos] == '0')
				pos++;

			if(int(value.length() - pos) > 22 || (int(value.length() - pos) > 21 && value.begin[pos] != '1'))
				Report(ctx, value.begin, "ERROR: overflow in octal constant");

			long long num = (long long)ParseLong(ctx, value.begin, value.end, 8);

			// If number overflows integer number, create long number
			if(int(num) == num)
				return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeInt, num);

			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeLong, num);
		}

		long long num = (long long)ParseLong(ctx, value.begin, value.end, 10);

		if(int(num) != num)
			Stop(ctx, value.begin, "ERROR: overflow in integer constant");

		return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(syntax, ctx.typeInt, num);
	}

	if(syntax->suffix == InplaceStr("f"))
	{
		double num = ParseDouble(ctx, value.begin);

		return new (ctx.get<ExprRationalLiteral>()) ExprRationalLiteral(syntax, ctx.typeFloat, float(num));
	}
	else if(!syntax->suffix.empty())
	{
		Report(ctx, syntax->suffix.begin, "ERROR: unknown number suffix '%.*s'", syntax->suffix.length(), syntax->suffix.begin);
	}

	double num = ParseDouble(ctx, value.begin);

	return new (ctx.get<ExprRationalLiteral>()) ExprRationalLiteral(syntax, ctx.typeDouble, num);
}

ExprBase* AnalyzeArray(ExpressionContext &ctx, SynArray *syntax)
{
	assert(syntax->values.head);

	SmallArray<ExprBase*, 64> raw(ctx.allocator);

	TypeBase *nestedUnsizedType = NULL;

	for(SynBase *el = syntax->values.head; el; el = el->next)
	{
		ExprBase *value = AnalyzeExpression(ctx, el);

		if(!raw.empty() && raw[0]->type != value->type)
		{
			if(TypeArray *arrayType = getType<TypeArray>(raw[0]->type))
				nestedUnsizedType = ctx.GetUnsizedArrayType(arrayType->subType);
		}

		raw.push_back(value);
	}

	IntrusiveList<ExprBase> values;

	TypeBase *subType = NULL;

	for(unsigned i = 0; i < raw.size(); i++)
	{
		ExprBase *value = raw[i];

		if(nestedUnsizedType)
			value = CreateCast(ctx, value->source, value, nestedUnsizedType, false);

		if(subType == NULL)
		{
			// Can't analyze other elements if the first one didn't compile
			if(isType<ExprError>(value))
				return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

			subType = value->type;
		}
		else if(subType != value->type)
		{
			// Allow numeric promotion
			if(ctx.IsIntegerType(value->type) && ctx.IsFloatingPointType(subType))
				value = CreateCast(ctx, value->source, value, subType, false);
			else if(ctx.IsIntegerType(value->type) && ctx.IsIntegerType(subType) && subType->size > value->type->size)
				value = CreateCast(ctx, value->source, value, subType, false);
			else if(ctx.IsFloatingPointType(value->type) && ctx.IsFloatingPointType(subType) && subType->size > value->type->size)
				value = CreateCast(ctx, value->source, value, subType, false);
			else
				value = ReportExpected(ctx, value->source, value->type, "ERROR: array element %d type '%.*s' doesn't match '%.*s'", i + 1, FMT_ISTR(value->type->name), FMT_ISTR(subType->name));
		}

		if(value->type == ctx.typeVoid)
			Stop(ctx, value->source->pos, "ERROR: array cannot be constructed from void type elements");

		AssertValueExpression(ctx, value->source, value);

		values.push_back(value);
	}

	if(TypeClass *typeClass = getType<TypeClass>(subType))
	{
		if(typeClass->hasFinalizer)
			Stop(ctx, syntax->pos, "ERROR: class '%.*s' implements 'finalize' so only an unsized array type can be created", FMT_ISTR(subType->name));
	}

	if(subType->size >= 64 * 1024)
		Stop(ctx, syntax->pos, "ERROR: array element size cannot exceed 65535 bytes");

	return new (ctx.get<ExprArray>()) ExprArray(syntax, ctx.GetArrayType(subType, values.size()), values);
}

ExprBase* CreateFunctionContextAccess(ExpressionContext &ctx, SynBase *source, FunctionData *function)
{
	assert(!function->scope->ownerType);

	bool inFunctionScope = false;

	// Walk up, but if we reach a type owner, stop - we're not in a context of a function
	for(ScopeData *curr = ctx.scope; curr; curr = curr->scope)
	{
		if(curr->ownerType)
			break;

		if(curr->ownerFunction == function)
		{
			inFunctionScope = true;
			break;
		}
	}

	ExprBase *context = NULL;

	if(inFunctionScope)
	{
		context = CreateVariableAccess(ctx, source, function->contextArgument, true);
	}
	else if(function->contextVariable)
	{
		context = CreateVariableAccess(ctx, source, function->contextVariable, true);

		if(ExprVariableAccess *access = getType<ExprVariableAccess>(context))
		{
			assert(access->variable == function->contextVariable);

			context = new (ctx.get<ExprFunctionContextAccess>()) ExprFunctionContextAccess(source, access->type, function);
		}
	}
	else
	{
		context = new (ctx.get<ExprNullptrLiteral>()) ExprNullptrLiteral(source, function->contextType);
	}

	return context;
}

ExprBase* CreateFunctionAccess(ExpressionContext &ctx, SynBase *source, HashMap<FunctionData*>::Node *function, ExprBase *context)
{
	if(HashMap<FunctionData*>::Node *curr = ctx.functionMap.next(function))
	{
		IntrusiveList<TypeHandle> types;
		IntrusiveList<FunctionHandle> functions;

		types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(function->value->type));
		functions.push_back(new (ctx.get<FunctionHandle>()) FunctionHandle(function->value));

		while(curr)
		{
			types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(curr->value->type));
			functions.push_back(new (ctx.get<FunctionHandle>()) FunctionHandle(curr->value));

			curr = ctx.functionMap.next(curr);
		}

		TypeFunctionSet *type = ctx.GetFunctionSetType(types);

		return new (ctx.get<ExprFunctionOverloadSet>()) ExprFunctionOverloadSet(source, type, functions, context);
	}

	if(!context)
		context = CreateFunctionContextAccess(ctx, source, function->value);

	return new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(source, function->value->type, function->value, context);
}

ExprBase* CreateFunctionCoroutineStateUpdate(ExpressionContext &ctx, SynBase *source, FunctionData *function, int state)
{
	if(!function->coroutine)
		return NULL;

	ExprBase *member = CreateVariableAccess(ctx, source, function->coroutineJumpOffset, true);

	return CreateAssignment(ctx, source, CreateGetAddress(ctx, source, member), new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, state));
}

VariableData* AddFunctionUpvalue(ExpressionContext &ctx, SynBase *source, FunctionData *function, VariableData *data)
{
	if(UpvalueData **prev = function->upvalueVariableMap.find(data))
		return (*prev)->target;

	TypeRef *refType = getType<TypeRef>(function->contextType);

	assert(refType);

	TypeClass *classType = getType<TypeClass>(refType->subType);

	assert(classType);

	ScopeData *currScope = ctx.scope;

	ctx.scope = classType->typeScope;

	unsigned index = 0;

	if(function->upvalueNameSet.contains(data->name))
		index = classType->members.size();

	// Pointer to target variable
	VariableData *target = AllocateClassMember(ctx, source, 0, ctx.GetReferenceType(data->type), GetFunctionContextMemberName(ctx, data->name, InplaceStr("target"), index), true, ctx.uniqueVariableId++);

	classType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(target));

	// Pointer to next upvalue
	VariableData *nextUpvalue = AllocateClassMember(ctx, source, 0, ctx.GetReferenceType(ctx.typeVoid), GetFunctionContextMemberName(ctx, data->name, InplaceStr("nextUpvalue"), index), true, ctx.uniqueVariableId++);

	classType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(nextUpvalue));

	// Copy of the data
	VariableData *copy = AllocateClassMember(ctx, source, data->alignment, data->type, GetFunctionContextMemberName(ctx, data->name, InplaceStr("copy"), index), true, ctx.uniqueVariableId++);

	classType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(copy));

	ctx.scope = currScope;

	data->usedAsExternal = true;

	UpvalueData *upvalue = new (ctx.get<UpvalueData>()) UpvalueData(data, target, nextUpvalue, copy);

	function->upvalues.push_back(upvalue);

	function->upvalueVariableMap.insert(data, upvalue);
	function->upvalueNameSet.insert(data->name);

	return target;
}

VariableData* AddFunctionCoroutineVariable(ExpressionContext &ctx, SynBase *source, FunctionData *function, VariableData *data)
{
	if(CoroutineStateData **prev = function->coroutineStateVariableMap.find(data))
		return (*prev)->storage;

	TypeRef *refType = getType<TypeRef>(function->contextType);

	assert(refType);

	TypeClass *classType = getType<TypeClass>(refType->subType);

	assert(classType);

	ScopeData *currScope = ctx.scope;

	ctx.scope = classType->typeScope;

	unsigned index = 0;

	if(function->coroutineStateNameSet.contains(data->name))
		index = classType->members.size();

	// Copy of the data
	VariableData *storage = AllocateClassMember(ctx, source, data->alignment, data->type, GetFunctionContextMemberName(ctx, data->name, InplaceStr("storage"), index), true, ctx.uniqueVariableId++);

	classType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(storage));

	ctx.scope = currScope;

	CoroutineStateData *state = new (ctx.get<CoroutineStateData>()) CoroutineStateData(data, storage);

	function->coroutineState.push_back(state);

	function->coroutineStateVariableMap.insert(data, state);
	function->coroutineStateNameSet.insert(data->name);

	return storage;
}

ExprBase* CreateVariableAccess(ExpressionContext &ctx, SynBase *source, VariableData *variable, bool handleReference)
{
	if(variable->type == ctx.typeAuto)
		Stop(ctx, source->pos, "ERROR: variable '%.*s' is being used while its type is unknown", FMT_ISTR(variable->name));

	// Is this is a class member access
	if(variable->scope->ownerType)
	{
		ExprBase *thisAccess = CreateVariableAccess(ctx, source, IntrusiveList<SynIdentifier>(), InplaceStr("this"), false);

		if(!thisAccess)
			Stop(ctx, source->pos, "ERROR: 'this' variable is not available");

		// Member access only shifts an address, so we are left with a reference to get value from
		ExprMemberAccess *shift = new (ctx.get<ExprMemberAccess>()) ExprMemberAccess(source, ctx.GetReferenceType(variable->type), thisAccess, variable);

		return new (ctx.get<ExprDereference>()) ExprDereference(source, variable->type, shift);
	}

	ExprBase *access = NULL;

	FunctionData *currentFunction = ctx.GetCurrentFunction();

	FunctionData *variableFunctionOwner = ctx.GetFunctionOwner(variable->scope);

	bool externalAccess = false;
	bool coroutineAccess = false;

	if(currentFunction && variableFunctionOwner)
	{
		if(variableFunctionOwner != currentFunction)
			externalAccess = true;
		else if(currentFunction->coroutine && !IsArgumentVariable(currentFunction, variable))
			coroutineAccess = true;
	}

	if(externalAccess)
	{
		if(currentFunction->scope->ownerType)
			Stop(ctx, source->pos, "ERROR: member function '%.*s' cannot access external variable '%.*s'", FMT_ISTR(currentFunction->name.name), FMT_ISTR(variable->name));

		ExprBase *context = new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, currentFunction->contextArgument->type, currentFunction->contextArgument);

		VariableData *closureMember = AddFunctionUpvalue(ctx, source, currentFunction, variable);

		ExprBase *member = new (ctx.get<ExprMemberAccess>()) ExprMemberAccess(source, ctx.GetReferenceType(closureMember->type), context, closureMember);

		member = new (ctx.get<ExprDereference>()) ExprDereference(source, closureMember->type, member);

		access = new (ctx.get<ExprDereference>()) ExprDereference(source, variable->type, member);
	}
	else if(coroutineAccess)
	{
		ExprBase *context = new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, currentFunction->contextArgument->type, currentFunction->contextArgument);

		VariableData *closureMember = AddFunctionCoroutineVariable(ctx, source, currentFunction, variable);

		ExprBase *member = new (ctx.get<ExprMemberAccess>()) ExprMemberAccess(source, ctx.GetReferenceType(closureMember->type), context, closureMember);

		access = new (ctx.get<ExprDereference>()) ExprDereference(source, variable->type, member);
	}
	else
	{
		access = new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, variable->type, variable);
	}

	if(variable->isReference && handleReference)
	{
		assert(isType<TypeRef>(variable->type));

		TypeRef *type = getType<TypeRef>(variable->type);

		access = new (ctx.get<ExprDereference>()) ExprDereference(source, type->subType, access);
	}

	return access;
}

ExprBase* CreateVariableAccess(ExpressionContext &ctx, SynBase *source, IntrusiveList<SynIdentifier> path, InplaceStr name, bool allowInternal)
{
	VariableData **variable = NULL;

	for(ScopeData *nsScope = NamedOrGlobalScopeFrom(ctx.scope); nsScope; nsScope = NamedOrGlobalScopeFrom(nsScope->scope))
	{
		unsigned hash = nsScope->ownerNamespace ? StringHashContinue(nsScope->ownerNamespace->fullNameHash, ".") : GetStringHash("");

		for(SynIdentifier *part = path.head; part; part = getType<SynIdentifier>(part->next))
		{
			hash = StringHashContinue(hash, part->name.begin, part->name.end);
			hash = StringHashContinue(hash, ".");
		}

		hash = StringHashContinue(hash, name.begin, name.end);

		variable = ctx.variableMap.find(hash);

		if(variable)
			break;
	}

	if(variable)
		return CreateVariableAccess(ctx, source, *variable, true);

	if(path.empty())
	{
		// Try a class constant or an alias
		if(TypeStruct *structType = getType<TypeStruct>(ctx.GetCurrentType()))
		{
			for(ConstantData *curr = structType->constants.head; curr; curr = curr->next)
			{
				if(curr->name == name)
					return CreateLiteralCopy(ctx, source, curr->value);
			}
		}
	}

	if(path.empty())
	{
		if(FindNextTypeFromScope(ctx.scope))
		{
			if(VariableData **variable = ctx.variableMap.find(InplaceStr("this").hash()))
			{
				if(ExprBase *member = CreateMemberAccess(ctx, source, CreateVariableAccess(ctx, source, *variable, true), name, true))
					return member;
			}
		}
	}

	HashMap<FunctionData*>::Node *function = NULL;

	for(ScopeData *nsScope = NamedOrGlobalScopeFrom(ctx.scope); nsScope; nsScope = NamedOrGlobalScopeFrom(nsScope->scope))
	{
		unsigned hash = nsScope->ownerNamespace ? StringHashContinue(nsScope->ownerNamespace->fullNameHash, ".") : GetStringHash("");

		for(SynIdentifier *part = path.head; part; part = getType<SynIdentifier>(part->next))
		{
			hash = StringHashContinue(hash, part->name.begin, part->name.end);
			hash = StringHashContinue(hash, ".");
		}

		hash = StringHashContinue(hash, name.begin, name.end);

		function = ctx.functionMap.first(hash);

		if(function)
		{
			if(function->value->isInternal && !allowInternal)
				function = NULL;

			break;
		}
	}

	if(function)
		return CreateFunctionAccess(ctx, source, function, NULL);

	return NULL;
}

ExprBase* AnalyzeVariableAccess(ExpressionContext &ctx, SynIdentifier *syntax)
{
	ExprBase *value = CreateVariableAccess(ctx, syntax, IntrusiveList<SynIdentifier>(), syntax->name, false);

	if(!value)
		return ReportExpected(ctx, syntax, ctx.GetErrorType(), "ERROR: unknown identifier '%.*s'", FMT_ISTR(syntax->name));

	return value;
}

ExprBase* AnalyzeVariableAccess(ExpressionContext &ctx, SynTypeSimple *syntax)
{
	ExprBase *value = CreateVariableAccess(ctx, syntax, syntax->path, syntax->name, false);

	if(!value)
		return ReportExpected(ctx, syntax, ctx.GetErrorType(), "ERROR: unknown identifier '%.*s'", FMT_ISTR(syntax->name));

	return value;
}

ExprBase* AnalyzePreModify(ExpressionContext &ctx, SynPreModify *syntax)
{
	ExprBase *value = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	ExprBase* wrapped = value;

	if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
		wrapped = new (ctx.get<ExprGetAddress>()) ExprGetAddress(syntax, ctx.GetReferenceType(value->type), node->variable);
	else if(ExprDereference *node = getType<ExprDereference>(value))
		wrapped = node->value;

	if(!isType<TypeRef>(wrapped->type))
		return ReportExpected(ctx, syntax, ctx.GetErrorType(), "ERROR: cannot change immutable value of type %.*s", FMT_ISTR(value->type->name));

	if(!ctx.IsNumericType(value->type))
		return ReportExpected(ctx, syntax, ctx.GetErrorType(), "ERROR: %s is not supported on '%.*s'", (syntax->isIncrement ? "increment" : "decrement"), FMT_ISTR(value->type->name));

	return new (ctx.get<ExprPreModify>()) ExprPreModify(syntax, value->type, wrapped, syntax->isIncrement);
}

ExprBase* AnalyzePostModify(ExpressionContext &ctx, SynPostModify *syntax)
{
	ExprBase *value = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	ExprBase* wrapped = value;

	if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
		wrapped = new (ctx.get<ExprGetAddress>()) ExprGetAddress(syntax, ctx.GetReferenceType(value->type), node->variable);
	else if(ExprDereference *node = getType<ExprDereference>(value))
		wrapped = node->value;

	AssertValueExpression(ctx, syntax, value);

	if(!isType<TypeRef>(wrapped->type))
		return ReportExpected(ctx, syntax, ctx.GetErrorType(), "ERROR: cannot change immutable value of type %.*s", FMT_ISTR(value->type->name));

	if(!ctx.IsNumericType(value->type))
		return ReportExpected(ctx, syntax, ctx.GetErrorType(), "ERROR: %s is not supported on '%.*s'", (syntax->isIncrement ? "increment" : "decrement"), FMT_ISTR(value->type->name));

	return new (ctx.get<ExprPostModify>()) ExprPostModify(syntax, value->type, wrapped, syntax->isIncrement);
}

ExprBase* AnalyzeUnaryOp(ExpressionContext &ctx, SynUnaryOp *syntax)
{
	ExprBase *value = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	if(ExprBase *result = CreateFunctionCall1(ctx, syntax, InplaceStr(GetOpName(syntax->type)), value, true, false))
		return result;

	AssertValueExpression(ctx, syntax, value);

	bool binaryOp = IsBinaryOp(syntax->type);
	bool logicalOp = IsLogicalOp(syntax->type);

	// Type check
	if(ctx.IsFloatingPointType(value->type))
	{
		if(binaryOp || logicalOp)
			Stop(ctx, syntax->pos, "ERROR: unary operation '%s' is not supported on '%.*s'", GetOpName(syntax->type), FMT_ISTR(value->type->name));
	}
	else if(value->type == ctx.typeBool || value->type == ctx.typeAutoRef)
	{
		if(!logicalOp)
			Stop(ctx, syntax->pos, "ERROR: unary operation '%s' is not supported on '%.*s'", GetOpName(syntax->type), FMT_ISTR(value->type->name));
	}
	else if(isType<TypeRef>(value->type))
	{
		if(!logicalOp)
			Stop(ctx, syntax->pos, "ERROR: unary operation '%s' is not supported on '%.*s'", GetOpName(syntax->type), FMT_ISTR(value->type->name));
	}
	else if(!ctx.IsNumericType(value->type))
	{
		Stop(ctx, syntax->pos, "ERROR: unary operation '%s' is not supported on '%.*s'", GetOpName(syntax->type), FMT_ISTR(value->type->name));
	}

	TypeBase *resultType = NULL;

	if(logicalOp)
		resultType = ctx.typeBool;
	else
		resultType = value->type;

	return new (ctx.get<ExprUnaryOp>()) ExprUnaryOp(syntax, resultType, syntax->type, value);
}

ExprBase* AnalyzeBinaryOp(ExpressionContext &ctx, SynBinaryOp *syntax)
{
	ExprBase *lhs = AnalyzeExpression(ctx, syntax->lhs);
	ExprBase *rhs = AnalyzeExpression(ctx, syntax->rhs);

	return CreateBinaryOp(ctx, syntax, syntax->type, lhs, rhs);
}

ExprBase* CreateGetAddress(ExpressionContext &ctx, SynBase *source, ExprBase *value)
{
	AssertValueExpression(ctx, source, value);

	if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
	{
		return new (ctx.get<ExprGetAddress>()) ExprGetAddress(source, ctx.GetReferenceType(value->type), node->variable);
	}
	else if(ExprDereference *node = getType<ExprDereference>(value))
	{
		return node->value;
	}

	Stop(ctx, source->pos, "ERROR: cannot get address of the expression");

	return NULL;
}

ExprBase* AnalyzeGetAddress(ExpressionContext &ctx, SynGetAddress *syntax)
{
	ExprBase *value = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	return CreateGetAddress(ctx, syntax, value);
}

ExprBase* AnalyzeDereference(ExpressionContext &ctx, SynDereference *syntax)
{
	ExprBase *value = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	if(TypeRef *type = getType<TypeRef>(value->type))
	{
		return new (ctx.get<ExprDereference>()) ExprDereference(syntax, type->subType, value);
	}

	if(isType<TypeAutoRef>(value->type))
	{
		return new (ctx.get<ExprUnboxing>()) ExprUnboxing(syntax, ctx.typeAutoRef, value);
	}

	Stop(ctx, syntax->pos, "ERROR: cannot dereference type '%.*s' that is not a pointer", FMT_ISTR(value->type->name));

	return NULL;
}

ExprBase* AnalyzeConditional(ExpressionContext &ctx, SynConditional *syntax)
{
	ExprBase *condition = AnalyzeExpression(ctx, syntax->condition);

	condition = CreateConditionCast(ctx, condition->source, condition);

	ExprBase *trueBlock = AnalyzeStatement(ctx, syntax->trueBlock);
	ExprBase *falseBlock = AnalyzeStatement(ctx, syntax->falseBlock);

	if(isType<ExprError>(condition) || isType<ExprError>(trueBlock) || isType<ExprError>(falseBlock))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	// Handle null pointer promotion
	if(trueBlock->type != falseBlock->type)
	{
		if(trueBlock->type == ctx.typeNullPtr)
			trueBlock = CreateCast(ctx, syntax->trueBlock, trueBlock, falseBlock->type, false);

		if(falseBlock->type == ctx.typeNullPtr)
			falseBlock = CreateCast(ctx, syntax->falseBlock, falseBlock, trueBlock->type, false);
	}

	TypeBase *resultType = NULL;

	if(trueBlock->type == falseBlock->type)
	{
		resultType = trueBlock->type;
	}
	else if(ctx.IsNumericType(trueBlock->type) && ctx.IsNumericType(falseBlock->type))
	{
		resultType = ctx.GetBinaryOpResultType(trueBlock->type, falseBlock->type);

		trueBlock = CreateCast(ctx, syntax->trueBlock, trueBlock, resultType, false);
		falseBlock = CreateCast(ctx, syntax->falseBlock, falseBlock, resultType, false);
	}
	else
	{
		Stop(ctx, syntax->pos, "ERROR: can't find common type between '%.*s' and '%.*s'", FMT_ISTR(trueBlock->type->name), FMT_ISTR(falseBlock->type->name));
	}

	AssertValueExpression(ctx, syntax, condition);

	return new (ctx.get<ExprConditional>()) ExprConditional(syntax, resultType, condition, trueBlock, falseBlock);
}

ExprBase* AnalyzeAssignment(ExpressionContext &ctx, SynAssignment *syntax)
{
	ExprBase *lhs = AnalyzeExpression(ctx, syntax->lhs);
	ExprBase *rhs = AnalyzeExpression(ctx, syntax->rhs);

	if(isType<ExprError>(lhs) || isType<ExprError>(rhs))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	return CreateAssignment(ctx, syntax, lhs, rhs);
}

ExprBase* AnalyzeModifyAssignment(ExpressionContext &ctx, SynModifyAssignment *syntax)
{
	ExprBase *lhs = AnalyzeExpression(ctx, syntax->lhs);
	ExprBase *rhs = AnalyzeExpression(ctx, syntax->rhs);

	if(isType<ExprError>(lhs) || isType<ExprError>(rhs))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	if(ExprBase *result = CreateFunctionCall2(ctx, syntax, InplaceStr(GetOpName(syntax->type)), lhs, rhs, true, false))
		return result;

	// Unwrap modifiable pointer
	ExprBase* wrapped = lhs;

	if(ExprVariableAccess *node = getType<ExprVariableAccess>(lhs))
	{
		wrapped = new (ctx.get<ExprGetAddress>()) ExprGetAddress(lhs->source, ctx.GetReferenceType(lhs->type), node->variable);
	}
	else if(ExprDereference *node = getType<ExprDereference>(lhs))
	{
		wrapped = node->value;
	}

	TypeRef *typeRef = getType<TypeRef>(wrapped->type);

	if(!typeRef)
		Stop(ctx, syntax->pos, "ERROR: cannot change immutable value of type %.*s", FMT_ISTR(lhs->type->name));

	VariableData *storage = AllocateTemporary(ctx, syntax, wrapped->type);

	ExprBase *assignment = CreateAssignment(ctx, syntax, CreateVariableAccess(ctx, syntax, storage, false), wrapped);

	ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, storage, assignment);

	ExprBase *lhsValue = new (ctx.get<ExprDereference>()) ExprDereference(syntax, typeRef->subType, CreateVariableAccess(ctx, syntax, storage, false));

	ExprBase *result = CreateBinaryOp(ctx, syntax, GetBinaryOpType(syntax->type), lhsValue, rhs);

	return CreateSequence(ctx, syntax, definition, CreateAssignment(ctx, syntax, new (ctx.get<ExprDereference>()) ExprDereference(syntax, typeRef->subType, CreateVariableAccess(ctx, syntax, storage, false)), result));
}

ExprBase* CreateTypeidMemberAccess(ExpressionContext &ctx, SynBase *source, TypeBase *type, InplaceStr member)
{
	if(member == InplaceStr("isReference"))
	{
		return new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(source, ctx.typeBool, isType<TypeRef>(type));
	}

	if(member == InplaceStr("isArray"))
	{
		return new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(source, ctx.typeBool, isType<TypeArray>(type) || isType<TypeUnsizedArray>(type));
	}

	if(member == InplaceStr("isFunction"))
	{
		return new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(source, ctx.typeBool, isType<TypeFunction>(type));
	}

	if(member == InplaceStr("arraySize"))
	{
		if(TypeArray *arrType = getType<TypeArray>(type))
			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, arrType->length);

		if(isType<TypeUnsizedArray>(type))
			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, -1);

		Stop(ctx, source->pos, "ERROR: 'arraySize' can only be applied to an array type, but we have '%.*s'", FMT_ISTR(type->name));
	}

	if(member == InplaceStr("size"))
	{
		if(TypeArgumentSet *argumentsType = getType<TypeArgumentSet>(type))
			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, argumentsType->types.size());

		Stop(ctx, source->pos, "ERROR: 'size' can only be applied to an function type, but we have '%.*s'", FMT_ISTR(type->name));
	}

	if(member == InplaceStr("argument"))
	{
		if(TypeFunction *functionType = getType<TypeFunction>(type))
			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, new (ctx.get<TypeArgumentSet>()) TypeArgumentSet(GetArgumentSetTypeName(ctx, functionType->arguments), functionType->arguments));

		Stop(ctx, source->pos, "ERROR: 'argument' can only be applied to a function type, but we have '%.*s'", FMT_ISTR(type->name));
	}

	if(member == InplaceStr("return"))
	{
		if(TypeFunction *functionType = getType<TypeFunction>(type))
			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, functionType->returnType);

		Stop(ctx, source->pos, "ERROR: 'return' can only be applied to a function type, but we have '%.*s'", FMT_ISTR(type->name));
	}

	if(member == InplaceStr("target"))
	{
		if(TypeRef *refType = getType<TypeRef>(type))
			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, refType->subType);

		if(TypeArray *arrType = getType<TypeArray>(type))
			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, arrType->subType);

		if(TypeUnsizedArray *arrType = getType<TypeUnsizedArray>(type))
			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, arrType->subType);

		Stop(ctx, source->pos, "ERROR: 'target' can only be applied to a pointer or array type, but we have '%.*s'", FMT_ISTR(type->name));
	}

	if(member == InplaceStr("first"))
	{
		if(TypeArgumentSet *argumentsType = getType<TypeArgumentSet>(type))
		{
			if(argumentsType->types.empty())
				Stop(ctx, source->pos, "ERROR: function argument set is empty");

			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, argumentsType->types.head->type);
		}

		Stop(ctx, source->pos, "ERROR: 'first' can only be applied to a function type, but we have '%.*s'", FMT_ISTR(type->name));
	}

	if(member == InplaceStr("last"))
	{
		if(TypeArgumentSet *argumentsType = getType<TypeArgumentSet>(type))
		{
			if(argumentsType->types.empty())
				Stop(ctx, source->pos, "ERROR: function argument set is empty");

			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, argumentsType->types.tail->type);
		}

		Stop(ctx, source->pos, "ERROR: 'last' can only be applied to a function type, but we have '%.*s'", FMT_ISTR(type->name));
	}

	if(TypeClass *classType = getType<TypeClass>(type))
	{
		for(MatchData *curr = classType->aliases.head; curr; curr = curr->next)
		{
			if(curr->name == member)
				return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, curr->type);
		}

		for(MatchData *curr = classType->generics.head; curr; curr = curr->next)
		{
			if(curr->name == member)
				return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, curr->type);
		}
	}

	if(TypeStruct *structType = getType<TypeStruct>(type))
	{
		for(VariableHandle *curr = structType->members.head; curr; curr = curr->next)
		{
			if(curr->variable->name == member)
				return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, curr->variable->type);
		}

		for(ConstantData *curr = structType->constants.head; curr; curr = curr->next)
		{
			if(curr->name == member)
				return CreateLiteralCopy(ctx, source, curr->value);
		}

		if(member == InplaceStr("hasMember"))
			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, new (ctx.get<TypeMemberSet>()) TypeMemberSet(GetMemberSetTypeName(ctx, structType), structType));
	}

	if(TypeGenericClass *typeGenericClass = getType<TypeGenericClass>(type))
	{
		for(SynIdentifier *curr = typeGenericClass->proto->definition->aliases.head; curr; curr = getType<SynIdentifier>(curr->next))
		{
			if(curr->name == member)
				return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, ctx.typeGeneric);
		}
	}

	return NULL;
}

ExprBase* CreateAutoRefFunctionSet(ExpressionContext &ctx, SynBase *source, ExprBase *value, InplaceStr name, TypeClass *preferredParent)
{
	IntrusiveList<TypeHandle> types;
	IntrusiveList<FunctionHandle> functions;

	// Find all member functions with the specified name
	for(unsigned i = 0; i < ctx.functions.size(); i++)
	{
		FunctionData *function = ctx.functions[i];

		TypeBase *parentType = function->scope->ownerType;

		if(!parentType)
			continue;

		unsigned hash = StringHashContinue(parentType->nameHash, "::");

		hash = StringHashContinue(hash, name.begin, name.end);

		if(function->nameHash != hash)
			continue;

		if(preferredParent && !IsDerivedFrom(preferredParent, getType<TypeClass>(parentType)))
			continue;

		FunctionHandle *prev = NULL;

		// Pointer to generic types don't stricly match because they might be resolved to different types
		if (!function->type->isGeneric)
		{
			for(FunctionHandle *curr = functions.head; curr; curr = curr->next)
			{
				if(curr->function->type == function->type)
				{
					prev = curr;
					break;
				}
			}
		}

		if(prev)
		{
			// Select the most specialized function for extendable member function call
			if(preferredParent)
			{
				unsigned prevDepth = GetDerivedFromDepth(preferredParent, getType<TypeClass>(prev->function->scope->ownerType));
				unsigned currDepth = GetDerivedFromDepth(preferredParent, getType<TypeClass>(function->scope->ownerType));

				if (currDepth < prevDepth)
					prev->function = function;
			}

			continue;
		}

		types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(function->type));
		functions.push_back(new (ctx.get<FunctionHandle>()) FunctionHandle(function));
	}

	if(functions.empty())
	{
		if(value->type != ctx.typeAutoRef)
			return NULL;

		Stop(ctx, source->pos, "ERROR: function '%.*s' is undefined in any of existing classes", FMT_ISTR(name));
	}

	TypeFunctionSet *type = ctx.GetFunctionSetType(types);

	return new (ctx.get<ExprFunctionOverloadSet>()) ExprFunctionOverloadSet(source, type, functions, value);
}

ExprBase* CreateMemberAccess(ExpressionContext &ctx, SynBase *source, ExprBase *value, InplaceStr name, bool allowFailure)
{
	ExprBase* wrapped = value;

	if(TypeRef *refType = getType<TypeRef>(value->type))
	{
		value = new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, value);

		if(TypeRef *refType = getType<TypeRef>(value->type))
		{
			wrapped = value;

			value = new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, value);
		}
	}
	else if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
	{
		wrapped = new (ctx.get<ExprGetAddress>()) ExprGetAddress(source, ctx.GetReferenceType(value->type), node->variable);
	}
	else if(ExprDereference *node = getType<ExprDereference>(value))
	{
		wrapped = node->value;
	}
	else if(!isType<TypeRef>(wrapped->type))
	{
		AssertValueExpression(ctx, source, wrapped);

		VariableData *storage = AllocateTemporary(ctx, source, wrapped->type);

		ExprBase *assignment = CreateAssignment(ctx, source, CreateVariableAccess(ctx, source, storage, false), value);

		ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(value->source, ctx.typeVoid, storage, assignment);

		wrapped = CreateSequence(ctx, source, definition, CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, storage, false)));
	}

	if(TypeArray *node = getType<TypeArray>(value->type))
	{
		if(name == InplaceStr("size"))
			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, node->length);
	}

	if(isType<TypeRef>(wrapped->type))
	{
		if(ExprTypeLiteral *node = getType<ExprTypeLiteral>(value))
		{
			if(ExprBase *result = CreateTypeidMemberAccess(ctx, source, node->value, name))
				return result;
		}

		if(TypeStruct *node = getType<TypeStruct>(value->type))
		{
			// Search for a member variable
			for(VariableHandle *el = node->members.head; el; el = el->next)
			{
				if(el->variable->name == name)
				{
					// Member access only shifts an address, so we are left with a reference to get value from
					ExprMemberAccess *shift = new (ctx.get<ExprMemberAccess>()) ExprMemberAccess(source, ctx.GetReferenceType(el->variable->type), wrapped, el->variable);

					ExprBase *memberValue = new (ctx.get<ExprDereference>()) ExprDereference(source, el->variable->type, shift);

					if(el->variable->isReadonly)
						return new (ctx.get<ExprPassthrough>()) ExprPassthrough(source, el->variable->type, memberValue);

					return memberValue;
				}
			}

			// Search for a member constant
			for(ConstantData *curr = node->constants.head; curr; curr = curr->next)
			{
				if(curr->name == name)
					return CreateLiteralCopy(ctx, source, curr->value);
			}
		}

		if(value->type == ctx.typeAutoRef)
			return CreateAutoRefFunctionSet(ctx, source, value, name, NULL);

		if(TypeClass *classType = getType<TypeClass>(value->type))
		{
			if(classType->baseClass != NULL || classType->extendable)
			{
				if(ExprBase *overloads = CreateAutoRefFunctionSet(ctx, source, wrapped, name, classType))
					return overloads;
			}
		}

		// Check if a name resembles a type alias of the value class
		TypeBase *aliasType = NULL;

		if(TypeBase **typeName = ctx.typeMap.find(name.hash()))
		{
			TypeBase *type = *typeName;

			if(type == value->type && type->name != name)
			{
				if(TypeClass *typeClass = getType<TypeClass>(type))
				{
					if(typeClass->proto)
						type = typeClass->proto;
				}

				aliasType = type;
			}
		}

		// Look for a member function
		ExprBase *mainFuncton = NULL;

		unsigned hash = StringHashContinue(value->type->nameHash, "::");

		hash = StringHashContinue(hash, name.begin, name.end);

		if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(hash))
			mainFuncton = CreateFunctionAccess(ctx, source, function, wrapped);

		if(!mainFuncton && aliasType)
			mainFuncton = CreateConstructorAccess(ctx, source, value->type, false, wrapped);

		if(!mainFuncton)
		{
			if(TypeArray *node = getType<TypeArray>(value->type))
			{
				TypeUnsizedArray *arrayType = ctx.GetUnsizedArrayType(node->subType);

				unsigned hash = StringHashContinue(arrayType->nameHash, "::");

				hash = StringHashContinue(hash, name.begin, name.end);

				if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(hash))
				{
					wrapped = CreateCast(ctx, source, wrapped, ctx.GetReferenceType(arrayType), false);

					return CreateFunctionAccess(ctx, source, function, wrapped);
				}
			}
		}

		ExprBase *baseFunction = NULL;

		// Look for a member function in a generic class base
		if(TypeClass *classType = getType<TypeClass>(value->type))
		{
			if(TypeGenericClassProto *protoType = classType->proto)
			{
				unsigned hash = StringHashContinue(protoType->nameHash, "::");

				hash = StringHashContinue(hash, name.begin, name.end);

				if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(hash))
					baseFunction = CreateFunctionAccess(ctx, source, function, wrapped);

				if(!baseFunction && aliasType)
					baseFunction = CreateConstructorAccess(ctx, source, protoType, false, wrapped);
			}
		}

		// Add together instantiated and generic base functions
		if(mainFuncton && baseFunction)
		{
			IntrusiveList<TypeHandle> types;
			IntrusiveList<FunctionHandle> overloads;

			// Collect a set of available functions
			SmallArray<FunctionValue, 32> functions(ctx.allocator);

			GetNodeFunctions(ctx, source, mainFuncton, functions);
			GetNodeFunctions(ctx, source, baseFunction, functions);

			for(unsigned i = 0; i < functions.size(); i++)
			{
				FunctionValue function = functions[i];

				bool instantiated = false;

				for(FunctionHandle *curr = overloads.head; curr && !instantiated; curr = curr->next)
				{
					if(SameArguments(curr->function->type, function.function->type))
						instantiated = true;
				}

				if(instantiated)
					continue;

				types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(function.function->type));
				overloads.push_back(new (ctx.get<FunctionHandle>()) FunctionHandle(function.function));
			}

			TypeFunctionSet *type = ctx.GetFunctionSetType(types);

			return new (ctx.get<ExprFunctionOverloadSet>()) ExprFunctionOverloadSet(source, type, overloads, wrapped);
		}

		if(mainFuncton)
			return mainFuncton;

		if(baseFunction)
		{
			if(ExprFunctionAccess *node = getType<ExprFunctionAccess>(baseFunction))
			{
				if(node->function->scope->ownerType && node->function->scope->ownerType->isGeneric && !node->function->type->isGeneric)
				{
					if(FunctionValue bestOverload = GetFunctionForType(ctx, source, baseFunction, node->function->type))
						return new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(bestOverload.source, bestOverload.function->type, bestOverload.function, bestOverload.context);
				}
			}

			return baseFunction;
		}

		// Look for an accessor
		hash = StringHashContinue(hash, "$");

		ExprBase *access = NULL;

		if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(hash))
			access = CreateFunctionAccess(ctx, source, function, wrapped);

		if(!access)
		{
			if(TypeArray *node = getType<TypeArray>(value->type))
			{
				TypeUnsizedArray *arrayType = ctx.GetUnsizedArrayType(node->subType);

				unsigned hash = StringHashContinue(arrayType->nameHash, "::");

				hash = StringHashContinue(hash, name.begin, name.end);

				if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(hash))
				{
					wrapped = CreateCast(ctx, source, wrapped, ctx.GetReferenceType(arrayType), false);

					access = CreateFunctionAccess(ctx, source, function, wrapped);
				}
			}
		}

		if(access)
		{
			ExprBase *call = CreateFunctionCall(ctx, source, access, IntrusiveList<TypeHandle>(), NULL, false);;

			if(TypeRef *refType = getType<TypeRef>(call->type))
				return new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, call);

			return call;
		}

		// Look for an accessor function in a generic class base
		if(TypeClass *classType = getType<TypeClass>(value->type))
		{
			if(TypeGenericClassProto *protoType = classType->proto)
			{
				unsigned hash = StringHashContinue(protoType->nameHash, "::");

				hash = StringHashContinue(hash, name.begin, name.end);

				// Look for an accessor
				hash = StringHashContinue(hash, "$");

				if(HashMap<FunctionData*>::Node *function = ctx.functionMap.first(hash))
				{
					ExprBase *access = CreateFunctionAccess(ctx, source, function, wrapped);

					ExprBase *call = CreateFunctionCall(ctx, source, access, IntrusiveList<TypeHandle>(), NULL, false);

					if(TypeRef *refType = getType<TypeRef>(call->type))
						return new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, call);

					return call;
				}
			}
		}

		if(allowFailure)
			return NULL;

		Stop(ctx, source->pos, "ERROR: member variable or function '%.*s' is not defined in class '%.*s'", FMT_ISTR(name), FMT_ISTR(value->type->name));
	}

	Stop(ctx, source->pos, "ERROR: can't access member '%.*s' of type '%.*s'", FMT_ISTR(name), FMT_ISTR(value->type->name));

	return NULL;
}

ExprBase* AnalyzeMemberAccess(ExpressionContext &ctx, SynMemberAccess *syntax)
{
	// It could be a type property
	if(TypeBase *type = AnalyzeType(ctx, syntax->value, false))
	{
		if(ExprBase *result = CreateTypeidMemberAccess(ctx, syntax, type, syntax->member))
			return result;
	}

	ExprBase* value = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	return CreateMemberAccess(ctx, syntax, value, syntax->member, false);
}

ExprBase* CreateArrayIndex(ExpressionContext &ctx, SynBase *source, ExprBase *value, ArrayView<ArgumentData> arguments)
{
	// Handle argument[x] expresion
	if(ExprTypeLiteral *type = getType<ExprTypeLiteral>(value))
	{
		if(TypeArgumentSet *argumentSet = getType<TypeArgumentSet>(type->value))
		{
			if(arguments.size() == 1)
			{
				if(ExprIntegerLiteral *number = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, arguments[0].value)))
				{
					if(number->value < 0)
						Stop(ctx, source->pos, "ERROR: argument index can't be negative");

					if(argumentSet->types.empty())
						Stop(ctx, source->pos, "ERROR: function argument set is empty");

					if(number->value >= argumentSet->types.size())
						Stop(ctx, source->pos, "ERROR: function arguemnt set '%.*s' has only %d argument(s)", FMT_ISTR(argumentSet->name), argumentSet->types.size());

					return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, argumentSet->types[unsigned(number->value)]->type);
				}
				else
				{
					Stop(ctx, source->pos, "ERROR: expression didn't evaluate to a constant number");
				}
			}
		}
	}

	ExprBase* wrapped = value;

	if(TypeRef *refType = getType<TypeRef>(value->type))
	{
		value = new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, value);

		if(isType<TypeUnsizedArray>(value->type))
			wrapped = value;
	}
	else if(isType<TypeUnsizedArray>(value->type))
	{
		wrapped = value; // Do not modify
	}
	else if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
	{
		wrapped = new (ctx.get<ExprGetAddress>()) ExprGetAddress(source, ctx.GetReferenceType(value->type), node->variable);
	}
	else if(ExprDereference *node = getType<ExprDereference>(value))
	{
		wrapped = node->value;
	}
	else if(!isType<TypeRef>(wrapped->type))
	{
		AssertValueExpression(ctx, source, wrapped);

		VariableData *storage = AllocateTemporary(ctx, source, wrapped->type);

		ExprBase *assignment = CreateAssignment(ctx, source, CreateVariableAccess(ctx, source, storage, false), value);

		ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, storage, assignment);

		wrapped = CreateSequence(ctx, source, definition, CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, storage, false)));
	}

	if(isType<TypeRef>(wrapped->type) || isType<TypeUnsizedArray>(value->type))
	{
		bool findOverload = arguments.empty() || arguments.size() > 1;

		for(unsigned i = 0; i < arguments.size(); i++)
		{
			if(!arguments[i].name.empty())
				findOverload = true;
		}

		if(ExprBase *overloads = CreateVariableAccess(ctx, source, IntrusiveList<SynIdentifier>(), InplaceStr("[]"), false))
		{
			SmallArray<ArgumentData, 32> callArguments(ctx.allocator);
			callArguments.push_back(ArgumentData(wrapped->source, false, InplaceStr(), wrapped->type, wrapped));

			for(unsigned i = 0; i < arguments.size(); i++)
				callArguments.push_back(arguments[i]);

			if(ExprBase *result = CreateFunctionCall(ctx, source, overloads, callArguments, true))
			{
				if(TypeRef *refType = getType<TypeRef>(result->type))
					return new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, result);

				return result;
			}

			callArguments[0] = ArgumentData(value->source, false, InplaceStr(), value->type, value);

			if(ExprBase *result = CreateFunctionCall(ctx, source, overloads, callArguments, !findOverload))
			{
				if(TypeRef *refType = getType<TypeRef>(result->type))
					return new (ctx.get<ExprDereference>()) ExprDereference(source, refType->subType, result);

				return result;
			}
		}

		if(findOverload)
			Stop(ctx, source->pos, "ERROR: overloaded '[]' operator is not available");

		ExprBase *index = CreateCast(ctx, source, arguments[0].value, ctx.typeInt, false);

		ExprIntegerLiteral *indexValue = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, index));

		if(indexValue && indexValue->value < 0)
			Stop(ctx, source->pos, "ERROR: array index cannot be negative");

		if(TypeArray *type = getType<TypeArray>(value->type))
		{
			if(indexValue && indexValue->value >= type->length)
				Stop(ctx, source->pos, "ERROR: array index out of bounds");

			// Array index only shifts an address, so we are left with a reference to get value from
			ExprArrayIndex *shift = new (ctx.get<ExprArrayIndex>()) ExprArrayIndex(source, ctx.GetReferenceType(type->subType), wrapped, index);

			return new (ctx.get<ExprDereference>()) ExprDereference(source, type->subType, shift);
		}

		if(TypeUnsizedArray *type = getType<TypeUnsizedArray>(value->type))
		{
			// Array index only shifts an address, so we are left with a reference to get value from
			ExprArrayIndex *shift = new (ctx.get<ExprArrayIndex>()) ExprArrayIndex(source, ctx.GetReferenceType(type->subType), wrapped, index);

			return new (ctx.get<ExprDereference>()) ExprDereference(source, type->subType, shift);
		}
	}

	Stop(ctx, source->pos, "ERROR: type '%.*s' is not an array", FMT_ISTR(value->type->name));

	return NULL;
}

ExprBase* AnalyzeArrayIndex(ExpressionContext &ctx, SynArrayIndex *syntax)
{
	ExprBase *value = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	SmallArray<ArgumentData, 32> arguments(ctx.allocator);

	for(SynCallArgument *curr = syntax->arguments.head; curr; curr = getType<SynCallArgument>(curr->next))
	{
		ExprBase *index = AnalyzeExpression(ctx, curr->value);

		if(isType<ExprError>(value))
			return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

		arguments.push_back(ArgumentData(index->source, false, curr->name, index->type, index));
	}

	return CreateArrayIndex(ctx, syntax, value, arguments);
}

ExprBase* AnalyzeArrayIndex(ExpressionContext &ctx, SynTypeArray *syntax)
{
	assert(syntax->sizes.head);

	SynArrayIndex *value = NULL;

	// Convert to a chain of SynArrayIndex
	for(SynBase *el = syntax->sizes.head; el; el = el->next)
	{
		IntrusiveList<SynCallArgument> arguments;

		if(!isType<SynNothing>(el))
			arguments.push_back(new (ctx.get<SynCallArgument>()) SynCallArgument(el->begin, el->end, InplaceStr(), el));

		value = new (ctx.get<SynArrayIndex>()) SynArrayIndex(el->begin, el->end, value ? value : syntax->type, arguments);
	}

	return AnalyzeArrayIndex(ctx, value);
}

InplaceStr GetTemporaryFunctionName(ExpressionContext &ctx)
{
	char *name = (char*)ctx.allocator->alloc(16);
	sprintf(name, "$func%d", ctx.unnamedFuncCount++);

	return InplaceStr(name);
}

InplaceStr GetDefaultArgumentWrapperFunctionName(ExpressionContext &ctx, FunctionData *function, InplaceStr argumentName)
{
	char *name = (char*)ctx.allocator->alloc(function->name.name.length() + argumentName.length() + 16);
	sprintf(name, "%.*s_%u_%.*s$", FMT_ISTR(function->name.name), function->type->nameHash, FMT_ISTR(argumentName));

	return InplaceStr(name);
}

InplaceStr GetFunctionName(ExpressionContext &ctx, ScopeData *scope, TypeBase *parentType, InplaceStr name, bool isOperator, bool isAccessor)
{
	if(name.empty())
		return GetTemporaryFunctionName(ctx);

	return GetFunctionNameInScope(ctx, scope, parentType, name, isOperator, isAccessor);
}

bool HasNamedCallArguments(ArrayView<ArgumentData> arguments)
{
	for(unsigned i = 0, e = arguments.count; i < e; i++)
	{
		if(!arguments.data[i].name.empty())
			return true;
	}

	return false;
}

bool HasMatchingArgumentNames(ArrayView<ArgumentData> &functionArguments, ArrayView<ArgumentData> arguments)
{
	for(unsigned i = 0; i < arguments.size(); i++)
	{
		InplaceStr argumentName = arguments[i].name;

		if(argumentName.empty())
			continue;

		bool found = false;

		for(unsigned k = 0; k < functionArguments.size(); k++)
		{
			if(functionArguments[k].name == argumentName)
			{
				found = true;
				break;
			}
		}

		if(!found)
			return false;
	}

	return true;
}

bool PrepareArgumentsForFunctionCall(ExpressionContext &ctx, SynBase *source, ArrayView<ArgumentData> functionArguments, ArrayView<ArgumentData> arguments, SmallArray<CallArgumentData, 16> &result, unsigned *extraRating, bool prepareValues)
{
	result.clear();

	if(HasNamedCallArguments(arguments))
	{
		if(!HasMatchingArgumentNames(functionArguments, arguments))
			return false;

		// Add first unnamed arguments
		for(unsigned i = 0; i < arguments.size(); i++)
		{
			ArgumentData &argument = arguments[i];

			if(argument.name.empty())
				result.push_back(CallArgumentData(argument.type, argument.value));
			else
				break;
		}

		unsigned unnamedCount = result.size();

		// Reserve slots for all remaining arguments
		for(unsigned i = unnamedCount; i < functionArguments.size(); i++)
			result.push_back(CallArgumentData(NULL, NULL));

		// Put named arguments in appropriate slots
		for(unsigned i = unnamedCount; i < arguments.size(); i++)
		{
			ArgumentData &argument = arguments[i];

			unsigned targetPos = 0;

			for(unsigned k = 0; k < functionArguments.size(); k++)
			{
				if(functionArguments[k].name == argument.name)
				{
					if(result[targetPos].type != NULL)
						Stop(ctx, argument.value->source->pos, "ERROR: argument '%.*s' is already set", FMT_ISTR(argument.name));

					result[targetPos] = CallArgumentData(argument.type, argument.value);
					break;
				}

				targetPos++;
			}
		}

		// Fill in any unset arguments with default values
		for(unsigned i = 0; i < functionArguments.size(); i++)
		{
			ArgumentData &argument = functionArguments[i];

			if(result[i].type == NULL)
			{
				if(ExprBase *value = argument.value)
					result[i] = CallArgumentData(value->type, new (ctx.get<ExprPassthrough>()) ExprPassthrough(argument.source, value->type, value));
			}
		}

		// All arguments must be set
		for(unsigned i = unnamedCount; i < arguments.size(); i++)
		{
			if(result[i].type == NULL)
				return false;
		}
	}
	else
	{
		// Add arguments
		for(unsigned i = 0; i < arguments.size(); i++)
		{
			ArgumentData &argument = arguments[i];

			result.push_back(CallArgumentData(argument.type, argument.value));
		}

		// Add any arguments with default values
		for(unsigned i = result.size(); i < functionArguments.size(); i++)
		{
			ArgumentData &argument = functionArguments[i];

			if(ExprBase *value = argument.value)
				result.push_back(CallArgumentData(value->type, new (ctx.get<ExprPassthrough>()) ExprPassthrough(argument.source, value->type, value)));
		}

		// Create variadic pack if neccessary
		TypeBase *varArgType = ctx.GetUnsizedArrayType(ctx.typeAutoRef);

		if(!functionArguments.empty() && functionArguments.back().type == varArgType && !functionArguments.back().isExplicit)
		{
			if(result.size() >= functionArguments.size() - 1 && !(result.size() == functionArguments.size() && result.back().type == varArgType))
			{
				if(extraRating)
					*extraRating = 10 + (result.size() - functionArguments.size() - 1) * 5;

				ExprBase *value = NULL;

				if(prepareValues)
				{
					if(!result.empty())
						source = result[0].value->source;

					IntrusiveList<ExprBase> values;

					for(unsigned i = functionArguments.size() - 1; i < result.size(); i++)
					{
						ExprBase *value = result[i].value;

						if(TypeArray *arrType = getType<TypeArray>(value->type))
						{
							// type[N] is converted to type[] first
							value = CreateCast(ctx, value->source, value, ctx.GetUnsizedArrayType(arrType->subType), false);
						}

						values.push_back(CreateCast(ctx, value->source, value, ctx.typeAutoRef, true));
					}

					if(values.empty())
						value = new (ctx.get<ExprNullptrLiteral>()) ExprNullptrLiteral(source, ctx.typeNullPtr);
					else
						value = new (ctx.get<ExprArray>()) ExprArray(source, ctx.GetArrayType(ctx.typeAutoRef, values.size()), values);

					value = CreateCast(ctx, source, value, varArgType, true);
				}

				result.shrink(functionArguments.size() - 1);
				result.push_back(CallArgumentData(varArgType, value));
			}
		}
	}

	if(result.size() != functionArguments.size())
		return false;

	// Convert all arguments to target type if this is a real call
	if(prepareValues)
	{
		for(unsigned i = 0; i < result.size(); i++)
		{
			CallArgumentData &argument = result[i];

			assert(argument.value);

			TypeBase *target = functionArguments[i].type;

			argument.value = CreateCast(ctx, argument.value->source, argument.value, target, true);
		}
	}

	return true;
}

unsigned GetFunctionRating(ExpressionContext &ctx, FunctionData *function, TypeFunction *instance, ArrayView<CallArgumentData> arguments)
{
	if(function->arguments.size() != arguments.size())
		return ~0u;	// Definitely, this isn't the function we are trying to call. Argument count does not match.

	unsigned rating = 0;

	unsigned i = 0;

	for(TypeHandle *argType = instance->arguments.head; argType; argType = argType->next, i++)
	{
		ArgumentData &expectedArgument = function->arguments[i];
		TypeBase *expectedType = argType->type;

		CallArgumentData &actualArgument = arguments[i];
		TypeBase *actualType = actualArgument.type;

		if(expectedType != actualType)
		{
			if(actualType == ctx.typeNullPtr)
			{
				// nullptr is convertable to T ref, T[] and function pointers
				if(isType<TypeRef>(expectedType) || isType<TypeUnsizedArray>(expectedType) || isType<TypeFunction>(expectedType))
					continue;

				// nullptr is also convertable to auto ref and auto[], but it has the same rating as type ref -> auto ref and array -> auto[] defined below
				if(expectedType == ctx.typeAutoRef || expectedType == ctx.typeAutoArray)
				{
					rating += 5;
					continue;
				}
			}

			// Generic function argument
			if(expectedType->isGeneric)
				continue;

			if(expectedArgument.isExplicit)
			{
				if(TypeFunction *target = getType<TypeFunction>(expectedType))
				{
					if(actualArgument.value && (isType<TypeFunction>(actualArgument.type) || isType<TypeFunctionSet>(actualArgument.type)))
					{
						if(FunctionValue functionValue = GetFunctionForType(ctx, actualArgument.value->source, actualArgument.value, target))
							continue;
					}
				}

				return ~0u;
			}

			// array -> class (unsized array)
			if(isType<TypeUnsizedArray>(expectedType) && isType<TypeArray>(actualType))
			{
				TypeUnsizedArray *lArray = getType<TypeUnsizedArray>(expectedType);
				TypeArray *rArray = getType<TypeArray>(actualType);

				if(lArray->subType == rArray->subType)
				{
					rating += 2;
					continue;
				}
			}

			// array -> auto[]
			if(expectedType == ctx.typeAutoArray && (isType<TypeArray>(actualType) || isType<TypeUnsizedArray>(actualType)))
			{
				rating += 5;
				continue;
			}

			// array[N] ref -> array[] -> array[] ref
			if(isType<TypeRef>(expectedType) && isType<TypeRef>(actualType))
			{
				TypeRef *lRef = getType<TypeRef>(expectedType);
				TypeRef *rRef = getType<TypeRef>(actualType);

				if(isType<TypeUnsizedArray>(lRef->subType) && isType<TypeArray>(rRef->subType))
				{
					TypeUnsizedArray *lArray = getType<TypeUnsizedArray>(lRef->subType);
					TypeArray *rArray = getType<TypeArray>(rRef->subType);

					if(lArray->subType == rArray->subType)
					{
						rating += 10;
						continue;
					}
				}
			}

			// derived ref -> base ref
			// base ref -> derived ref
			if(isType<TypeRef>(expectedType) && isType<TypeRef>(actualType))
			{
				TypeRef *lRef = getType<TypeRef>(expectedType);
				TypeRef *rRef = getType<TypeRef>(actualType);

				if(isType<TypeClass>(lRef->subType) && isType<TypeClass>(rRef->subType))
				{
					TypeClass *lClass = getType<TypeClass>(lRef->subType);
					TypeClass *rClass = getType<TypeClass>(rRef->subType);

					if(IsDerivedFrom(rClass, lClass))
					{
						rating += 5;
						continue;
					}

					if(IsDerivedFrom(lClass, rClass))
					{
						rating += 10;
						continue;
					}
				}
			}

			if(isType<TypeClass>(expectedType) && isType<TypeClass>(actualType))
			{
				TypeClass *lClass = getType<TypeClass>(expectedType);
				TypeClass *rClass = getType<TypeClass>(actualType);

				if(IsDerivedFrom(rClass, lClass))
				{
					rating += 5;
					continue;
				}
			}

			if(isType<TypeFunction>(expectedType))
			{
				TypeFunction *lFunction = getType<TypeFunction>(expectedType);

				if(actualArgument.value && (isType<TypeFunction>(actualArgument.type) || isType<TypeFunctionSet>(actualArgument.type)))
				{
					if(FunctionValue functionValue = GetFunctionForType(ctx, actualArgument.value->source, actualArgument.value, lFunction))
						continue;
				}
				
				return ~0u;
			}

			// type -> type ref
			if(isType<TypeRef>(expectedType))
			{
				TypeRef *lRef = getType<TypeRef>(expectedType);

				if(lRef->subType == actualType)
				{
					rating += 5;
					continue;
				}
			}

			// type ref -> auto ref
			if(expectedType == ctx.typeAutoRef && isType<TypeRef>(actualType))
			{
				rating += 5;
				continue;
			}

			// type -> type ref -> auto ref
			if(expectedType == ctx.typeAutoRef)
			{
				rating += 10;
				continue;
			}

			// numeric -> numeric
			if(ctx.IsNumericType(expectedType) && ctx.IsNumericType(actualType))
			{
				rating += 1;
				continue;
			}

			return ~0u;
		}
	}

	return rating;
}

TypeBase* MatchGenericType(ExpressionContext &ctx, SynBase *source, TypeBase *matchType, TypeBase *argType, IntrusiveList<MatchData> &aliases, bool strict)
{
	if(!matchType->isGeneric)
	{
		if(argType->isGeneric)
		{
			IntrusiveList<MatchData> subAliases;

			if(TypeBase *improved = MatchGenericType(ctx, source, argType, matchType, subAliases, true))
				argType = improved;
		}

		if(matchType == argType)
			return argType;

		if(strict)
			return NULL;

		return matchType;
	}

	// 'generic' match with 'type' results with 'type'
	if(isType<TypeGeneric>(matchType))
	{
		if(!strict)
		{
			// 'generic' match with 'type[N]' results with 'type[]'
			if(TypeArray *rhs = getType<TypeArray>(argType))
				argType = ctx.GetUnsizedArrayType(rhs->subType);
		}

		return argType;
	}

	if(TypeGenericAlias *lhs = getType<TypeGenericAlias>(matchType))
	{
		if(!strict)
		{
			// 'generic' match with 'type[N]' results with 'type[]'
			if(TypeArray *rhs = getType<TypeArray>(argType))
				argType = ctx.GetUnsizedArrayType(rhs->subType);
		}

		for(MatchData *curr = aliases.head; curr; curr = curr->next)
		{
			if(curr->name == lhs->baseName)
			{
				if(strict)
				{
					if(curr->type != argType)
						return NULL;
				}

				return curr->type;
			}
		}

		aliases.push_back(new (ctx.get<MatchData>()) MatchData(lhs->baseName, argType));

		return argType;
	}

	if(TypeRef *lhs = getType<TypeRef>(matchType))
	{
		// 'generic ref' match with 'type ref' results with 'type ref'
		if(TypeRef *rhs = getType<TypeRef>(argType))
		{
			if(TypeBase *match = MatchGenericType(ctx, source, lhs->subType, rhs->subType, aliases, true))
				return ctx.GetReferenceType(match);

			return NULL;
		}

		if(strict)
			return NULL;

		// 'generic ref' match with 'type' results with 'type ref'
		if(TypeBase *match = MatchGenericType(ctx, source, lhs->subType, argType, aliases, true))
			return ctx.GetReferenceType(match);

		return NULL;
	}

	if(TypeArray *lhs = getType<TypeArray>(matchType))
	{
		// Only match with arrays of the same size
		if(TypeArray *rhs = getType<TypeArray>(argType))
		{
			if(lhs->length == rhs->length)
			{
				if(TypeBase *match = MatchGenericType(ctx, source, lhs->subType, rhs->subType, aliases, true))
					return ctx.GetArrayType(match, lhs->length);

				return NULL;
			}
		}

		return NULL;
	}

	if(TypeUnsizedArray *lhs = getType<TypeUnsizedArray>(matchType))
	{
		// 'generic[]' match with 'type[]' results with 'type[]'
		if(TypeUnsizedArray *rhs = getType<TypeUnsizedArray>(argType))
		{
			if(TypeBase *match = MatchGenericType(ctx, source, lhs->subType, rhs->subType, aliases, true))
				return ctx.GetUnsizedArrayType(match);

			return NULL;
		}

		if(strict)
			return NULL;

		// 'generic[]' match with 'type[N]' results with 'type[]'
		if(TypeArray *rhs = getType<TypeArray>(argType))
		{
			if(TypeBase *match = MatchGenericType(ctx, source, lhs->subType, rhs->subType, aliases, true))
				return ctx.GetUnsizedArrayType(match);
		}

		return NULL;
	}

	if(TypeFunction *lhs = getType<TypeFunction>(matchType))
	{
		// Only match with other function type
		if(TypeFunction *rhs = getType<TypeFunction>(argType))
		{
			TypeBase *returnType = MatchGenericType(ctx, source, lhs->returnType, rhs->returnType, aliases, true);

			if(!returnType)
				return NULL;

			IntrusiveList<TypeHandle> arguments;

			TypeHandle *lhsArg = lhs->arguments.head;
			TypeHandle *rhsArg = rhs->arguments.head;

			while(lhsArg && rhsArg)
			{
				TypeBase *argMatched = MatchGenericType(ctx, source, lhsArg->type, rhsArg->type, aliases, true);

				if(!argMatched)
					return NULL;

				arguments.push_back(new (ctx.get<TypeHandle>()) TypeHandle(argMatched));

				lhsArg = lhsArg->next;
				rhsArg = rhsArg->next;
			}

			// Different number of arguments
			if(lhsArg || rhsArg)
				return NULL;

			return ctx.GetFunctionType(source, returnType, arguments);
		}

		return NULL;
	}

	if(TypeGenericClass *lhs = getType<TypeGenericClass>(matchType))
	{
		// Match with a generic class instance
		if(TypeClass *rhs = getType<TypeClass>(argType))
		{
			if(lhs->proto != rhs->proto)
				return NULL;

			TypeHandle *lhsArg = lhs->generics.head;
			MatchData *rhsArg = rhs->generics.head;

			while(lhsArg && rhsArg)
			{
				TypeBase *argMatched = MatchGenericType(ctx, source, lhsArg->type, rhsArg->type, aliases, true);

				if(!argMatched)
					return NULL;

				lhsArg = lhsArg->next;
				rhsArg = rhsArg->next;
			}

			return argType;
		}

		return NULL;
	}

	if(TypeGenericClassProto *lhs = getType<TypeGenericClassProto>(matchType))
	{
		// Match with a generic class instance
		if(TypeClass *rhs = getType<TypeClass>(argType))
		{
			if(lhs != rhs->proto)
				return NULL;

			return argType;
		}

		return NULL;
	}

	Stop(ctx, source->pos, "ERROR: unknown generic type match");

	return NULL;
}

TypeBase* ResolveGenericTypeAliases(ExpressionContext &ctx, SynBase *source, TypeBase *type, IntrusiveList<MatchData> aliases)
{
	if(!type->isGeneric || aliases.empty())
		return type;

	// Replace with alias type if there is a match, otherwise leave as generic
	if(isType<TypeGeneric>(type))
		return type;

	// Replace with alias type if there is a match, otherwise leave as generic
	if(TypeGenericAlias *lhs = getType<TypeGenericAlias>(type))
	{
		for(MatchData *curr = aliases.head; curr; curr = curr->next)
		{
			if(curr->name == lhs->baseName)
				return curr->type;
		}

		return ctx.typeGeneric;
	}

	if(TypeRef *lhs = getType<TypeRef>(type))
		return ctx.GetReferenceType(ResolveGenericTypeAliases(ctx, source, lhs->subType, aliases));

	if(TypeArray *lhs = getType<TypeArray>(type))
		return ctx.GetArrayType(ResolveGenericTypeAliases(ctx, source, lhs->subType, aliases), lhs->length);

	if(TypeUnsizedArray *lhs = getType<TypeUnsizedArray>(type))
		return ctx.GetUnsizedArrayType(ResolveGenericTypeAliases(ctx, source, lhs->subType, aliases));

	if(TypeFunction *lhs = getType<TypeFunction>(type))
	{
		TypeBase *returnType = ResolveGenericTypeAliases(ctx, source, lhs->returnType, aliases);

		IntrusiveList<TypeHandle> arguments;

		for(TypeHandle *curr = lhs->arguments.head; curr; curr = curr->next)
			arguments.push_back(new (ctx.get<TypeHandle>()) TypeHandle(ResolveGenericTypeAliases(ctx, source, curr->type, aliases)));

		return ctx.GetFunctionType(source, returnType, arguments);
	}

	if(TypeGenericClass *lhs = getType<TypeGenericClass>(type))
	{
		bool isGeneric = false;
		IntrusiveList<TypeHandle> types;

		for(TypeHandle *curr = lhs->generics.head; curr; curr = curr->next)
		{
			TypeBase *resolvedType = ResolveGenericTypeAliases(ctx, source, curr->type, aliases);

			if(resolvedType == ctx.typeAuto)
				Stop(ctx, source->pos, "ERROR: 'auto' type cannot be used as template argument");

			isGeneric |= resolvedType->isGeneric;

			types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(resolvedType));
		}

		if(isGeneric)
			return ctx.GetGenericClassType(source, lhs->proto, types);

		return CreateGenericTypeInstance(ctx, source, lhs->proto, types);
	}

	Stop(ctx, source->pos, "ERROR: unknown generic type resolve");

	return NULL;
}

TypeBase* MatchArgumentType(ExpressionContext &ctx, SynBase *source, TypeBase *expectedType, TypeBase *actualType, ExprBase *actualValue, IntrusiveList<MatchData> &aliases)
{
	if(actualType->isGeneric)
	{
		if(TypeFunction *target = getType<TypeFunction>(expectedType))
		{
			if(FunctionValue bestOverload = GetFunctionForType(ctx, source, actualValue, target))
				actualType = bestOverload.function->type;
		}

		if(actualType->isGeneric)
			return NULL;
	}

	return MatchGenericType(ctx, source, expectedType, actualType, aliases, !actualValue);
}

TypeFunction* GetGenericFunctionInstanceType(ExpressionContext &ctx, SynBase *source, TypeBase *parentType, FunctionData *function, ArrayView<CallArgumentData> arguments, IntrusiveList<MatchData> &aliases)
{
	assert(function->arguments.size() == arguments.size());

	// Switch to original function scope
	ScopeData *scope = ctx.scope;

	ctx.SwitchToScopeAtPoint(NULL, function->scope, function->source);

	IntrusiveList<TypeHandle> types;

	jmp_buf prevErrorHandler;
	memcpy(&prevErrorHandler, &ctx.errorHandler, sizeof(jmp_buf));

	bool prevErrorHandlerNested = ctx.errorHandlerNested;
	ctx.errorHandlerNested = true;

	if(!setjmp(ctx.errorHandler))
	{
		if(SynFunctionDefinition *syntax = function->definition)
		{
			bool addedParentScope = RestoreParentTypeScope(ctx, source, parentType);

			// Create temporary scope with known arguments for reference in type expression
			ctx.PushTemporaryScope();

			unsigned pos = 0;

			for(SynFunctionArgument *argument = syntax->arguments.head; argument; argument = getType<SynFunctionArgument>(argument->next), pos++)
			{
				bool failed = false;
				TypeBase *expectedType = AnalyzeType(ctx, argument->type, true, &failed);

				if(failed)
					break;

				CallArgumentData &actualArgument = arguments[pos];

				TypeBase *type = expectedType == ctx.typeAuto ? actualArgument.type : MatchArgumentType(ctx, argument, expectedType, actualArgument.type, actualArgument.value, aliases);

				if(!type)
					break;

				ctx.AddVariable(new (ctx.get<VariableData>()) VariableData(ctx.allocator, argument, ctx.scope, 0, type, argument->name, 0, ctx.uniqueVariableId++));

				types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(type));
			}

			ctx.PopScope(SCOPE_TEMPORARY);

			if(addedParentScope)
				ctx.PopScope(SCOPE_TYPE);
		}
		else
		{
			if(function->importModule)
				Stop(ctx, source->pos, "ERROR: imported generic function call is not supported");

			for(unsigned i = 0; i < function->arguments.size(); i++)
			{
				ArgumentData &funtionArgument = function->arguments[i];

				CallArgumentData &actualArgument = arguments[i];

				TypeBase *type = MatchArgumentType(ctx, funtionArgument.source, funtionArgument.type, actualArgument.type, actualArgument.value, aliases);

				if(!type)
				{
					// TODO: what about scope restore
					return NULL;
				}

				types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(type));
			}
		}
	}
	else
	{
		// Restore old scope
		ctx.SwitchToScopeAtPoint(function->source, scope, NULL);

		memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
		ctx.errorHandlerNested = prevErrorHandlerNested;

		longjmp(ctx.errorHandler, 1);
	}

	// Restore old scope
	ctx.SwitchToScopeAtPoint(function->source, scope, NULL);

	memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
	ctx.errorHandlerNested = prevErrorHandlerNested;

	if(types.size() != arguments.size())
		return NULL;

	// Check that all generics have been resolved
	for(MatchData *curr = function->generics.head; curr; curr = curr->next)
	{
		bool matched = false;

		for(MatchData *alias = aliases.head; alias; alias = alias->next)
		{
			if(curr->name == alias->name)
			{
				matched = true;
				break;
			}
		}

		if(!matched)
			return NULL;
	}

	return ctx.GetFunctionType(source, function->type->returnType, types);
}

void ReportOnFunctionSelectError(ExpressionContext &ctx, SynBase *source, char* errorBuf, unsigned errorBufSize, ArrayView<FunctionValue> functions)
{
	ArrayView<ArgumentData> arguments;
	ArrayView<unsigned> ratings;

	ReportOnFunctionSelectError(ctx, source, errorBuf, errorBufSize, InplaceStr(), functions, arguments, ratings, 0, false);
}

void ReportOnFunctionSelectError(ExpressionContext &ctx, SynBase *source, char* errorBuf, unsigned errorBufSize, InplaceStr functionName, ArrayView<FunctionValue> functions, ArrayView<ArgumentData> arguments, ArrayView<unsigned> ratings, unsigned bestRating, bool showInstanceInfo)
{
	assert(errorBuf);

	char *errPos = errorBuf;

	if(!functionName.empty())
	{
		errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "  %.*s(", FMT_ISTR(functionName));

		for(unsigned i = 0; i < arguments.size(); i++)
			errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "%s%.*s", i != 0 ? ", " : "", FMT_ISTR(arguments[i].type->name));

		errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), !functions.empty() ? ")\n" : ")");
	}

	if(!functions.empty())
		errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), bestRating == ~0u ? " the only available are:\n" : " candidates are:\n");

	for(unsigned i = 0; i < functions.size(); i++)
	{
		FunctionData *function = functions[i].function;

		if(!ratings.empty() && ratings[i] != bestRating)
			continue;

		errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "  %.*s %.*s", FMT_ISTR(function->type->returnType->name), FMT_ISTR(function->name.name));

		if(!function->generics.empty())
		{
			errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "<");

			for(unsigned k = 0; k < function->generics.size(); k++)
			{
				MatchData *match = function->generics[k];

				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "%s%.*s", k != 0 ? ", " : "", FMT_ISTR(match->type->name));
			}

			errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ">");
		}

		errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "(");

		for(unsigned k = 0; k < function->arguments.size(); k++)
		{
			ArgumentData &argument = function->arguments[k];

			errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "%s%s%.*s", k != 0 ? ", " : "", argument.isExplicit ? "explicit " : "", FMT_ISTR(argument.type->name));
		}

		if(ctx.IsGenericFunction(function) && showInstanceInfo)
		{
			TypeBase *parentType = NULL;

			if(functions[i].context->type == ctx.typeAutoRef)
			{
				assert(function->scope->ownerType && !function->scope->ownerType->isGeneric);
				parentType = function->scope->ownerType;
			}
			else if(function->scope->ownerType)
			{
				parentType = getType<TypeRef>(functions[i].context->type)->subType;
			}

			IntrusiveList<MatchData> aliases;
			SmallArray<CallArgumentData, 16> result(ctx.allocator);

			// Handle named argument order, default argument values and variadic functions
			if(!PrepareArgumentsForFunctionCall(ctx, source, function->arguments, arguments, result, NULL, false))
			{
				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ") (wasn't instanced here)");
			}
			else if(TypeFunction *instance = GetGenericFunctionInstanceType(ctx, source, parentType, function, result, aliases))
			{
				GetFunctionRating(ctx, function, instance, result);

				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ") instanced to\n    %.*s %.*s(", FMT_ISTR(function->type->returnType->name), FMT_ISTR(function->name.name));

				TypeHandle *curr = instance->arguments.head;

				for(unsigned k = 0; k < function->arguments.size(); k++)
				{
					ArgumentData &argument = function->arguments[k];

					errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "%s%s%.*s", k != 0 ? ", " : "", argument.isExplicit ? "explicit " : "", FMT_ISTR(curr->type->name));

					curr = curr->next;
				}

				if(!aliases.empty())
				{
					errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ") with [");

					for(MatchData *curr = aliases.head; curr; curr = curr->next)
						errPos += SafeSprintf(errPos, errorBufSize - unsigned(errPos - errorBuf), "%s%.*s = %.*s", curr != aliases.head ? ", " : "", FMT_ISTR(curr->name), FMT_ISTR(curr->type->name));

					errPos += SafeSprintf(errPos, errorBufSize - unsigned(errPos - errorBuf), "]");
				}
				else
				{
					errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ")");
				}
			}
			else
			{
				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ") (wasn't instanced here)");
			}
		}
		else
		{
			errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ")");
		}

		errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "\n");
	}

	AddErrorLocationInfo(FindModuleCodeWithSourceLocation(ctx, source->pos.begin), source->pos.begin, errorBuf, errorBufSize);
}

bool IsVirtualFunctionCall(ExpressionContext &ctx, FunctionData *function, TypeBase *type)
{
	assert(function);

	if(type == ctx.typeAutoRef)
		return true;

	if(TypeRef *refType = getType<TypeRef>(type))
	{
		if(TypeClass *classType = getType<TypeClass>(refType->subType))
		{
			if(classType->extendable || classType->baseClass != NULL)
			{
				unsigned hash = StringHashContinue(classType->nameHash, "::");

				InplaceStr constructor = GetTypeConstructorName(classType);

				hash = StringHashContinue(hash, constructor.begin, constructor.end);

				if(function->nameHash == hash || function->nameHash == StringHashContinue(hash, "$"))
					return false;

				return true;
			}
		}
	}

	return false;
}

FunctionValue SelectBestFunction(ExpressionContext &ctx, SynBase *source, ArrayView<FunctionValue> functions, IntrusiveList<TypeHandle> generics, ArrayView<ArgumentData> arguments, SmallArray<unsigned, 32> &ratings)
{
	ratings.resize(functions.size());

	SmallArray<TypeFunction*, 16> instanceTypes(ctx.allocator);

	SmallArray<CallArgumentData, 16> result(ctx.allocator);

	TypeClass *preferredParent = NULL;

	for(unsigned i = 0; i < functions.size(); i++)
	{
		FunctionValue value = functions[i];

		FunctionData *function = value.function;

		instanceTypes.push_back(function->type);

		if(TypeRef *typeRef = getType<TypeRef>(value.context->type))
		{
			if(TypeClass *typeClass = getType<TypeClass>(typeRef->subType))
			{
				if(typeClass->extendable)
				{
					if(!preferredParent)
						preferredParent = typeClass;
					else
						assert(preferredParent == typeClass);
				}
			}
		}

		if(!generics.empty())
		{
			MatchData *ca = function->generics.head;
			TypeHandle *cb = generics.head;

			bool sameGenerics = true;

			for(; ca && cb; ca = ca->next, cb = cb->next)
			{
				if(!ca->type->isGeneric && ca->type != cb->type)
				{
					sameGenerics = false;
					break;
				}
			}

			// Fail if provided explicit type list elements can't match
			if(!sameGenerics)
			{
				ratings[i] = ~0u;
				continue;
			}

			// Fail if provided explicit type list is larger than expected explicit type list
			if(cb)
			{
				ratings[i] = ~0u;
				continue;
			}
		}

		unsigned extraRating = 0;

		// Handle named argument order, default argument values and variadic functions
		if(!PrepareArgumentsForFunctionCall(ctx, source, function->arguments, arguments, result, &extraRating, false))
		{
			ratings[i] = ~0u;
			continue;
		}

		ratings[i] = GetFunctionRating(ctx, function, function->type, result);

		if(ratings[i] == ~0u)
			continue;

		ratings[i] += extraRating;

		if(ctx.IsGenericFunction(function))
		{
			TypeBase *parentType = NULL;

			if(value.context->type == ctx.typeAutoRef)
			{
				assert(function->scope->ownerType && !function->scope->ownerType->isGeneric);
				parentType = function->scope->ownerType;
			}
			else if(function->scope->ownerType)
			{
				parentType = getType<TypeRef>(value.context->type)->subType;
			}

			IntrusiveList<MatchData> aliases;

			{
				MatchData *currMatch = function->generics.head;
				TypeHandle *currGeneric = generics.head;

				for(; currMatch && currGeneric; currMatch = currMatch->next, currGeneric = currGeneric->next)
					aliases.push_back(new (ctx.get<MatchData>()) MatchData(currMatch->name, currGeneric->type));
			}

			TypeFunction *instance = GetGenericFunctionInstanceType(ctx, source, parentType, function, result, aliases);

			instanceTypes.back() = instance;

			if(!instance)
			{
				ratings[i] = ~0u;
				continue;
			}
			
			ratings[i] = GetFunctionRating(ctx, function, instance, result);

			if(ratings[i] == ~0u)
				continue;
		}
	}

	// For member functions, if there are multiple functions with the same rating and arguments, hide those which parent is derived further from preferred parent
	if(preferredParent)
	{
		for(unsigned i = 0; i < functions.size(); i++)
		{
			FunctionData *a = functions[i].function;

			for(unsigned k = 0; k < functions.size(); k++)
			{
				if(i == k)
					continue;

				if(ratings[k] == ~0u)
					continue;

				FunctionData *b = functions[k].function;

				if(ratings[i] == ratings[k] && instanceTypes[i]->arguments.size() == instanceTypes[k]->arguments.size())
				{
					bool sameArguments = true;

					for(unsigned arg = 0; arg < instanceTypes[i]->arguments.size(); arg++)
					{
						if(instanceTypes[i]->arguments[arg]->type != instanceTypes[k]->arguments[arg]->type)
							sameArguments = false;
					}

					if(sameArguments)
					{
						unsigned aDepth = GetDerivedFromDepth(preferredParent, getType<TypeClass>(a->scope->ownerType));
						unsigned bDepth = GetDerivedFromDepth(preferredParent, getType<TypeClass>(b->scope->ownerType));

						if (aDepth < bDepth)
							ratings[k] = ~0u;
					}
				}
			}
		}
	}

	// Select best generic and non-generic function
	unsigned bestRating = ~0u;
	FunctionValue bestFunction;

	unsigned bestGenericRating = ~0u;
	FunctionValue bestGenericFunction;

	for(unsigned i = 0; i < functions.size(); i++)
	{
		FunctionValue value = functions[i];

		FunctionData *function = value.function;

		if(ctx.IsGenericFunction(function))
		{
			if(ratings[i] < bestGenericRating)
			{
				bestGenericRating = ratings[i];
				bestGenericFunction = value;
			}
		}
		else
		{
			if(ratings[i] < bestRating)
			{
				bestRating = ratings[i];
				bestFunction = value;
			}
		}
	}

	// Use generic function only if it is better that selected
	if(bestGenericRating < bestRating)
	{
		bestRating = bestGenericRating;
		bestFunction = bestGenericFunction;
	}
	else
	{
		// Hide all generic functions from selection
		for(unsigned i = 0; i < functions.size(); i++)
		{
			FunctionData *function = functions[i].function;

			if(ctx.IsGenericFunction(function))
			{
				if(bestRating != ~0u && ratings[i] == bestRating && functions[i].context->type == ctx.typeAutoRef)
					CreateGenericFunctionInstance(ctx, source, functions[i], generics, arguments, true);

				ratings[i] = ~0u;
			}
		}
	}

	return bestFunction;
}

FunctionValue CreateGenericFunctionInstance(ExpressionContext &ctx, SynBase *source, FunctionValue proto, IntrusiveList<TypeHandle> generics, ArrayView<ArgumentData> arguments, bool standalone)
{
	FunctionData *function = proto.function;

	SmallArray<CallArgumentData, 16> result(ctx.allocator);

	if(!PrepareArgumentsForFunctionCall(ctx, source, function->arguments, arguments, result, NULL, false))
		assert(!"unexpected");

	TypeBase *parentType = NULL;

	if(proto.context->type == ctx.typeAutoRef)
	{
		assert(function->scope->ownerType && !function->scope->ownerType->isGeneric);
		parentType = function->scope->ownerType;
	}
	else if(function->scope->ownerType)
	{
		parentType = getType<TypeRef>(proto.context->type)->subType;
	}

	if(parentType && parentType->isGeneric)
		Stop(ctx, source->pos, "ERROR: generic type arguments required for type '%.*s'", FMT_ISTR(parentType->name));

	IntrusiveList<MatchData> aliases;

	{
		MatchData *currMatch = function->generics.head;
		TypeHandle *currGeneric = generics.head;

		for(; currMatch && currGeneric; currMatch = currMatch->next, currGeneric = currGeneric->next)
			aliases.push_back(new (ctx.get<MatchData>()) MatchData(currMatch->name, currGeneric->type));
	}

	TypeFunction *instance = GetGenericFunctionInstanceType(ctx, source, parentType, function, result, aliases);

	assert(instance);
	assert(!instance->isGeneric);

	// Search for an existing functions
	for(unsigned i = 0; i < function->instances.size(); i++)
	{
		FunctionData *data = function->instances[i];

		if(parentType != data->scope->ownerType)
			continue;

		if(!SameGenerics(data->generics, generics))
			continue;

		if(!SameArguments(data->type, instance))
			continue;

		ExprBase *context = proto.context;

		if(!data->scope->ownerType)
		{
			assert(isType<ExprNullptrLiteral>(context));

			context = CreateFunctionContextAccess(ctx, source, data);
		}

		return FunctionValue(source, function->instances[i], context);
	}

	// Switch to original function scope
	ScopeData *scope = ctx.scope;

	ctx.SwitchToScopeAtPoint(NULL, function->scope, function->source);

	ctx.instanceDepth++;

	if(ctx.instanceDepth > NULLC_MAX_GENERIC_INSTANCE_DEPTH)
		Stop(ctx, source->pos, "ERROR: reached maximum generic function instance depth (%d)", NULLC_MAX_GENERIC_INSTANCE_DEPTH);

	jmp_buf prevErrorHandler;
	memcpy(&prevErrorHandler, &ctx.errorHandler, sizeof(jmp_buf));

	bool prevErrorHandlerNested = ctx.errorHandlerNested;
	ctx.errorHandlerNested = true;

	ExprBase *expr = NULL;
	
	if(!setjmp(ctx.errorHandler))
	{
		if(SynFunctionDefinition *syntax = function->definition)
			expr = AnalyzeFunctionDefinition(ctx, syntax, instance, parentType, aliases, false, false, false);
		else if(SynShortFunctionDefinition *node = getType<SynShortFunctionDefinition>(function->declaration->source))
			expr = AnalyzeShortFunctionDefinition(ctx, node, instance);
		else
			Stop(ctx, source->pos, "ERROR: imported generic function call is not supported");
	}
	else
	{
		ctx.instanceDepth--;

		// Restore old scope
		ctx.SwitchToScopeAtPoint(function->source, scope, NULL);

		// Additional error info
		if(ctx.errorBuf)
		{
			char *errorCurr = ctx.errorBuf + strlen(ctx.errorBuf);

			errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "while instantiating generic function %.*s(", FMT_ISTR(function->name.name));

			for(TypeHandle *curr = function->type->arguments.head; curr; curr = curr->next)
				errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "%s%.*s", curr != function->type->arguments.head ? ", " : "", FMT_ISTR(curr->type->name));

			errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), ")");

			if(!arguments.empty())
			{
				errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "\n  using argument(s) (");

				for(unsigned i = 0; i < arguments.size(); i++)
					errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "%s%.*s", i != 0 ? ", " : "", FMT_ISTR(arguments[i].type->name));

				errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), ")");
			}

			if(!aliases.empty())
			{
				errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "\n  with [");

				for(MatchData *curr = aliases.head; curr; curr = curr->next)
					errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "%s%.*s = %.*s", curr != aliases.head ? ", " : "", FMT_ISTR(curr->name), FMT_ISTR(curr->type->name));

				errorCurr += SafeSprintf(errorCurr, ctx.errorBufSize - unsigned(errorCurr - ctx.errorBuf), "]");
			}

			AddErrorLocationInfo(FindModuleCodeWithSourceLocation(ctx, source->pos.begin), source->pos.begin, ctx.errorBuf, ctx.errorBufSize);

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);
		}

		memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
		ctx.errorHandlerNested = prevErrorHandlerNested;

		longjmp(ctx.errorHandler, 1);
	}

	ctx.instanceDepth--;

	// Restore old scope
	ctx.SwitchToScopeAtPoint(function->source, scope, NULL);

	memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
	ctx.errorHandlerNested = prevErrorHandlerNested;

	ExprFunctionDefinition *definition = getType<ExprFunctionDefinition>(expr);

	assert(definition);

	definition->function->proto = function;

	function->instances.push_back(definition->function);

	if(definition->contextVariable)
	{
		if(ExprGenericFunctionPrototype *genericProto = getType<ExprGenericFunctionPrototype>(function->declaration))
			genericProto->contextVariables.push_back(definition->contextVariable);
		else
			ctx.setup.push_back(definition->contextVariable);
	}

	if(standalone)
		return FunctionValue();

	ExprBase *context = proto.context;

	if(!definition->function->scope->ownerType)
	{
		assert(isType<ExprNullptrLiteral>(context));

		context = CreateFunctionContextAccess(ctx, source, definition->function);
	}

	return FunctionValue(source, definition->function, CreateSequence(ctx, source, definition, context));
}

void GetNodeFunctions(ExpressionContext &ctx, SynBase *source, ExprBase *function, SmallArray<FunctionValue, 32> &functions)
{
	if(ExprPassthrough *node = getType<ExprPassthrough>(function))
		function = node->value;

	if(ExprFunctionAccess *node = getType<ExprFunctionAccess>(function))
	{
		functions.push_back(FunctionValue(node->source, node->function, node->context));
	}
	else if(ExprFunctionDefinition *node = getType<ExprFunctionDefinition>(function))
	{
		functions.push_back(FunctionValue(node->source, node->function, CreateFunctionContextAccess(ctx, source, node->function)));
	}
	else if(ExprGenericFunctionPrototype *node = getType<ExprGenericFunctionPrototype>(function))
	{
		functions.push_back(FunctionValue(node->source, node->function, CreateFunctionContextAccess(ctx, source, node->function)));
	}
	else if(ExprFunctionOverloadSet *node = getType<ExprFunctionOverloadSet>(function))
	{
		for(FunctionHandle *arg = node->functions.head; arg; arg = arg->next)
		{
			ExprBase *context = node->context;

			if(!context)
				context = CreateFunctionContextAccess(ctx, source, arg->function);

			functions.push_back(FunctionValue(node->source, arg->function, context));
		}
	}
}

ExprBase* GetFunctionTable(ExpressionContext &ctx, SynBase *source, FunctionData *function)
{
	InplaceStr vtableName = GetFunctionTableName(ctx, function);

	if(VariableData **variable = ctx.variableMap.find(vtableName.hash()))
	{
		return new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, (*variable)->type, *variable);
	}
	
	TypeBase *type = ctx.GetUnsizedArrayType(ctx.typeFunctionID);

	unsigned offset = AllocateGlobalVariable(ctx, source, type->alignment, type->size);
	VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.globalScope, type->alignment, type, vtableName, offset, ctx.uniqueVariableId++);

	ctx.globalScope->variables.push_back(variable);
	ctx.globalScope->allVariables.push_back(variable);

	ctx.variables.push_back(variable);
	ctx.variableMap.insert(variable->nameHash, variable);

	ctx.vtables.push_back(variable);

	return new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, variable->type, variable);
}

ExprBase* CreateFunctionCall0(ExpressionContext &ctx, SynBase *source, InplaceStr name, bool allowFailure, bool allowInternal)
{
	SmallArray<ArgumentData, 1> arguments(ctx.allocator);

	return CreateFunctionCallByName(ctx, source, name, arguments, allowFailure, allowInternal);
}

ExprBase* CreateFunctionCall1(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, bool allowFailure, bool allowInternal)
{
	SmallArray<ArgumentData, 1> arguments(ctx.allocator);

	arguments.push_back(ArgumentData(arg0->source, false, InplaceStr(), arg0->type, arg0));

	return CreateFunctionCallByName(ctx, source, name, arguments, allowFailure, allowInternal);
}

ExprBase* CreateFunctionCall2(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, ExprBase *arg1, bool allowFailure, bool allowInternal)
{
	SmallArray<ArgumentData, 2> arguments(ctx.allocator);

	arguments.push_back(ArgumentData(arg0->source, false, InplaceStr(), arg0->type, arg0));
	arguments.push_back(ArgumentData(arg1->source, false, InplaceStr(), arg1->type, arg1));

	return CreateFunctionCallByName(ctx, source, name, arguments, allowFailure, allowInternal);
}

ExprBase* CreateFunctionCall3(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, ExprBase *arg1, ExprBase *arg2, bool allowFailure, bool allowInternal)
{
	SmallArray<ArgumentData, 3> arguments(ctx.allocator);

	arguments.push_back(ArgumentData(arg0->source, false, InplaceStr(), arg0->type, arg0));
	arguments.push_back(ArgumentData(arg1->source, false, InplaceStr(), arg1->type, arg1));
	arguments.push_back(ArgumentData(arg2->source, false, InplaceStr(), arg2->type, arg2));

	return CreateFunctionCallByName(ctx, source, name, arguments, allowFailure, allowInternal);
}

ExprBase* CreateFunctionCall4(ExpressionContext &ctx, SynBase *source, InplaceStr name, ExprBase *arg0, ExprBase *arg1, ExprBase *arg2, ExprBase *arg3, bool allowFailure, bool allowInternal)
{
	SmallArray<ArgumentData, 4> arguments(ctx.allocator);

	arguments.push_back(ArgumentData(arg0->source, false, InplaceStr(), arg0->type, arg0));
	arguments.push_back(ArgumentData(arg1->source, false, InplaceStr(), arg1->type, arg1));
	arguments.push_back(ArgumentData(arg2->source, false, InplaceStr(), arg2->type, arg2));
	arguments.push_back(ArgumentData(arg3->source, false, InplaceStr(), arg3->type, arg3));

	return CreateFunctionCallByName(ctx, source, name, arguments, allowFailure, allowInternal);
}

ExprBase* CreateFunctionCallByName(ExpressionContext &ctx, SynBase *source, InplaceStr name, ArrayView<ArgumentData> arguments, bool allowFailure, bool allowInternal)
{
	if(ExprBase *overloads = CreateVariableAccess(ctx, source, IntrusiveList<SynIdentifier>(), name, allowInternal))
	{
		if(ExprBase *result = CreateFunctionCall(ctx, source, overloads, arguments, allowFailure))
			return result;
	}

	if(!allowFailure)
	{
		ArrayView<FunctionValue> functions;
		ArrayView<unsigned> ratings;

		if(ctx.errorBuf && ctx.errorBufSize)
		{
			if(ctx.errorCount == 0)
			{
				ctx.errorPos = source->pos.begin;
				ctx.errorBufLocation = ctx.errorBuf;
			}

			SafeSprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), "ERROR: can't find function '%.*s' with following arguments:\n", FMT_ISTR(name));

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);

			ReportOnFunctionSelectError(ctx, source, ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), functions);

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);
		}

		assert(ctx.errorHandlerActive);

		longjmp(ctx.errorHandler, 1);
	}

	return NULL;
}

ExprBase* CreateFunctionCall(ExpressionContext &ctx, SynBase *source, ExprBase *value, ArrayView<ArgumentData> arguments, bool allowFailure)
{
	// Collect a set of available functions
	SmallArray<FunctionValue, 32> functions(ctx.allocator);

	GetNodeFunctions(ctx, source, value, functions);

	return CreateFunctionCallFinal(ctx, source, value, functions, IntrusiveList<TypeHandle>(), arguments, allowFailure);
}

ExprBase* CreateFunctionCall(ExpressionContext &ctx, SynBase *source, ExprBase *value, IntrusiveList<TypeHandle> generics, SynCallArgument *argumentHead, bool allowFailure)
{
	// Collect a set of available functions
	SmallArray<FunctionValue, 32> functions(ctx.allocator);

	GetNodeFunctions(ctx, source, value, functions);

	return CreateFunctionCallOverloaded(ctx, source, value, functions, generics, argumentHead, allowFailure);
}

ExprBase* CreateFunctionCallOverloaded(ExpressionContext &ctx, SynBase *source, ExprBase *value, ArrayView<FunctionValue> functions, IntrusiveList<TypeHandle> generics, SynCallArgument *argumentHead, bool allowFailure)
{
	// Analyze arguments
	SmallArray<ArgumentData, 16> arguments(ctx.allocator);
	
	for(SynCallArgument *el = argumentHead; el; el = getType<SynCallArgument>(el->next))
	{
		if(functions.empty() && !el->name.empty())
			Stop(ctx, source->pos, "ERROR: function argument names are unknown at this point");

		ExprBase *argument = NULL;

		if(SynShortFunctionDefinition *node = getType<SynShortFunctionDefinition>(el->value))
		{
			SmallArray<ExprBase*, 16> options(ctx.allocator);

			if(functions.empty())
			{
				if(ExprBase *option = AnalyzeShortFunctionDefinition(ctx, node, value->type, arguments, IntrusiveList<MatchData>()))
					options.push_back(option);
			}
			else
			{
				for(unsigned i = 0; i < functions.size(); i++)
				{
					IntrusiveList<MatchData> aliases;

					FunctionData *function = functions[i].function;

					TypeBase *parentType = function->scope->ownerType ? getType<TypeRef>(functions[i].context->type)->subType : NULL;

					if(TypeClass *classType = getType<TypeClass>(parentType))
					{
						for(MatchData *el = classType->generics.head; el; el = el->next)
							aliases.push_back(new (ctx.get<MatchData>()) MatchData(el->name, el->type));
					}

					if(ExprBase *option = AnalyzeShortFunctionDefinition(ctx, node, function->type, arguments, aliases))
					{
						bool found = false;

						for(unsigned k = 0; k < options.size(); k++)
						{
							if(options[k]->type == option->type)
								found = true;
						}

						if(!found)
							options.push_back(option);
					}
				}
			}

			if(options.empty())
				Stop(ctx, source->pos, "ERROR: cannot find function which accepts a function with %d argument(s) as an argument #%d", node->arguments.size(), arguments.size() + 1);

			if(options.size() == 1)
			{
				argument = options[0];
			}
			else
			{
				IntrusiveList<TypeHandle> types;
				IntrusiveList<FunctionHandle> overloads;

				for(unsigned i = 0; i < options.size(); i++)
				{
					ExprBase *option = options[i];

					assert(isType<ExprFunctionDefinition>(option) || isType<ExprGenericFunctionPrototype>(option));

					types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(option->type));

					if(ExprFunctionDefinition *function = getType<ExprFunctionDefinition>(option))
						overloads.push_back(new (ctx.get<FunctionHandle>()) FunctionHandle(function->function));
					else if(ExprGenericFunctionPrototype *function = getType<ExprGenericFunctionPrototype>(option))
						overloads.push_back(new (ctx.get<FunctionHandle>()) FunctionHandle(function->function));
				}

				TypeFunctionSet *type = ctx.GetFunctionSetType(types);

				argument = new (ctx.get<ExprFunctionOverloadSet>()) ExprFunctionOverloadSet(source, type, overloads, NULL);
			}
		}
		else
		{
			argument = AnalyzeExpression(ctx, el->value);
		}

		arguments.push_back(ArgumentData(el, false, el->name, argument->type, argument));
	}

	return CreateFunctionCallFinal(ctx, source, value, functions, generics, arguments, allowFailure);
}

ExprBase* CreateFunctionCallFinal(ExpressionContext &ctx, SynBase *source, ExprBase *value, ArrayView<FunctionValue> functions, IntrusiveList<TypeHandle> generics, ArrayView<ArgumentData> arguments, bool allowFailure)
{
	if(isType<ExprError>(value))
		return new (ctx.get<ExprError>()) ExprError(source, ctx.GetErrorType());

	for(unsigned i = 0; i < arguments.size(); i++)
	{
		if(isType<ExprError>(arguments[i].value))
			return new (ctx.get<ExprError>()) ExprError(source, ctx.GetErrorType());
	}

	TypeFunction *type = getType<TypeFunction>(value->type);

	for(unsigned i = 0; i < arguments.size(); i++)
		AssertResolvableTypeLiteral(ctx, source, arguments[i].value);

	IntrusiveList<ExprBase> actualArguments;

	if(!functions.empty())
	{
		SmallArray<unsigned, 32> ratings(ctx.allocator);

		FunctionValue bestOverload = SelectBestFunction(ctx, source, functions, generics, arguments, ratings);

		// Didn't find an appropriate function
		if(!bestOverload)
		{
			if(allowFailure)
				return NULL;

			// auto ref -> type cast
			if(isType<ExprTypeLiteral>(value) && arguments.size() == 1 && arguments[0].type == ctx.typeAutoRef && arguments[0].name.empty())
			{
				ExprBase *result = CreateCast(ctx, source, arguments[0].value, ((ExprTypeLiteral*)value)->value, true);

				// If this was a member function call, store to context
				if(!isType<ExprNullptrLiteral>(functions[0].context))
					return CreateAssignment(ctx, source, functions[0].context, result);

				return result;
			}

			if(ctx.errorBuf && ctx.errorBufSize)
			{
				if(ctx.errorCount == 0)
				{
					ctx.errorPos = source->pos.begin;
					ctx.errorBufLocation = ctx.errorBuf;
				}

				SafeSprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), "ERROR: can't find function '%.*s' with following arguments:\n", FMT_ISTR(functions[0].function->name.name));

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);

				ReportOnFunctionSelectError(ctx, source, ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), functions[0].function->name.name, functions, arguments, ratings, ~0u, true);

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);
			}

			assert(ctx.errorHandlerActive);

			longjmp(ctx.errorHandler, 1);
		}

		unsigned bestRating = ~0u;

		for(unsigned i = 0; i < functions.size(); i++)
		{
			if(functions[i].function == bestOverload.function)
				bestRating = ratings[i];
		}

		// Check if multiple functions share the same rating
		for(unsigned i = 0; i < functions.size(); i++)
		{
			if(functions[i].function != bestOverload.function && ratings[i] == bestRating)
			{
				if(ctx.errorBuf && ctx.errorBufSize)
				{
					if(ctx.errorCount == 0)
					{
						ctx.errorPos = source->pos.begin;
						ctx.errorBufLocation = ctx.errorBuf;
					}

					SafeSprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), "ERROR: ambiguity, there is more than one overloaded function available for the call:\n");

					ctx.errorBufLocation += strlen(ctx.errorBufLocation);

					ReportOnFunctionSelectError(ctx, source, ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), functions[0].function->name.name, functions, arguments, ratings, bestRating, true);

					ctx.errorBufLocation += strlen(ctx.errorBufLocation);
				}

				assert(ctx.errorHandlerActive);

				longjmp(ctx.errorHandler, 1);
			}
		}

		FunctionData *function = bestOverload.function;

		type = getType<TypeFunction>(function->type);

		if(ctx.IsGenericFunction(function))
		{
			bestOverload = CreateGenericFunctionInstance(ctx, source, bestOverload, generics, arguments, false);

			function = bestOverload.function;

			type = getType<TypeFunction>(function->type);
		}

		if(IsVirtualFunctionCall(ctx, function, bestOverload.context->type))
		{
			ExprBase *table = GetFunctionTable(ctx, source, bestOverload.function);

			value = CreateFunctionCall2(ctx, source, InplaceStr("__redirect"), bestOverload.context, table, false, true);

			value = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, function->type, value, EXPR_CAST_REINTERPRET);
		}
		else
		{
			value = new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(bestOverload.source, function->type, function, bestOverload.context);
		}

		SmallArray<CallArgumentData, 16> result(ctx.allocator);

		PrepareArgumentsForFunctionCall(ctx, source, function->arguments, arguments, result, NULL, true);

		for(unsigned i = 0; i < result.size(); i++)
			actualArguments.push_back(result[i].value);
	}
	else if(type)
	{
		SmallArray<ArgumentData, 8> functionArguments(ctx.allocator);

		for(TypeHandle *argType = type->arguments.head; argType; argType = argType->next)
			functionArguments.push_back(ArgumentData(NULL, false, InplaceStr(), argType->type, NULL));

		SmallArray<CallArgumentData, 16> result(ctx.allocator);

		if(!PrepareArgumentsForFunctionCall(ctx, source, functionArguments, arguments, result, NULL, true))
		{
			if(allowFailure)
				return NULL;

			if(ctx.errorBuf && ctx.errorBufSize)
			{
				if(ctx.errorCount == 0)
				{
					ctx.errorPos = source->pos.begin;
					ctx.errorBufLocation = ctx.errorBuf;
				}

				char *errorBuf = ctx.errorBufLocation;
				unsigned errorBufSize = ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf);

				char *errPos = ctx.errorBufLocation;

				if(arguments.size() != functionArguments.size())
					errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "ERROR: function expects %d argument(s), while %d are supplied\n", functionArguments.size(), arguments.size());
				else
					errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "ERROR: there is no conversion from specified arguments and the ones that function accepts\n");

				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "\tExpected: (");

				for(unsigned i = 0; i < functionArguments.size(); i++)
					errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "%s%.*s", i != 0 ? ", " : "", FMT_ISTR(functionArguments[i].type->name));

				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ")\n");
			
				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "\tProvided: (");

				for(unsigned i = 0; i < arguments.size(); i++)
					errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), "%s%.*s", i != 0 ? ", " : "", FMT_ISTR(arguments[i].type->name));

				errPos += SafeSprintf(errPos, errorBufSize - int(errPos - errorBuf), ")");

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);

				AddErrorLocationInfo(FindModuleCodeWithSourceLocation(ctx, source->pos.begin), source->pos.begin, ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf));

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);
			}

			assert(ctx.errorHandlerActive);

			longjmp(ctx.errorHandler, 1);
		}

		for(unsigned i = 0; i < result.size(); i++)
			actualArguments.push_back(result[i].value);
	}
	else if(isType<ExprTypeLiteral>(value) && arguments.size() == 1 && arguments[0].name.empty())
	{
		if(ExprTypeLiteral *typeLiteral = getType<ExprTypeLiteral>(value))
		{
			if(isType<TypeGenericClassProto>(typeLiteral->value))
				Stop(ctx, source->pos, "ERROR: generic type arguments in <> are not found after constructor name");
			else if(typeLiteral->value->isGeneric)
				Stop(ctx, source->pos, "ERROR: can't cast to a generic type");
		}

		// Function-style type casts
		return CreateCast(ctx, source, arguments[0].value, ((ExprTypeLiteral*)value)->value, true);
	}
	else
	{
		if(ExprTypeLiteral *typeLiteral = getType<ExprTypeLiteral>(value))
		{
			if(isType<TypeGenericClassProto>(typeLiteral->value))
				Stop(ctx, source->pos, "ERROR: generic type arguments in <> are not found after constructor name");
			else if(typeLiteral->value->isGeneric)
				Stop(ctx, source->pos, "ERROR: can't cast to a generic type");
		}

		// Call operator()
		if(ExprBase *overloads = CreateVariableAccess(ctx, source, IntrusiveList<SynIdentifier>(), InplaceStr("()"), false))
		{
			SmallArray<ArgumentData, 32> callArguments(ctx.allocator);
			callArguments.push_back(ArgumentData(value->source, false, InplaceStr(), value->type, value));

			for(unsigned i = 0; i < arguments.size(); i++)
				callArguments.push_back(arguments[i]);

			if(ExprBase *result = CreateFunctionCall(ctx, source, overloads, callArguments, false))
				return result;
		}
		else
		{
			Stop(ctx, source->pos, "ERROR: operator '()' accepting %d argument(s) is undefined for a class '%.*s'", arguments.size(), FMT_ISTR(value->type->name));
		}
	}

	assert(type);

	if(type->isGeneric)
		Stop(ctx, source->pos, "ERROR: generic function call is not supported");

	if(type->returnType == ctx.typeAuto)
		Stop(ctx, source->pos, "ERROR: function can't return auto");

	assert(actualArguments.size() == type->arguments.size());

	{
		ExprBase *actual = actualArguments.head;
		TypeHandle *expected = type->arguments.head;

		for(; actual && expected; actual = actual->next, expected = expected->next)
			assert(actual->type == expected->type);

		assert(actual == NULL);
		assert(expected == NULL);
	}

	return new (ctx.get<ExprFunctionCall>()) ExprFunctionCall(source, type->returnType, value, actualArguments);
}

ExprBase* CreateObjectAllocation(ExpressionContext &ctx, SynBase *source, TypeBase *type)
{
	ExprBase *size = new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, type->size);
	ExprBase *typeId = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.typeInt, new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, type), EXPR_CAST_REINTERPRET);

	ExprFunctionCall *alloc = getType<ExprFunctionCall>(CreateFunctionCall2(ctx, source, InplaceStr("__newS"), size, typeId, false, true));

	return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.GetReferenceType(type), alloc, EXPR_CAST_REINTERPRET);
}

ExprBase* CreateArrayAllocation(ExpressionContext &ctx, SynBase *source, TypeBase *type, ExprBase *count)
{
	ExprBase *size = new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, type->size);
	ExprBase *typeId = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.typeInt, new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, type), EXPR_CAST_REINTERPRET);

	count = CreateCast(ctx, source, count, ctx.typeInt, true);

	ExprFunctionCall *alloc = getType<ExprFunctionCall>(CreateFunctionCall3(ctx, source, InplaceStr("__newA"), size, count, typeId, false, true));

	if(type->size >= 64 * 1024)
		Stop(ctx, source->pos, "ERROR: array element size cannot exceed 65535 bytes");

	return new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.GetUnsizedArrayType(type), alloc, EXPR_CAST_REINTERPRET);
}

ExprBase* AnalyzeFunctionCall(ExpressionContext &ctx, SynFunctionCall *syntax)
{
	ExprBase *function = AnalyzeExpression(ctx, syntax->value);

	if(isType<ExprError>(function))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	IntrusiveList<TypeHandle> generics;

	for(SynBase *curr = syntax->aliases.head; curr; curr = curr->next)
	{
		TypeBase *type = AnalyzeType(ctx, curr);

		if(type == ctx.typeAuto)
			Stop(ctx, syntax->pos, "ERROR: explicit generic argument type can't be auto");

		if(type == ctx.typeVoid)
			Stop(ctx, syntax->pos, "ERROR: explicit generic argument cannot be a void type");

		generics.push_back(new (ctx.get<TypeHandle>()) TypeHandle(type));
	}

	if(ExprTypeLiteral *type = getType<ExprTypeLiteral>(function))
	{
		// Handle hasMember(x) expresion
		if(TypeMemberSet *memberSet = getType<TypeMemberSet>(type->value))
		{
			if(generics.empty() && syntax->arguments.size() == 1 && syntax->arguments.head->name.empty())
			{
				if(SynTypeSimple *name = getType<SynTypeSimple>(syntax->arguments.head->value))
				{
					if(name->path.empty())
					{
						for(VariableHandle *curr = memberSet->type->members.head; curr; curr = curr->next)
						{
							if(curr->variable->name == name->name)
								return new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(syntax, ctx.typeBool, true);
						}

						return new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(syntax, ctx.typeBool, false);
					}
				}
			}
		}

		ExprBase *regular = NULL;

		if(SynTypeSimple *node = getType<SynTypeSimple>(syntax->value))
		{
			regular = CreateVariableAccess(ctx, syntax->value, node->path, node->name, false);

			if(!regular && node->path.empty())
				regular = CreateVariableAccess(ctx, syntax->value, IntrusiveList<SynIdentifier>(), type->value->name, false);
		}
		else
		{
			regular = CreateVariableAccess(ctx, syntax->value, IntrusiveList<SynIdentifier>(), type->value->name, false);
		}

		if(regular)
		{
			// Collect a set of available functions
			SmallArray<FunctionValue, 32> functions(ctx.allocator);

			GetNodeFunctions(ctx, syntax, regular, functions);

			// If only constructors are available, do not call as a regular function
			bool hasReturnValue = false;

			for(unsigned i = 0; i < functions.size(); i++)
			{
				if(functions[i].function->type->returnType != ctx.typeVoid)
				{
					hasReturnValue = true;
					break;
				}
			}

			if(hasReturnValue)
			{
				ExprBase *call = CreateFunctionCallOverloaded(ctx, syntax, function, functions, generics, syntax->arguments.head, true);

				if(call)
					return call;
			}
		}

		if(!type->value->isGeneric)
		{
			VariableData *variable = AllocateTemporary(ctx, syntax, type->value);

			ExprBase *pointer = CreateGetAddress(ctx, syntax, CreateVariableAccess(ctx, syntax, variable, false));

			ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, variable, NULL);

			ExprBase *constructor = CreateConstructorAccess(ctx, syntax, type->value, syntax->arguments.empty(), pointer);

			if(!constructor && syntax->arguments.empty())
			{
				IntrusiveList<ExprBase> expressions;

				expressions.push_back(definition);
				expressions.push_back(CreateVariableAccess(ctx, syntax, variable, false));

				return new (ctx.get<ExprSequence>()) ExprSequence(syntax, type->value, expressions);
			}

			if(constructor)
			{
				// Collect a set of available functions
				SmallArray<FunctionValue, 32> functions(ctx.allocator);

				GetNodeFunctions(ctx, syntax, constructor, functions);

				ExprBase *call = CreateFunctionCallOverloaded(ctx, syntax, function, functions, generics, syntax->arguments.head, false);

				IntrusiveList<ExprBase> expressions;

				expressions.push_back(definition);
				expressions.push_back(call);
				expressions.push_back(CreateVariableAccess(ctx, syntax, variable, false));

				return new (ctx.get<ExprSequence>()) ExprSequence(syntax, type->value, expressions);
			}
		}
	}

	return CreateFunctionCall(ctx, syntax, function, generics, syntax->arguments.head, false);
}

ExprBase* AnalyzeNew(ExpressionContext &ctx, SynNew *syntax)
{
	TypeBase *type = AnalyzeType(ctx, syntax->type, false);

	// If there is no count and we have an array type that failed, take last extend as the size
	if(!type && !syntax->count && isType<SynArrayIndex>(syntax->type))
	{
		SynArrayIndex *arrayIndex = getType<SynArrayIndex>(syntax->type);

		if(arrayIndex->arguments.size() == 1 && arrayIndex->arguments.head->name.empty())
		{
			syntax->count = arrayIndex->arguments.head->value;
			syntax->type = arrayIndex->value;

			type = AnalyzeType(ctx, syntax->type, false);
		}
	}

	// If there are no arguments and we have a function type that failed, take the arguments list as constructor arguments
	if(!type && syntax->arguments.empty() && isType<SynTypeFunction>(syntax->type))
	{
		SynTypeFunction *functionType = getType<SynTypeFunction>(syntax->type);

		for(SynBase *curr = functionType->arguments.head; curr; curr = curr->next)
			syntax->arguments.push_back(new (ctx.get<SynCallArgument>()) SynCallArgument(curr->begin, curr->end, InplaceStr(), curr));

		syntax->type = new (ctx.get<SynTypeReference>()) SynTypeReference(functionType->begin, functionType->end, functionType->returnType);

		type = AnalyzeType(ctx, syntax->type, false);
	}

	// Report the original error
	if(!type)
		AnalyzeType(ctx, syntax->type);

	if(type->isGeneric)
		Stop(ctx, syntax->type->pos, "ERROR: generic type is not allowed");

	if(type == ctx.typeVoid || type == ctx.typeAuto)
		Stop(ctx, syntax->type->pos, "ERROR: can't allocate objects of type '%.*s'", FMT_ISTR(type->name));

	if(TypeClass *typeClass = getType<TypeClass>(type))
	{
		if(!typeClass->completed)
			Stop(ctx, syntax->pos, "ERROR: type '%.*s' is not fully defined", FMT_ISTR(type->name));
	}

	if(syntax->count)
	{
		assert(syntax->arguments.empty());
		assert(syntax->constructor.empty());

		ExprBase *count = AnalyzeExpression(ctx, syntax->count);

		if(isType<ExprError>(count))
			return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

		ExprBase *alloc = CreateArrayAllocation(ctx, syntax, type, count);

		if(HasDefautConstructor(ctx, syntax, type))
		{
			VariableData *variable = AllocateTemporary(ctx, syntax, alloc->type);

			ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, variable, CreateAssignment(ctx, syntax, CreateVariableAccess(ctx, syntax, variable, false), alloc));

			if(ExprBase *call = CreateDefaultConstructorCall(ctx, syntax, variable->type, CreateVariableAccess(ctx, syntax, variable, true)))
				return CreateSequence(ctx, syntax, definition, call, CreateVariableAccess(ctx, syntax, variable, true));
		}

		return alloc;
	}

	ExprBase *alloc = CreateObjectAllocation(ctx, syntax, type);

	// Call constructor
	TypeRef *allocType = getType<TypeRef>(alloc->type);

	TypeBase *parentType = allocType->subType;

	SmallArray<FunctionData*, 32> functions(ctx.allocator);

	if(GetTypeConstructorFunctions(ctx, parentType, syntax->arguments.empty(), functions))
	{
		VariableData *variable = AllocateTemporary(ctx, syntax, alloc->type);

		ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, variable, CreateAssignment(ctx, syntax, CreateVariableAccess(ctx, syntax, variable, false), alloc));

		ExprBase *overloads = CreateConstructorAccess(ctx, syntax, functions, CreateVariableAccess(ctx, syntax, variable, false));

		if(ExprBase *call = CreateFunctionCall(ctx, syntax, overloads, IntrusiveList<TypeHandle>(), syntax->arguments.head, syntax->arguments.empty()))
		{
			IntrusiveList<ExprBase> expressions;

			expressions.push_back(definition);
			expressions.push_back(call);
			expressions.push_back(CreateVariableAccess(ctx, syntax, variable, false));

			alloc = new (ctx.get<ExprSequence>()) ExprSequence(syntax, allocType, expressions);
		}
	}
	else if(syntax->arguments.size() == 1 && syntax->arguments.head->name.empty())
	{
		VariableData *variable = AllocateTemporary(ctx, syntax, alloc->type);

		ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, variable, CreateAssignment(ctx, syntax, CreateVariableAccess(ctx, syntax, variable, false), alloc));

		ExprBase *copy = CreateAssignment(ctx, syntax, new (ctx.get<ExprDereference>()) ExprDereference(syntax, parentType, CreateVariableAccess(ctx, syntax, variable, false)), AnalyzeExpression(ctx, syntax->arguments.head->value));

		IntrusiveList<ExprBase> expressions;

		expressions.push_back(definition);
		expressions.push_back(copy);
		expressions.push_back(CreateVariableAccess(ctx, syntax, variable, false));

		alloc = new (ctx.get<ExprSequence>()) ExprSequence(syntax, allocType, expressions);
	}
	else if(!syntax->arguments.empty())
	{
		Stop(ctx, syntax->pos, "ERROR: function '%.*s::%.*s' that accepts %d arguments is undefined", FMT_ISTR(parentType->name), FMT_ISTR(parentType->name), syntax->arguments.size());
	}

	// Handle custom constructor
	if(!syntax->constructor.empty())
	{
		VariableData *variable = AllocateTemporary(ctx, syntax, alloc->type);

		ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, variable, CreateAssignment(ctx, syntax, CreateVariableAccess(ctx, syntax, variable, false), alloc));

		// Create a member function with the constructor body
		InplaceStr name = GetTemporaryFunctionName(ctx);

		ExprBase *function = CreateFunctionDefinition(ctx, syntax, false, false, parentType, false, ctx.typeVoid, false, SynIdentifier(name), IntrusiveList<SynIdentifier>(), IntrusiveList<SynFunctionArgument>(), syntax->constructor, NULL, IntrusiveList<MatchData>());

		ExprFunctionDefinition *functionDefinition = getType<ExprFunctionDefinition>(function);

		// Call this member function
		SmallArray<FunctionValue, 32> functions(ctx.allocator);
		functions.push_back(FunctionValue(syntax, functionDefinition->function, CreateVariableAccess(ctx, syntax, variable, false)));

		SmallArray<ArgumentData, 32> arguments(ctx.allocator);

		ExprBase *call = CreateFunctionCallFinal(ctx, syntax, function, functions, IntrusiveList<TypeHandle>(), arguments, false);

		IntrusiveList<ExprBase> expressions;

		expressions.push_back(definition);
		expressions.push_back(call);
		expressions.push_back(CreateVariableAccess(ctx, syntax, variable, false));

		alloc = new (ctx.get<ExprSequence>()) ExprSequence(syntax, allocType, expressions);
	}

	return alloc;
}

ExprBase* AnalyzeReturn(ExpressionContext &ctx, SynReturn *syntax)
{
	ExprBase *result = syntax->value ? AnalyzeExpression(ctx, syntax->value) : new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid);

	if(isType<ExprError>(result))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	if(FunctionData *function = ctx.GetCurrentFunction())
	{
		TypeBase *returnType = function->type->returnType;

		// If return type is auto, set it to type that is being returned
		if(returnType == ctx.typeAuto)
		{
			if(result->type->isGeneric)
				AssertValueExpression(ctx, syntax, result);

			returnType = result->type;

			function->type = ctx.GetFunctionType(syntax, returnType, function->type->arguments);
		}

		if(returnType == ctx.typeVoid && result->type != ctx.typeVoid)
			Stop(ctx, syntax->pos, "ERROR: 'void' function returning a value");
		if(returnType != ctx.typeVoid && result->type == ctx.typeVoid)
			Stop(ctx, syntax->pos, "ERROR: function must return a value of type '%.*s'", FMT_ISTR(returnType->name));

		result = CreateCast(ctx, syntax, result, function->type->returnType, false);

		function->hasExplicitReturn = true;

		// TODO: checked return value

		return new (ctx.get<ExprReturn>()) ExprReturn(syntax, ctx.typeVoid, result, CreateFunctionCoroutineStateUpdate(ctx, syntax, function, 0), CreateFunctionUpvalueClose(ctx, syntax, function, ctx.scope));
	}

	if(isType<TypeFunction>(result->type))
		result = CreateCast(ctx, syntax, result, result->type, false);

	AssertValueExpression(ctx, result->source, result);

	if(!ctx.IsNumericType(result->type) && !isType<TypeEnum>(result->type))
		Stop(ctx, syntax->pos, "ERROR: global return cannot accept '%.*s'", FMT_ISTR(result->type->name));

	return new (ctx.get<ExprReturn>()) ExprReturn(syntax, ctx.typeVoid, result, NULL, NULL);
}

ExprBase* AnalyzeYield(ExpressionContext &ctx, SynYield *syntax)
{
	ExprBase *result = syntax->value ? AnalyzeExpression(ctx, syntax->value) : new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid);

	if(isType<ExprError>(result))
		return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());

	if(FunctionData *function = ctx.GetCurrentFunction())
	{
		if(!function->coroutine)
			Stop(ctx, syntax->pos, "ERROR: yield can only be used inside a coroutine");

		TypeBase *returnType = function->type->returnType;

		// If return type is auto, set it to type that is being returned
		if(returnType == ctx.typeAuto)
		{
			returnType = result->type;

			function->type = ctx.GetFunctionType(syntax, returnType, function->type->arguments);
		}

		if(returnType == ctx.typeVoid && result->type != ctx.typeVoid)
			Stop(ctx, syntax->pos, "ERROR: 'void' function returning a value");
		if(returnType != ctx.typeVoid && result->type == ctx.typeVoid)
			Stop(ctx, syntax->pos, "ERROR: function must return a value of type '%.*s'", FMT_ISTR(returnType->name));

		result = CreateCast(ctx, syntax, result, function->type->returnType, false);

		function->hasExplicitReturn = true;

		// TODO: checked return value

		unsigned yieldId = ++function->yieldCount;

		return new (ctx.get<ExprYield>()) ExprYield(syntax, ctx.typeVoid, result, CreateFunctionCoroutineStateUpdate(ctx, syntax, function, yieldId), CreateArgumentUpvalueClose(ctx, syntax, function), yieldId);
	}

	Stop(ctx, syntax->pos, "ERROR: global yield is not allowed");

	return NULL;
}

ExprBase* ResolveInitializerValue(ExpressionContext &ctx, SynBase *source, ExprBase *initializer)
{
	if(!initializer)
		Stop(ctx, source->pos, "ERROR: auto variable must be initialized in place of definition");

	if(initializer->type == ctx.typeVoid)
		Stop(ctx, source->pos, "ERROR: r-value type is 'void'");

	if(TypeFunction *target = getType<TypeFunction>(initializer->type))
	{
		if(FunctionValue bestOverload = GetFunctionForType(ctx, initializer->source, initializer, target))
			initializer = new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(bestOverload.source, bestOverload.function->type, bestOverload.function, bestOverload.context);
	}

	if(ExprFunctionOverloadSet *node = getType<ExprFunctionOverloadSet>(initializer))
	{
		if(node->functions.size() == 1)
		{
			FunctionData *function = node->functions.head->function;

			if(IsVirtualFunctionCall(ctx, function, node->context->type))
			{
				ExprBase *table = GetFunctionTable(ctx, source, function);

				initializer = CreateFunctionCall2(ctx, source, InplaceStr("__redirect_ptr"), node->context, table, false, true);

				initializer = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, function->type, initializer, EXPR_CAST_REINTERPRET);
			}
			else
			{
				initializer = new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(initializer->source, function->type, function, node->context);
			}
		}
		else
		{
			SmallArray<FunctionValue, 32> functions(ctx.allocator);

			GetNodeFunctions(ctx, initializer->source, initializer, functions);

			if(ctx.errorBuf && ctx.errorBufSize)
			{
				if(ctx.errorCount == 0)
				{
					ctx.errorPos = source->pos.begin;
					ctx.errorBufLocation = ctx.errorBuf;
				}

				SafeSprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), "ERROR: ambiguity, there is more than one overloaded function available:\n");

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);

				ReportOnFunctionSelectError(ctx, source, ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), functions);

				ctx.errorBufLocation += strlen(ctx.errorBufLocation);
			}

			assert(ctx.errorHandlerActive);

			longjmp(ctx.errorHandler, 1);
		}
	}

	if(isType<ExprGenericFunctionPrototype>(initializer))
		Stop(ctx, source->pos, "ERROR: cannot instance generic function, because target type is not known");

	return initializer;
}

ExprBase* AnalyzeVariableDefinition(ExpressionContext &ctx, SynVariableDefinition *syntax, unsigned alignment, TypeBase *type)
{
	if(syntax->name == InplaceStr("this"))
		Stop(ctx, syntax->pos, "ERROR: 'this' is a reserved keyword");

	InplaceStr fullName = GetVariableNameInScope(ctx, ctx.scope, syntax->name);

	CheckVariableConflict(ctx, syntax, fullName);

	VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, syntax, ctx.scope, 0, type, fullName, 0, ctx.uniqueVariableId++);

	if (IsLookupOnlyVariable(ctx, variable))
		variable->lookupOnly = true;

	ctx.AddVariable(variable);

	ExprBase *initializer = syntax->initializer ? AnalyzeExpression(ctx, syntax->initializer) : NULL;

	if(type == ctx.typeAuto)
	{
		initializer = ResolveInitializerValue(ctx, syntax, initializer);

		if(isType<ExprError>(initializer))
		{
			ctx.variableMap.remove(variable->nameHash, variable);

			return new (ctx.get<ExprError>()) ExprError(syntax, ctx.GetErrorType());
		}

		type = initializer->type;
	}
	else if(type->isGeneric && initializer)
	{
		IntrusiveList<MatchData> aliases;

		TypeBase *match = MatchGenericType(ctx, syntax, type, initializer->type, aliases, true);

		if(!match || match->isGeneric)
			Stop(ctx, syntax->pos, "ERROR: can't resolve generic type '%.*s' instance for '%.*s'", FMT_ISTR(initializer->type->name), FMT_ISTR(type->name));

		type = match;
	}
	else if(type->isGeneric)
	{
		Stop(ctx, syntax->pos, "ERROR: initializer is required to resolve generic type '%.*s'", FMT_ISTR(type->name));
	}

	if(alignment == 0)
	{
		TypeBase *parentType = ctx.scope->ownerType;

		if(parentType && parentType->alignment != 0 && parentType->alignment < type->alignment)
			alignment = parentType->alignment;
		else
			alignment = type->alignment;
	}

	// Fixup variable data not that the final type is known
	unsigned offset = AllocateVariableInScope(ctx, syntax, alignment, type);
	
	variable->type = type;
	variable->alignment = alignment;
	variable->offset = offset;

	if(TypeClass *classType = getType<TypeClass>(variable->type))
	{
		if(classType->hasFinalizer)
			Stop(ctx, syntax->pos, "ERROR: cannot create '%.*s' that implements 'finalize' on stack", FMT_ISTR(classType->name));
	}

	if(initializer)
	{
		ExprBase *access = CreateVariableAccess(ctx, syntax, variable, true);

		TypeArray *arrType = getType<TypeArray>(variable->type);

		// Single-level array might be set with a single element at the point of definition
		if(arrType && !isType<TypeArray>(initializer->type) && initializer->type != ctx.typeAutoArray)
		{
			initializer = CreateCast(ctx, syntax->initializer, initializer, arrType->subType, false);

			if(ExprVariableAccess *node = getType<ExprVariableAccess>(access))
				access = new (ctx.get<ExprGetAddress>()) ExprGetAddress(access->source, ctx.GetReferenceType(access->type), node->variable);
			else if(ExprDereference *node = getType<ExprDereference>(access))
				access = node->value;

			initializer = new (ctx.get<ExprArraySetup>()) ExprArraySetup(syntax->initializer, ctx.typeVoid, access, initializer);
		}
		else
		{
			initializer = CreateAssignment(ctx, syntax->initializer, access, initializer);
		}
	}
	else if(!variable->scope->ownerType)
	{
		if(HasDefautConstructor(ctx, syntax, variable->type))
		{
			ExprBase *access = CreateVariableAccess(ctx, syntax, variable, true);

			if(ExprBase *call = CreateDefaultConstructorCall(ctx, syntax, variable->type, CreateGetAddress(ctx, syntax, access)))
				initializer = call;
		}
	}

	return new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, variable, initializer);
}

ExprVariableDefinitions* AnalyzeVariableDefinitions(ExpressionContext &ctx, SynVariableDefinitions *syntax)
{
	unsigned alignment = syntax->align ? AnalyzeAlignment(ctx, syntax->align) : 0;

	TypeBase *parentType = ctx.scope->ownerType;

	if(parentType)
	{
		// Introduce 'this' variable into a temporary scope
		ctx.PushTemporaryScope();

		ctx.AddVariable(new (ctx.get<VariableData>()) VariableData(ctx.allocator, syntax, ctx.scope, 0, ctx.GetReferenceType(parentType), InplaceStr("this"), 0, ctx.uniqueVariableId++));
	}

	TypeBase *type = AnalyzeType(ctx, syntax->type);

	if(parentType)
		ctx.PopScope(SCOPE_TEMPORARY);

	IntrusiveList<ExprBase> definitions;

	for(SynVariableDefinition *el = syntax->definitions.head; el; el = getType<SynVariableDefinition>(el->next))
		definitions.push_back(AnalyzeVariableDefinition(ctx, el, alignment, type));

	return new (ctx.get<ExprVariableDefinitions>()) ExprVariableDefinitions(syntax, ctx.typeVoid, definitions);
}

TypeBase* CreateFunctionContextType(ExpressionContext &ctx, SynBase *source, InplaceStr functionName)
{
	InplaceStr functionContextName = GetFunctionContextTypeName(ctx, functionName, ctx.functions.size());

	TypeClass *contextClassType = new (ctx.get<TypeClass>()) TypeClass(functionContextName, source, ctx.scope, NULL, IntrusiveList<MatchData>(), false, NULL);

	ctx.AddType(contextClassType);

	ctx.PushScope(contextClassType);

	contextClassType->typeScope = ctx.scope;

	ctx.PopScope(SCOPE_TYPE);

	contextClassType->completed = true;

	return contextClassType;
}

ExprVariableDefinition* CreateFunctionContextArgument(ExpressionContext &ctx, SynBase *source, FunctionData *function)
{
	TypeBase *type = function->contextType;

	assert(!type->isGeneric);

	unsigned offset = AllocateArgumentInScope(ctx, source, 0, type);

	function->contextArgument = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, 0, type, InplaceStr(function->scope->ownerType ? "this" : "$context"), offset, ctx.uniqueVariableId++);

	ctx.AddVariable(function->contextArgument);

	return new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, function->contextArgument, NULL);
}

ExprVariableDefinition* CreateFunctionContextVariable(ExpressionContext &ctx, SynBase *source, FunctionData *function, FunctionData *prototype)
{
	if(function->scope->ownerType)
		return NULL;

	TypeRef *refType = getType<TypeRef>(function->contextType);

	assert(refType);

	TypeClass *classType = getType<TypeClass>(refType->subType);

	assert(classType);

	FinalizeAlignment(classType);

	if(classType->members.empty())
		return NULL;

	VariableData *context = NULL;

	if(prototype)
	{
		context = prototype->contextVariable;

		context->isAlloca = false;
		context->offset = AllocateVariableInScope(ctx, source, refType->alignment, refType);
	}
	else
	{
		// Create a variable holding a reference to a closure
		unsigned offset = AllocateVariableInScope(ctx, source, refType->alignment, refType);
		context = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, refType->alignment, refType, GetFunctionContextVariableName(ctx, function, ctx.GetFunctionIndex(function)), offset, ctx.uniqueVariableId++);

		ctx.AddVariable(context);
	}

	function->contextVariable = context;

	// Allocate closure
	ExprBase *alloc = CreateObjectAllocation(ctx, source, classType);

	// Initialize closure
	IntrusiveList<ExprBase> expressions;

	expressions.push_back(CreateAssignment(ctx, source, CreateVariableAccess(ctx, source, context, true), alloc));

	for(UpvalueData *upvalue = function->upvalues.head; upvalue; upvalue = upvalue->next)
	{
		ExprBase *target = new (ctx.get<ExprMemberAccess>()) ExprMemberAccess(source, ctx.GetReferenceType(upvalue->target->type), CreateVariableAccess(ctx, source, context, true), upvalue->target);

		target = new (ctx.get<ExprDereference>()) ExprDereference(source, upvalue->target->type, target);

		// Save variable address to current target value
		ExprBase *value = CreateVariableAccess(ctx, source, upvalue->variable, false);

		expressions.push_back(CreateAssignment(ctx, source, target, CreateGetAddress(ctx, source, value)));

		// Link to the current head of the upvalue list
		ExprBase *nextUpvalue = new (ctx.get<ExprMemberAccess>()) ExprMemberAccess(source, ctx.GetReferenceType(upvalue->nextUpvalue->type), CreateVariableAccess(ctx, source, context, true), upvalue->nextUpvalue);

		nextUpvalue = new (ctx.get<ExprDereference>()) ExprDereference(source, upvalue->nextUpvalue->type, nextUpvalue);

		expressions.push_back(CreateAssignment(ctx, source, nextUpvalue, GetFunctionUpvalue(ctx, source, upvalue->variable)));

		// Update current head of the upvalue list to our upvalue
		ExprBase *newHead = new (ctx.get<ExprMemberAccess>()) ExprMemberAccess(source, ctx.GetReferenceType(upvalue->target->type), CreateVariableAccess(ctx, source, context, true), upvalue->target);

		newHead = new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.GetReferenceType(ctx.typeVoid), newHead, EXPR_CAST_REINTERPRET);

		expressions.push_back(CreateAssignment(ctx, source, GetFunctionUpvalue(ctx, source, upvalue->variable), newHead));
	}

	ExprBase *initializer = new (ctx.get<ExprBlock>()) ExprBlock(source, ctx.typeVoid, expressions, NULL);

	if(prototype)
	{
		ExprFunctionDefinition *declaration = getType<ExprFunctionDefinition>(prototype->declaration);

		assert(declaration);

		declaration->contextVariable->initializer = initializer;
		return NULL;
	}

	return new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, context, initializer);
}

bool RestoreParentTypeScope(ExpressionContext &ctx, SynBase *source, TypeBase *parentType)
{
	if(parentType && ctx.scope->ownerType != parentType)
	{
		ctx.PushScope(parentType);

		if(TypeClass *classType = getType<TypeClass>(parentType))
		{
			for(MatchData *el = classType->generics.head; el; el = el->next)
				ctx.AddAlias(new (ctx.get<AliasData>()) AliasData(source, ctx.scope, el->type, el->name, ctx.uniqueAliasId++));

			for(MatchData *el = classType->aliases.head; el; el = el->next)
				ctx.AddAlias(new (ctx.get<AliasData>()) AliasData(source, ctx.scope, el->type, el->name, ctx.uniqueAliasId++));

			for(VariableHandle *el = classType->members.head; el; el = el->next)
				ctx.AddVariable(el->variable);
		}
		else if(TypeGenericClassProto *genericProto = getType<TypeGenericClassProto>(parentType))
		{
			SynClassDefinition *definition = genericProto->definition;

			for(SynIdentifier *curr = definition->aliases.head; curr; curr = getType<SynIdentifier>(curr->next))
				ctx.AddAlias(new (ctx.get<AliasData>()) AliasData(source, ctx.scope, ctx.GetGenericAliasType(curr->name), curr->name, ctx.uniqueAliasId++));
		}

		return true;
	}

	return false;
}

void CreateFunctionArgumentVariables(ExpressionContext &ctx, SynBase *source, FunctionData *function, ArrayView<ArgumentData> arguments, IntrusiveList<ExprVariableDefinition> &variables)
{
	for(unsigned i = 0; i < arguments.size(); i++)
	{
		ArgumentData &argument = arguments[i];

		assert(!argument.type->isGeneric);

		CheckVariableConflict(ctx, argument.source, argument.name);

		unsigned offset = AllocateArgumentInScope(ctx, source, 4, argument.type);
		VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, argument.source, ctx.scope, 0, argument.type, argument.name, offset, ctx.uniqueVariableId++);

		if(TypeClass *classType = getType<TypeClass>(variable->type))
		{
			if(classType->hasFinalizer)
				Stop(ctx, argument.source->pos, "ERROR: cannot create '%.*s' that implements 'finalize' on stack", FMT_ISTR(classType->name));
		}

		ctx.AddVariable(variable);

		variables.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(argument.source, ctx.typeVoid, variable, NULL));

		function->argumentVariables.push_back(new (ctx.get<VariableHandle>()) VariableHandle(variable));
	}
}

ExprBase* AnalyzeFunctionDefinition(ExpressionContext &ctx, SynFunctionDefinition *syntax, TypeFunction *instance, TypeBase *instanceParent, IntrusiveList<MatchData> matches, bool createAccess, bool hideFunction, bool checkParent)
{
	TypeBase *parentType = NULL;

	if(instanceParent)
	{
		parentType = instanceParent;
	}
	else if(syntax->parentType)
	{
		parentType = AnalyzeType(ctx, syntax->parentType);

		if(checkParent)
		{
			if(TypeBase *currentType = ctx.GetCurrentType())
			{
				if(parentType == currentType)
					Stop(ctx, syntax->parentType->pos, "ERROR: class name repeated inside the definition of class");

				Stop(ctx, syntax->pos, "ERROR: cannot define class '%.*s' function inside the scope of class '%.*s'", FMT_ISTR(parentType->name), FMT_ISTR(currentType->name));
			}
		}
	}

	TypeBase *returnType = AnalyzeType(ctx, syntax->returnType);

	if(returnType->isGeneric)
		Stop(ctx, syntax->pos, "ERROR: return type can't be generic");

	ExprBase *value = CreateFunctionDefinition(ctx, syntax, syntax->prototype, syntax->coroutine, parentType, syntax->accessor, returnType, syntax->isOperator, syntax->name, syntax->aliases, syntax->arguments, syntax->expressions, instance, matches);

	if(ExprFunctionDefinition *definition = getType<ExprFunctionDefinition>(value))
	{
		if(definition->function->scope->ownerType)
			return value;

		if(createAccess)
			return CreateFunctionPointer(ctx, syntax, definition, hideFunction);
	}

	return value;
}

void CheckOperatorName(ExpressionContext &ctx, SynBase *source, InplaceStr name, ArrayView<ArgumentData> argData)
{
	if(name == InplaceStr("~") || name == InplaceStr("!"))
	{
		if(argData.size() != 1)
			Stop(ctx, source->pos, "ERROR: operator '%.*s' definition must accept exactly one argument", FMT_ISTR(name));
	}
	else if(name == InplaceStr("+") || name == InplaceStr("-"))
	{
		if(argData.size() != 1 && argData.size() != 2)
			Stop(ctx, source->pos, "ERROR: operator '%.*s' definition must accept one or two arguments", FMT_ISTR(name));
	}
	else if(name == InplaceStr("&&") || name == InplaceStr("||"))
	{
		// Two arguments with the second argument being special
		if(argData.size() != 2 || !isType<TypeFunction>(argData[1].type) || getType<TypeFunction>(argData[1].type)->arguments.size() != 0)
			Stop(ctx, source->pos, "ERROR: operator '%.*s' definition must accept a function returning desired type as the second argument", FMT_ISTR(name));
	}
	else if(name != InplaceStr("()") && name != InplaceStr("[]"))
	{
		if(argData.size() != 2)
			Stop(ctx, source->pos, "ERROR: operator '%.*s' definition must accept exactly two arguments", FMT_ISTR(name));
	}
}

void AnalyzeFunctionArguments(ExpressionContext &ctx, IntrusiveList<SynFunctionArgument> arguments, TypeBase *parentType, TypeFunction *instance, SmallArray<ArgumentData, 8> &argData)
{
	TypeHandle *instanceArg = instance ? instance->arguments.head : NULL;

	bool hadGenericArgument = parentType ? parentType->isGeneric : false;

	for(SynFunctionArgument *argument = arguments.head; argument; argument = getType<SynFunctionArgument>(argument->next))
	{
		ExprBase *initializer = argument->initializer ? AnalyzeExpression(ctx, argument->initializer) : NULL;

		TypeBase *type = NULL;

		if(instance)
		{
			type = instanceArg->type;

			instanceArg = instanceArg->next;
		}
		else
		{
			// Create temporary scope with known arguments for reference in type expression
			ctx.PushTemporaryScope();

			unsigned pos = 0;

			for(SynFunctionArgument *prevArg = arguments.head; prevArg && prevArg != argument; prevArg = getType<SynFunctionArgument>(prevArg->next))
			{
				ArgumentData &data = argData[pos++];

				ctx.AddVariable(new (ctx.get<VariableData>()) VariableData(ctx.allocator, prevArg, ctx.scope, 0, data.type, data.name, 0, ctx.uniqueVariableId++));
			}

			bool failed = false;
			type = AnalyzeType(ctx, argument->type, true, hadGenericArgument ? &failed : NULL);

			if(type == ctx.typeAuto)
			{
				if(!initializer)
					Stop(ctx, argument->type->pos, "ERROR: function argument cannot be an auto type");

				initializer = ResolveInitializerValue(ctx, argument, initializer);

				type = initializer->type;
			}
			else if(initializer)
			{
				// Just a test
				if(!type->isGeneric)
					CreateCast(ctx, argument->type, initializer, type, true);
			}

			if(type == ctx.typeVoid)
				Stop(ctx, argument->type->pos, "ERROR: function argument cannot be a void type");

			hadGenericArgument |= type->isGeneric;

			// Remove temporary scope
			ctx.PopScope(SCOPE_TEMPORARY);
		}

		argData.push_back(ArgumentData(argument, argument->isExplicit, argument->name, type, initializer));
	}
}

ExprBase* CreateFunctionDefinition(ExpressionContext &ctx, SynBase *source, bool prototype, bool coroutine, TypeBase *parentType, bool accessor, TypeBase *returnType, bool isOperator, SynIdentifier name, IntrusiveList<SynIdentifier> aliases, IntrusiveList<SynFunctionArgument> arguments, IntrusiveList<SynBase> expressions, TypeFunction *instance, IntrusiveList<MatchData> matches)
{
	bool addedParentScope = RestoreParentTypeScope(ctx, source, parentType);

	if(ctx.scope->ownerType && !parentType)
		parentType = ctx.scope->ownerType;

	if(parentType && coroutine)
		Stop(ctx, source->pos, "ERROR: coroutine cannot be a member function");

	IntrusiveList<MatchData> generics;

	for(SynIdentifier *curr = aliases.head; curr; curr = getType<SynIdentifier>(curr->next))
	{
		if(ctx.typeMap.find(curr->name.hash()))
			Stop(ctx, curr->pos, "ERROR: there is already a type with the same name");

		for(SynIdentifier *prev = aliases.head; prev && prev != curr; prev = getType<SynIdentifier>(prev->next))
		{
			if(prev->name == curr->name)
				Stop(ctx, curr->pos, "ERROR: there is already an alias with the same name");
		}

		TypeBase *target = NULL;

		for(MatchData *match = matches.head; match; match = match->next)
		{
			if(curr->name == match->name)
			{
				target = match->type;
				break;
			}
		}

		if(!target)
			target = ctx.GetGenericAliasType(curr->name);

		generics.push_back(new (ctx.get<MatchData>()) MatchData(curr->name, target));
	}

	SmallArray<ArgumentData, 8> argData(ctx.allocator);

	AnalyzeFunctionArguments(ctx, arguments, parentType, instance, argData);

	// Check required operator properties
	if(isOperator)
		CheckOperatorName(ctx, source, name.name, argData);

	InplaceStr functionName = GetFunctionName(ctx, ctx.scope, parentType, name.name, isOperator, accessor);

	TypeBase *contextRefType = NULL;

	if(parentType)
		contextRefType = ctx.GetReferenceType(parentType);
	else if(!coroutine && ctx.GetFunctionOwner(ctx.scope) == NULL)
		contextRefType = ctx.GetReferenceType(ctx.typeVoid);
	else
		contextRefType = ctx.GetReferenceType(CreateFunctionContextType(ctx, source, functionName));

	TypeFunction *functionType = ctx.GetFunctionType(source, returnType, argData);

	if(instance)
		assert(functionType == instance);

	if(VariableData **variable = ctx.variableMap.find(functionName.hash()))
	{
		if((*variable)->scope == ctx.scope)
			Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken for a variable in current scope", FMT_ISTR(name.name));
	}

	if(TypeClass *classType = getType<TypeClass>(parentType))
	{
		if(name.name == InplaceStr("finalize"))
			classType->hasFinalizer = true;
	}

	FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, source, ctx.scope, coroutine, accessor, isOperator, functionType, contextRefType, SynIdentifier(name, functionName), generics, ctx.uniqueFunctionId++);

	function->aliases = matches;

	// Fill in argument data
	for(unsigned i = 0; i < argData.size(); i++)
		function->arguments.push_back(argData[i]);

	FunctionData *implementedPrototype = NULL;

	// If the type is known, implement the prototype immediately
	if(functionType->returnType != ctx.typeAuto)
	{
		if(FunctionData *functionPrototype = ImplementPrototype(ctx, function))
		{
			if(prototype)
				Stop(ctx, source->pos, "ERROR: function is already defined");

			function->contextType = functionPrototype->contextType;

			implementedPrototype = functionPrototype;
		}
	}

	CheckFunctionConflict(ctx, source, function->name.name);

	ctx.AddFunction(function);

	if(ctx.IsGenericFunction(function))
	{
		assert(!instance);

		if(prototype)
			Stop(ctx, source->pos, "ERROR: generic function cannot be forward-declared");

		if(addedParentScope)
			ctx.PopScope(SCOPE_TYPE);

		assert(isType<SynFunctionDefinition>(source));

		function->definition = getType<SynFunctionDefinition>(source);
		function->declaration = new (ctx.get<ExprGenericFunctionPrototype>()) ExprGenericFunctionPrototype(source, function->type, function);

		function->contextType = ctx.GetReferenceType(ctx.typeVoid);

		return function->declaration;
	}

	// Operator overloads can't be called recursively and become available at the end of the definition
	if (isOperator)
		ctx.functionMap.remove(function->nameHash, function);

	ctx.PushScope(function);

	function->functionScope = ctx.scope;

	for(MatchData *curr = function->aliases.head; curr; curr = curr->next)
		ctx.AddAlias(new (ctx.get<AliasData>()) AliasData(source, ctx.scope, curr->type, curr->name, ctx.uniqueAliasId++));

	IntrusiveList<ExprVariableDefinition> variables;

	CreateFunctionArgumentVariables(ctx, source, function, argData, variables);

	ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, source, function);

	function->argumentsSize = function->functionScope->dataSize;

	ExprBase *coroutineStateRead = NULL;

	IntrusiveList<ExprBase> code;

	if(prototype)
	{
		if(function->type->returnType == ctx.typeAuto)
			Stop(ctx, source->pos, "ERROR: function prototype with unresolved return type");

		function->isPrototype = true;
	}
	else
	{
		if(function->coroutine)
		{
			unsigned offset = AllocateVariableInScope(ctx, source, ctx.typeInt->alignment, ctx.typeInt);
			function->coroutineJumpOffset = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, 0, ctx.typeInt, InplaceStr("$jmpOffset"), offset, ctx.uniqueVariableId++);

			if (IsLookupOnlyVariable(ctx, function->coroutineJumpOffset))
				function->coroutineJumpOffset->lookupOnly = true;

			ctx.AddVariable(function->coroutineJumpOffset);

			AddFunctionCoroutineVariable(ctx, source, function, function->coroutineJumpOffset);

			coroutineStateRead = CreateVariableAccess(ctx, source, function->coroutineJumpOffset, true);
		}

		// If this is a custom default constructor, add a prolog
		if(TypeClass *classType = getType<TypeClass>(function->scope->ownerType))
		{
			if(GetTypeConstructorName(classType) == name.name)
				CreateDefaultConstructorCode(ctx, source, classType, code);
		}

		for(SynBase *expression = expressions.head; expression; expression = expression->next)
			code.push_back(AnalyzeStatement(ctx, expression));

		// If the function type is still auto it means that it hasn't returned anything
		if(function->type->returnType == ctx.typeAuto)
			function->type = ctx.GetFunctionType(source, ctx.typeVoid, function->type->arguments);

		if(function->type->returnType != ctx.typeVoid && !function->hasExplicitReturn)
			Report(ctx, source->pos, "ERROR: function must return a value of type '%.*s'", FMT_ISTR(returnType->name));

		// User might have not returned from all control paths, for a void function we will generate a return
		if(function->type->returnType == ctx.typeVoid)
			code.push_back(new (ctx.get<ExprReturn>()) ExprReturn(source, ctx.typeVoid, new (ctx.get<ExprVoid>()) ExprVoid(source, ctx.typeVoid), CreateFunctionCoroutineStateUpdate(ctx, source, function, 0), CreateFunctionUpvalueClose(ctx, source, function, ctx.scope)));
	}

	ClosePendingUpvalues(ctx, function);

	ctx.PopScope(SCOPE_FUNCTION);

	if(addedParentScope)
		ctx.PopScope(SCOPE_TYPE);

	if(parentType)
	{
		InplaceStr parentName = parentType->name;

		if(TypeClass *classType = getType<TypeClass>(parentType))
		{
			if(classType->proto)
				parentName = classType->proto->name;
		}

		if(name.name == parentName && function->type->returnType != ctx.typeVoid)
			Stop(ctx, source->pos, "ERROR: type constructor return type must be 'void'");
	}

	ExprVariableDefinition *contextVariableDefinition = NULL;

	if(parentType)
	{
		contextVariableDefinition = NULL;
	}
	else if(!coroutine && ctx.GetFunctionOwner(ctx.scope) == NULL)
	{
		contextVariableDefinition = NULL;
	}
	else if(prototype)
	{
		TypeRef *refType = getType<TypeRef>(function->contextType);

		assert(refType);

		VariableData *context = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, refType->alignment, refType, GetFunctionContextVariableName(ctx, function, ctx.GetFunctionIndex(function)), 0, ctx.uniqueVariableId++);

		context->isAlloca = true;
		context->offset = ~0u;

		function->contextVariable = context;

		ctx.AddVariable(context);

		contextVariableDefinition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, context, NULL);
	}
	else
	{
		contextVariableDefinition = CreateFunctionContextVariable(ctx, source, function, implementedPrototype);
	}

	// If the type was deduced, implement prototype now that it's known
	if(ImplementPrototype(ctx, function))
	{
		TypeRef *refType = getType<TypeRef>(function->contextType);

		assert(refType);

		if(refType->subType != ctx.typeVoid)
		{
			TypeClass *classType = getType<TypeClass>(refType->subType);

			assert(classType);

			if(!classType->members.empty())
				Stop(ctx, source->pos, "ERROR: function '%.*s' is being defined with the same set of arguments", FMT_ISTR(function->name.name));
		}
	}

	// Time to make operator overload visible
	if(isOperator)
		ctx.functionMap.insert(function->nameHash, function);

	FunctionData *conflict = CheckUniqueness(ctx, function);

	if(conflict)
		Stop(ctx, source->pos, "ERROR: function '%.*s' is being defined with the same set of arguments", FMT_ISTR(function->name.name));

	function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(source, function->type, function, contextArgumentDefinition, variables, coroutineStateRead, code, contextVariableDefinition);

	ctx.definitions.push_back(function->declaration);

	return function->declaration;
}

void DeduceShortFunctionReturnValue(ExpressionContext &ctx, SynBase *source, FunctionData *function, IntrusiveList<ExprBase> &expressions)
{
	if(function->hasExplicitReturn)
		return;

	TypeBase *expected = function->type->returnType;

	if(expected == ctx.typeVoid)
		return;

	TypeBase *actual = expressions.tail ? expressions.tail->type : ctx.typeVoid;

	if(actual == ctx.typeVoid)
		return;

	// If return type is auto, set it to type that is being returned
	if(function->type->returnType == ctx.typeAuto)
		function->type = ctx.GetFunctionType(source, actual, function->type->arguments);

	ExprBase *result = expected == ctx.typeAuto ? expressions.tail : CreateCast(ctx, source, expressions.tail, expected, false);
	result = new (ctx.get<ExprReturn>()) ExprReturn(source, ctx.typeVoid, result, CreateFunctionCoroutineStateUpdate(ctx, source, function, 0), CreateFunctionUpvalueClose(ctx, source, function, ctx.scope));

	if(expressions.head == expressions.tail)
	{
		expressions.head = expressions.tail = result;
	}
	else
	{
		ExprBase *curr = expressions.head;

		while(curr)
		{
			if(curr->next == expressions.tail)
				curr->next = result;

			curr = curr->next;
		}
	}

	function->hasExplicitReturn = true;
}

ExprBase* AnalyzeShortFunctionDefinition(ExpressionContext &ctx, SynShortFunctionDefinition *syntax, TypeFunction *argumentType)
{
	if(syntax->arguments.size() != argumentType->arguments.size())
		return NULL;

	TypeBase *returnType = argumentType->returnType;

	if(returnType->isGeneric)
		returnType = ctx.typeAuto;

	IntrusiveList<MatchData> argCasts;
	SmallArray<ArgumentData, 32> argData(ctx.allocator);

	TypeHandle *expected = argumentType->arguments.head;

	for(SynShortFunctionArgument *param = syntax->arguments.head; param; param = getType<SynShortFunctionArgument>(param->next))
	{
		TypeBase *type = NULL;

		if(param->type)
			type = AnalyzeType(ctx, param->type);

		if(type)
		{
			if(type == ctx.typeAuto)
				Stop(ctx, syntax->pos, "ERROR: function argument cannot be an auto type");

			if(type == ctx.typeVoid)
				Stop(ctx, syntax->pos, "ERROR: function argument cannot be a void type");

			char *name = (char*)ctx.allocator->alloc(param->name.length() + 2);

			sprintf(name, "%.*s$", FMT_ISTR(param->name));

			if(expected->type->isGeneric)
			{
				IntrusiveList<MatchData> aliases;

				if(TypeBase *match = MatchGenericType(ctx, syntax, expected->type, type, aliases, false))
					argData.push_back(ArgumentData(param, false, InplaceStr(name), match, NULL));
				else
					return NULL;
			}
			else
			{
				argData.push_back(ArgumentData(param, false, InplaceStr(name), expected->type, NULL));
			}

			argCasts.push_back(new (ctx.get<MatchData>()) MatchData(param->name, type));
		}
		else
		{
			argData.push_back(ArgumentData(param, false, param->name, expected->type, NULL));
		}

		expected = expected->next;
	}

	InplaceStr functionName = GetFunctionName(ctx, ctx.scope, NULL, InplaceStr(), false, false);

	TypeBase *contextClassType = CreateFunctionContextType(ctx, syntax, functionName);

	FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, syntax, ctx.scope, false, false, false, ctx.GetFunctionType(syntax, returnType, argData), ctx.GetReferenceType(contextClassType), SynIdentifier(functionName), IntrusiveList<MatchData>(), ctx.uniqueFunctionId++);

	// Fill in argument data
	for(unsigned i = 0; i < argData.size(); i++)
		function->arguments.push_back(argData[i]);

	CheckFunctionConflict(ctx, syntax, function->name.name);

	ctx.AddFunction(function);

	if(ctx.IsGenericFunction(function))
	{
		function->declaration = new (ctx.get<ExprGenericFunctionPrototype>()) ExprGenericFunctionPrototype(syntax, function->type, function);

		function->contextType = ctx.GetReferenceType(ctx.typeVoid);

		return function->declaration;
	}

	ctx.PushScope(function);

	function->functionScope = ctx.scope;

	IntrusiveList<ExprVariableDefinition> arguments;

	CreateFunctionArgumentVariables(ctx, syntax, function, argData, arguments);

	ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, syntax, function);

	function->argumentsSize = function->functionScope->dataSize;

	IntrusiveList<ExprBase> expressions;

	// Create casts of arguments with a wrong type
	for(MatchData *el = argCasts.head; el; el = el->next)
	{
		CheckVariableConflict(ctx, syntax, el->name);

		unsigned offset = AllocateVariableInScope(ctx, syntax, el->type->alignment, el->type);
		VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, syntax, ctx.scope, el->type->alignment, el->type, el->name, offset, ctx.uniqueVariableId++);

		if (IsLookupOnlyVariable(ctx, variable))
			variable->lookupOnly = true;

		ctx.AddVariable(variable);

		char *name = (char*)ctx.allocator->alloc(el->name.length() + 2);

		sprintf(name, "%.*s$", FMT_ISTR(el->name));

		ExprBase *access = CreateVariableAccess(ctx, syntax, IntrusiveList<SynIdentifier>(), InplaceStr(name), false);

		if(ctx.GetReferenceType(el->type) == access->type)
			access = new (ctx.get<ExprDereference>()) ExprDereference(syntax, el->type, access);
		else
			access = CreateCast(ctx, syntax, access, el->type, true);

		expressions.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, variable, CreateAssignment(ctx, syntax, CreateVariableAccess(ctx, syntax, variable, false), access)));
	}

	for(SynBase *expression = syntax->expressions.head; expression; expression = expression->next)
		expressions.push_back(AnalyzeStatement(ctx, expression));

	DeduceShortFunctionReturnValue(ctx, syntax, function, expressions);

	// If the function type is still auto it means that it hasn't returned anything
	if(function->type->returnType == ctx.typeAuto)
		function->type = ctx.GetFunctionType(syntax, ctx.typeVoid, function->type->arguments);

	if(function->type->returnType != ctx.typeVoid && !function->hasExplicitReturn)
		Stop(ctx, syntax->pos, "ERROR: function must return a value of type '%.*s'", FMT_ISTR(returnType->name));

	// User might have not returned from all control paths, for a void function we will generate a return
	if(function->type->returnType == ctx.typeVoid)
		expressions.push_back(new (ctx.get<ExprReturn>()) ExprReturn(syntax, ctx.typeVoid, new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid), CreateFunctionCoroutineStateUpdate(ctx, syntax, function, 0), CreateFunctionUpvalueClose(ctx, syntax, function, ctx.scope)));

	ClosePendingUpvalues(ctx, function);

	ctx.PopScope(SCOPE_FUNCTION);

	ExprVariableDefinition *contextVariableDefinition = CreateFunctionContextVariable(ctx, syntax, function, NULL);

	function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(syntax, function->type, function, contextArgumentDefinition, arguments, NULL, expressions, contextVariableDefinition);

	ctx.definitions.push_back(function->declaration);

	return function->declaration;
}

ExprBase* AnalyzeGenerator(ExpressionContext &ctx, SynGenerator *syntax)
{
	InplaceStr functionName = GetTemporaryFunctionName(ctx);

	SmallArray<ArgumentData, 32> arguments(ctx.allocator);

	TypeBase *contextClassType = CreateFunctionContextType(ctx, syntax, functionName);

	FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, syntax, ctx.scope, true, false, false, ctx.GetFunctionType(syntax, ctx.typeAuto, arguments), ctx.GetReferenceType(contextClassType), SynIdentifier(functionName), IntrusiveList<MatchData>(), ctx.uniqueFunctionId++);

	CheckFunctionConflict(ctx, syntax, function->name.name);

	ctx.AddFunction(function);

	ctx.PushScope(function);

	function->functionScope = ctx.scope;

	ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, syntax, function);

	function->argumentsSize = function->functionScope->dataSize;

	ExprBase *coroutineStateRead = NULL;

	IntrusiveList<ExprBase> expressions;

	if(function->coroutine)
	{
		unsigned offset = AllocateVariableInScope(ctx, syntax, ctx.typeInt->alignment, ctx.typeInt);
		function->coroutineJumpOffset = new (ctx.get<VariableData>()) VariableData(ctx.allocator, syntax, ctx.scope, 0, ctx.typeInt, InplaceStr("$jmpOffset"), offset, ctx.uniqueVariableId++);

		if (IsLookupOnlyVariable(ctx, function->coroutineJumpOffset))
			function->coroutineJumpOffset->lookupOnly = true;

		ctx.AddVariable(function->coroutineJumpOffset);

		AddFunctionCoroutineVariable(ctx, syntax, function, function->coroutineJumpOffset);

		coroutineStateRead = CreateVariableAccess(ctx, syntax, function->coroutineJumpOffset, true);
	}

	for(SynBase *expression = syntax->expressions.head; expression; expression = expression->next)
		expressions.push_back(AnalyzeStatement(ctx, expression));

	if(!function->hasExplicitReturn)
		Stop(ctx, syntax->pos, "ERROR: not a single element is generated, and an array element type is unknown");

	if(function->type->returnType == ctx.typeVoid)
		Stop(ctx, syntax->pos, "ERROR: cannot generate an array of 'void' element type");

	VariableData *empty = AllocateTemporary(ctx, syntax, function->type->returnType);

	expressions.push_back(new (ctx.get<ExprReturn>()) ExprReturn(syntax, ctx.typeVoid, CreateVariableAccess(ctx, syntax, empty, false), CreateFunctionCoroutineStateUpdate(ctx, syntax, function, 0), CreateFunctionUpvalueClose(ctx, syntax, function, ctx.scope)));

	ClosePendingUpvalues(ctx, function);

	ctx.PopScope(SCOPE_FUNCTION);

	ExprVariableDefinition *contextVariableDefinition = CreateFunctionContextVariable(ctx, syntax, function, NULL);

	function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(syntax, function->type, function, contextArgumentDefinition, IntrusiveList<ExprVariableDefinition>(), coroutineStateRead, expressions, contextVariableDefinition);

	ctx.definitions.push_back(function->declaration);

	ExprBase *access = new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(syntax, function->type, function, CreateFunctionContextAccess(ctx, syntax, function));

	return CreateFunctionCall1(ctx, syntax, InplaceStr("__gen_list"), CreateSequence(ctx, syntax, contextVariableDefinition, access), false, true);
}

ExprBase* AnalyzeShortFunctionDefinition(ExpressionContext &ctx, SynShortFunctionDefinition *syntax, TypeBase *type, ArrayView<ArgumentData> currArguments, IntrusiveList<MatchData> aliases)
{
	TypeFunction *functionType = getType<TypeFunction>(type);

	// Only applies to function calls
	if(!functionType)
		return NULL;

	IntrusiveList<TypeHandle> &fuctionArgs = functionType->arguments;

	// Function doesn't accept any more arguments
	if(currArguments.size() + 1 > fuctionArgs.size())
		return NULL;

	// Get current argument type
	TypeBase *target = NULL;

	if(functionType->isGeneric)
	{
		// Collect aliases up to the current argument
		for(unsigned i = 0; i < currArguments.size(); i++)
		{
			// Exit if the arguments before the short inline function fail to match
			if(!MatchGenericType(ctx, syntax, fuctionArgs[i]->type, currArguments[i].type, aliases, false))
				return NULL;
		}

		target = ResolveGenericTypeAliases(ctx, syntax, fuctionArgs[currArguments.size()]->type, aliases);
	}
	else
	{
		target = fuctionArgs[currArguments.size()]->type;
	}

	TypeFunction *argumentType = getType<TypeFunction>(target);

	if(!argumentType)
		return NULL;

	return AnalyzeShortFunctionDefinition(ctx, syntax, argumentType);
}

ExprBase* AssertResolvableTypeLiteral(ExpressionContext &ctx, SynBase *source, ExprBase *expr)
{
	if(ExprTypeLiteral *node = getType<ExprTypeLiteral>(expr))
	{
		if(isType<TypeArgumentSet>(node->value))
			Stop(ctx, source->pos, "ERROR: expected '.first'/'.last'/'[N]'/'.size' after 'argument'");

		if(isType<TypeMemberSet>(node->value))
			Stop(ctx, source->pos, "ERROR: expected '(' after 'hasMember'");

		if(node->value->isGeneric)
			Stop(ctx, source->pos, "ERROR: cannot take typeid from generic type");
	}

	return expr;
}

ExprBase* AssertValueExpression(ExpressionContext &ctx, SynBase *source, ExprBase *expr)
{
	if(isType<ExprFunctionOverloadSet>(expr))
	{
		SmallArray<FunctionValue, 32> functions(ctx.allocator);

		GetNodeFunctions(ctx, source, expr, functions);

		if(ctx.errorBuf && ctx.errorBufSize)
		{
			if(ctx.errorCount == 0)
			{
				ctx.errorPos = source->pos.begin;
				ctx.errorBufLocation = ctx.errorBuf;
			}

			SafeSprintf(ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), "ERROR: ambiguity, there is more than one overloaded function available:\n");

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);

			ReportOnFunctionSelectError(ctx, source, ctx.errorBufLocation, ctx.errorBufSize - unsigned(ctx.errorBufLocation - ctx.errorBuf), functions);

			ctx.errorBufLocation += strlen(ctx.errorBufLocation);
		}

		assert(ctx.errorHandlerActive);

		longjmp(ctx.errorHandler, 1);
	}

	if(isType<ExprGenericFunctionPrototype>(expr))
		Stop(ctx, source->pos, "ERROR: ambiguity, the expression is a generic function");

	if(ExprFunctionAccess *node = getType<ExprFunctionAccess>(expr))
	{
		if(ctx.IsGenericFunction(node->function))
			Stop(ctx, source->pos, "ERROR: ambiguity, '%.*s' is a generic function", FMT_ISTR(node->function->name.name));

		if(node->function->type->returnType == ctx.typeAuto)
			Stop(ctx, source->pos, "ERROR: function '%.*s' type is unresolved at this point", FMT_ISTR(node->function->name.name));
	}

	AssertResolvableTypeLiteral(ctx, source, expr);

	return expr;
}

InplaceStr GetTypeConstructorName(InplaceStr functionName)
{
	// TODO: add type scopes and lookup owner namespace
	for(const char *pos = functionName.end; pos > functionName.begin; pos--)
	{
		if(*pos == '.')
		{
			functionName = InplaceStr(pos + 1, functionName.end);
			break;
		}
	}

	return functionName;
}

InplaceStr GetTypeConstructorName(TypeClass *classType)
{
	if(TypeGenericClassProto *proto = classType->proto)
		return GetTypeConstructorName(proto->name);

	return GetTypeConstructorName(classType->name);
}

InplaceStr GetTypeConstructorName(TypeGenericClassProto *typeGenericClassProto)
{
	return GetTypeConstructorName(typeGenericClassProto->name);
}

InplaceStr GetTypeDefaultConstructorName(ExpressionContext &ctx, TypeClass *classType)
{
	InplaceStr baseName(GetTypeConstructorName(classType));

	char *name = (char*)ctx.allocator->alloc(baseName.length() + 2);
	sprintf(name, "%.*s$", FMT_ISTR(baseName));

	return InplaceStr(name);
}

bool ContainsSameOverload(ArrayView<FunctionData*> functions, FunctionData *value)
{
	for(unsigned i = 0; i < functions.size(); i++)
	{
		if(SameArguments(functions[i]->type, value->type))
			return true;
	}

	return false;
}

bool GetTypeConstructorFunctions(ExpressionContext &ctx, TypeBase *type, bool noArguments, SmallArray<FunctionData*, 32> &functions)
{
	TypeClass *classType = getType<TypeClass>(type);
	TypeGenericClassProto *typeGenericClassProto = getType<TypeGenericClassProto>(type);

	if(classType && classType->proto)
		typeGenericClassProto = classType->proto;

	unsigned hash = StringHashContinue(type->nameHash, "::");

	if(classType)
	{
		InplaceStr functionName = GetTypeConstructorName(classType);

		hash = StringHashContinue(hash, functionName.begin, functionName.end);
	}
	else
	{
		hash = StringHashContinue(hash, type->name.begin, type->name.end);
	}

	for(HashMap<FunctionData*>::Node *node = ctx.functionMap.first(hash); node; node = ctx.functionMap.next(node))
	{
		if(noArguments && !node->value->arguments.empty() && !node->value->arguments[0].value)
			continue;

		if(!ContainsSameOverload(functions, node->value))
			functions.push_back(node->value);
	}

	if(typeGenericClassProto)
	{
		// Look for a member function in a generic class base and instantiate them
		unsigned hash = StringHashContinue(typeGenericClassProto->nameHash, "::");

		hash = StringHashContinue(hash, typeGenericClassProto->name.begin, typeGenericClassProto->name.end);

		for(HashMap<FunctionData*>::Node *node = ctx.functionMap.first(hash); node; node = ctx.functionMap.next(node))
		{
			if(noArguments && !node->value->arguments.empty() && !node->value->arguments[0].value)
				continue;

			if(!ContainsSameOverload(functions, node->value))
				functions.push_back(node->value);
		}
	}

	for(HashMap<FunctionData*>::Node *node = ctx.functionMap.first(StringHashContinue(hash, "$")); node; node = ctx.functionMap.next(node))
	{
		if(noArguments && !node->value->arguments.empty() && !node->value->arguments[0].value)
			continue;

		if(!ContainsSameOverload(functions, node->value))
			functions.push_back(node->value);
	}

	if(typeGenericClassProto)
	{
		// Look for a member function in a generic class base and instantiate them
		unsigned hash = StringHashContinue(typeGenericClassProto->nameHash, "::");

		hash = StringHashContinue(hash, typeGenericClassProto->name.begin, typeGenericClassProto->name.end);

		for(HashMap<FunctionData*>::Node *node = ctx.functionMap.first(StringHashContinue(hash, "$")); node; node = ctx.functionMap.next(node))
		{
			if(noArguments && !node->value->arguments.empty() && !node->value->arguments[0].value)
				continue;

			if(!ContainsSameOverload(functions, node->value))
				functions.push_back(node->value);
		}
	}

	return !functions.empty();
}

ExprBase* CreateConstructorAccess(ExpressionContext &ctx, SynBase *source, ArrayView<FunctionData*> functions, ExprBase *context)
{
	if(functions.size() > 1)
	{
		IntrusiveList<TypeHandle> types;
		IntrusiveList<FunctionHandle> handles;

		for(unsigned i = 0; i < functions.size(); i++)
		{
			FunctionData *curr = functions[i];

			types.push_back(new (ctx.get<TypeHandle>()) TypeHandle(curr->type));
			handles.push_back(new (ctx.get<FunctionHandle>()) FunctionHandle(curr));
		}

		TypeFunctionSet *type = ctx.GetFunctionSetType(types);

		return new (ctx.get<ExprFunctionOverloadSet>()) ExprFunctionOverloadSet(source, type, handles, context);
	}

	return new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(source, functions[0]->type, functions[0], context);
}

ExprBase* CreateConstructorAccess(ExpressionContext &ctx, SynBase *source, TypeBase *type, bool noArguments, ExprBase *context)
{
	SmallArray<FunctionData*, 32> functions(ctx.allocator);

	if(GetTypeConstructorFunctions(ctx, type, noArguments, functions))
		return CreateConstructorAccess(ctx, source, functions, context);

	return NULL;
}

bool HasDefautConstructor(ExpressionContext &ctx, SynBase *source, TypeBase *type)
{
	// Find array element type
	while(TypeArray *arrType = getType<TypeArray>(type))
		type = arrType->subType;

	SmallArray<FunctionData*, 32> functions(ctx.allocator);

	if(GetTypeConstructorFunctions(ctx, type, true, functions))
	{
		SmallArray<FunctionValue, 32> overloads(ctx.allocator);

		for(unsigned i = 0; i < functions.size(); i++)
		{
			FunctionData *curr = functions[i];

			overloads.push_back(FunctionValue(source, curr, new (ctx.get<ExprNullptrLiteral>()) ExprNullptrLiteral(source, curr->contextType)));
		}

		SmallArray<unsigned, 32> ratings(ctx.allocator);
		SmallArray<ArgumentData, 32> arguments(ctx.allocator);

		if(FunctionValue bestOverload = SelectBestFunction(ctx, source, overloads, IntrusiveList<TypeHandle>(), arguments, ratings))
			return true;

		return false;
	}

	return false;
}

ExprBase* CreateDefaultConstructorCall(ExpressionContext &ctx, SynBase *source, TypeBase *type, ExprBase *pointer)
{
	assert(isType<TypeRef>(pointer->type) || isType<TypeUnsizedArray>(pointer->type));

	if(isType<TypeArray>(type) || isType<TypeUnsizedArray>(type))
	{
		if(TypeArray *arrType = getType<TypeArray>(type))
			type = arrType->subType;
		else if(TypeUnsizedArray *arrType = getType<TypeUnsizedArray>(type))
			type = arrType->subType;

		if(HasDefautConstructor(ctx, source, type))
		{
			if(TypeRef *typeRef = getType<TypeRef>(pointer->type))
				return CreateFunctionCall1(ctx, source, InplaceStr("__init_array"), new (ctx.get<ExprDereference>()) ExprDereference(source, typeRef->subType, pointer), false, true);

			return CreateFunctionCall1(ctx, source, InplaceStr("__init_array"), pointer, false, true);
		}

		return NULL;
	}

	if(ExprBase *constructor = CreateConstructorAccess(ctx, source, type, true, pointer))
	{
		// Collect a set of available functions
		SmallArray<FunctionValue, 32> functions(ctx.allocator);

		GetNodeFunctions(ctx, source, constructor, functions);

		return CreateFunctionCallOverloaded(ctx, source, constructor, functions, IntrusiveList<TypeHandle>(), NULL, false);
	}

	return NULL;
}

void CreateDefaultConstructorCode(ExpressionContext &ctx, SynBase *source, TypeClass *classType, IntrusiveList<ExprBase> &expressions)
{
	for(VariableHandle *el = classType->members.head; el; el = el->next)
	{
		VariableData *variable = el->variable;

		ExprBase *member = CreateGetAddress(ctx, source, CreateVariableAccess(ctx, source, variable, true));

		if(variable->name == InplaceStr("$typeid"))
		{
			expressions.push_back(CreateAssignment(ctx, source, member, new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, classType)));
			continue;
		}

		if(HasDefautConstructor(ctx, source, variable->type))
		{
			if(ExprBase *call = CreateDefaultConstructorCall(ctx, source, variable->type, member))
				expressions.push_back(call);
		}
	}
}

void CreateDefaultClassConstructor(ExpressionContext &ctx, SynBase *source, ExprClassDefinition *classDefinition)
{
	TypeClass *classType = classDefinition->classType;

	// Check if custom default assignment operator is required
	bool customConstructor = false;

	if(classType->extendable)
	{
		customConstructor = true;
	}
	else
	{
		for(VariableHandle *el = classType->members.head; el; el = el->next)
		{
			TypeBase *base = el->variable->type;

			// Find array element type
			while(TypeArray *arrType = getType<TypeArray>(base))
				base = arrType->subType;

			if(HasDefautConstructor(ctx, source, base))
			{
				customConstructor = true;
				break;
			}
		}
	}

	if(customConstructor)
	{
		bool addedParentScope = RestoreParentTypeScope(ctx, source, classType);

		InplaceStr functionName = GetFunctionNameInScope(ctx, ctx.scope, classType, GetTypeDefaultConstructorName(ctx, classType), false, false);

		SmallArray<ArgumentData, 32> arguments(ctx.allocator);

		FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, source, ctx.scope, false, false, false, ctx.GetFunctionType(source, ctx.typeVoid, arguments), ctx.GetReferenceType(classType), SynIdentifier(functionName), IntrusiveList<MatchData>(), ctx.uniqueFunctionId++);

		CheckFunctionConflict(ctx, source, function->name.name);

		ctx.AddFunction(function);

		ctx.PushScope(function);

		function->functionScope = ctx.scope;

		ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, source, function);

		function->argumentsSize = function->functionScope->dataSize;

		IntrusiveList<ExprBase> expressions;

		CreateDefaultConstructorCode(ctx, source, classType, expressions);

		expressions.push_back(new (ctx.get<ExprReturn>()) ExprReturn(source, ctx.typeVoid, new (ctx.get<ExprVoid>()) ExprVoid(source, ctx.typeVoid), NULL, NULL));

		ClosePendingUpvalues(ctx, function);

		ctx.PopScope(SCOPE_FUNCTION);

		if(addedParentScope)
			ctx.PopScope(SCOPE_TYPE);

		ExprVariableDefinition *contextVariableDefinition = CreateFunctionContextVariable(ctx, source, function, NULL);

		function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(source, function->type, function, contextArgumentDefinition, IntrusiveList<ExprVariableDefinition>(), NULL, expressions, contextVariableDefinition);

		ctx.definitions.push_back(function->declaration);

		classDefinition->functions.push_back(function->declaration);
	}
}

void CreateDefaultClassAssignment(ExpressionContext &ctx, SynBase *source, ExprClassDefinition *classDefinition)
{
	TypeClass *classType = classDefinition->classType;

	IntrusiveList<VariableHandle> customAssignMembers;

	for(VariableHandle *curr = classType->members.head; curr; curr = curr->next)
	{
		TypeBase *type = curr->variable->type;

		if(isType<TypeRef>(type) || isType<TypeArray>(type) || isType<TypeUnsizedArray>(type) || isType<TypeFunction>(type))
			continue;

		SmallArray<ArgumentData, 2> arguments(ctx.allocator);

		arguments.push_back(ArgumentData(source, false, InplaceStr(), ctx.GetReferenceType(type), NULL));
		arguments.push_back(ArgumentData(source, false, InplaceStr(), type, NULL));

		if(ExprBase *overloads = CreateVariableAccess(ctx, source, IntrusiveList<SynIdentifier>(), InplaceStr("="), false))
		{
			SmallArray<FunctionValue, 32> functions(ctx.allocator);

			GetNodeFunctions(ctx, source, overloads, functions);

			if(!functions.empty())
			{
				SmallArray<unsigned, 32> ratings(ctx.allocator);

				FunctionValue bestOverload = SelectBestFunction(ctx, source, functions, IntrusiveList<TypeHandle>(), arguments, ratings);

				if(bestOverload)
				{
					customAssignMembers.push_back(new (ctx.get<VariableHandle>()) VariableHandle(curr->variable));
					continue;
				}
			}
		}

		if(ExprBase *overloads = CreateVariableAccess(ctx, source, IntrusiveList<SynIdentifier>(), InplaceStr("default_assign$_"), false))
		{
			SmallArray<FunctionValue, 32> functions(ctx.allocator);

			GetNodeFunctions(ctx, source, overloads, functions);

			if(!functions.empty())
			{
				SmallArray<unsigned, 32> ratings(ctx.allocator);

				FunctionValue bestOverload = SelectBestFunction(ctx, source, functions, IntrusiveList<TypeHandle>(), arguments, ratings);

				if(bestOverload)
				{
					customAssignMembers.push_back(new (ctx.get<VariableHandle>()) VariableHandle(curr->variable));
					continue;
				}
			}
		}
	}

	if(!customAssignMembers.empty())
	{
		InplaceStr functionName = GetFunctionNameInScope(ctx, ctx.scope, NULL, InplaceStr("default_assign$_"), false, false);

		SmallArray<ArgumentData, 32> arguments(ctx.allocator);

		arguments.push_back(ArgumentData(source, false, InplaceStr("left"), ctx.GetReferenceType(classType), NULL));
		arguments.push_back(ArgumentData(source, false, InplaceStr("right"), classType, NULL));

		FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, source, ctx.scope, false, false, false, ctx.GetFunctionType(source, ctx.typeVoid, arguments), ctx.GetReferenceType(ctx.typeVoid), SynIdentifier(functionName), IntrusiveList<MatchData>(), ctx.uniqueFunctionId++);

		// Fill in argument data
		for(unsigned i = 0; i < arguments.size(); i++)
			function->arguments.push_back(arguments[i]);

		CheckFunctionConflict(ctx, source, function->name.name);

		ctx.AddFunction(function);

		ctx.PushScope(function);

		function->functionScope = ctx.scope;

		IntrusiveList<ExprVariableDefinition> variables;

		CreateFunctionArgumentVariables(ctx, source, function, arguments, variables);

		ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, source, function);

		function->argumentsSize = function->functionScope->dataSize;

		IntrusiveList<ExprBase> expressions;

		for(VariableHandle *curr = customAssignMembers.head; curr; curr = curr->next)
		{
			VariableData *leftArgument = variables.head->variable;

			ExprBase *left = CreateVariableAccess(ctx, source, leftArgument, false);

			ExprBase *leftMember = CreateMemberAccess(ctx, source, left, curr->variable->name, false);

			VariableData *rightArgument = getType<ExprVariableDefinition>(variables.head->next)->variable;

			ExprBase *right = CreateVariableAccess(ctx, source, rightArgument, false);

			ExprBase *rightMember = CreateMemberAccess(ctx, source, right, curr->variable->name, false);

			expressions.push_back(CreateAssignment(ctx, source, leftMember, rightMember));
		}

		expressions.push_back(new (ctx.get<ExprReturn>()) ExprReturn(source, ctx.typeVoid, new (ctx.get<ExprVoid>()) ExprVoid(source, ctx.typeVoid), NULL, NULL));

		ClosePendingUpvalues(ctx, function);

		ctx.PopScope(SCOPE_FUNCTION);

		function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(source, function->type, function, contextArgumentDefinition, variables, NULL, expressions, NULL);

		ctx.definitions.push_back(function->declaration);

		classDefinition->functions.push_back(function->declaration);
	}
}

void CreateDefaultClassMembers(ExpressionContext &ctx, SynBase *source, ExprClassDefinition *classDefinition)
{
	CreateDefaultClassConstructor(ctx, source, classDefinition);

	CreateDefaultClassAssignment(ctx, source, classDefinition);
}

void AnalyzeClassStaticIf(ExpressionContext &ctx, ExprClassDefinition *classDefinition, SynClassStaticIf *syntax)
{
	ExprBase *condition = AnalyzeExpression(ctx, syntax->condition);

	condition = CreateConditionCast(ctx, condition->source, condition);

	if(ExprBoolLiteral *number = getType<ExprBoolLiteral>(EvaluateExpression(ctx, CreateCast(ctx, syntax, condition, ctx.typeBool, false))))
	{
		if(number->value)
			AnalyzeClassElements(ctx, classDefinition, syntax->trueBlock);
		else if(syntax->falseBlock)
			AnalyzeClassElements(ctx, classDefinition, syntax->falseBlock);
	}
	else
	{
		Stop(ctx, syntax->pos, "ERROR: can't get condition value");
	}
}

void AnalyzeClassConstants(ExpressionContext &ctx, SynBase *source, TypeBase *type, IntrusiveList<SynConstant> constants, IntrusiveList<ConstantData> &target)
{
	for(SynConstant *constant = constants.head; constant; constant = getType<SynConstant>(constant->next))
	{
		ExprBase *value = NULL;
			
		if(constant->value)
		{
			value = AnalyzeExpression(ctx, constant->value);

			if(type == ctx.typeAuto)
				type = value->type;

			if(!ctx.IsNumericType(type))
				Stop(ctx, source->pos, "ERROR: only basic numeric types can be used as constants");

			value = EvaluateExpression(ctx, CreateCast(ctx, constant, value, type, false));
		}
		else if(ctx.IsIntegerType(type) && constant != constants.head)
		{
			value = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateCast(ctx, constant, CreateBinaryOp(ctx, constant, SYN_BINARY_OP_ADD, target.tail->value, new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(constant, type, 1)), type, false)));
		}
		else
		{
			if(constant == constants.head)
				Stop(ctx, source->pos, "ERROR: '=' not found after constant name");
			else
				Stop(ctx, source->pos, "ERROR: only integer constant list gets automatically incremented by 1");
		}

		if(!value || (!isType<ExprBoolLiteral>(value) && !isType<ExprCharacterLiteral>(value) && !isType<ExprIntegerLiteral>(value) && !isType<ExprRationalLiteral>(value)))
			Stop(ctx, source->pos, "ERROR: expression didn't evaluate to a constant number");

		for(ConstantData *curr = target.head; curr; curr = curr->next)
		{
			if(constant->name == curr->name)
				Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken", FMT_ISTR(curr->name));
		}

		CheckVariableConflict(ctx, constant, constant->name);

		target.push_back(new (ctx.get<ConstantData>()) ConstantData(constant->name, value));
	}
}

void AnalyzeClassElements(ExpressionContext &ctx, ExprClassDefinition *classDefinition, SynClassElements *syntax)
{
	for(SynTypedef *typeDef = syntax->typedefs.head; typeDef; typeDef = getType<SynTypedef>(typeDef->next))
	{
		ExprAliasDefinition *alias = AnalyzeTypedef(ctx, typeDef);

		classDefinition->classType->aliases.push_back(new (ctx.get<MatchData>()) MatchData(alias->alias->name, alias->alias->type));
	}

	{
		for(SynVariableDefinitions *member = syntax->members.head; member; member = getType<SynVariableDefinitions>(member->next))
		{
			ExprVariableDefinitions *node = AnalyzeVariableDefinitions(ctx, member);

			for(ExprBase *definition = node->definitions.head; definition; definition = definition->next)
			{
				if(isType<ExprError>(definition))
					continue;

				ExprVariableDefinition *variableDefinition = getType<ExprVariableDefinition>(definition);

				assert(variableDefinition);

				if(variableDefinition->initializer)
					Report(ctx, syntax->pos, "ERROR: member can't have an initializer");

				classDefinition->classType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(variableDefinition->variable));
			}
		}
	}

	FinalizeAlignment(classDefinition->classType);

	classDefinition->classType->completed = true;

	for(SynConstantSet *constantSet = syntax->constantSets.head; constantSet; constantSet = getType<SynConstantSet>(constantSet->next))
	{
		TypeBase *type = AnalyzeType(ctx, constantSet->type);

		AnalyzeClassConstants(ctx, constantSet, type, constantSet->constants, classDefinition->classType->constants);
	}

	for(SynFunctionDefinition *function = syntax->functions.head; function; function = getType<SynFunctionDefinition>(function->next))
		classDefinition->functions.push_back(AnalyzeFunctionDefinition(ctx, function, NULL, NULL, IntrusiveList<MatchData>(), false, false, true));

	for(SynAccessor *accessor = syntax->accessors.head; accessor; accessor = getType<SynAccessor>(accessor->next))
	{
		SynBase *parentType = new (ctx.get<SynTypeSimple>()) SynTypeSimple(accessor->begin, accessor->end, IntrusiveList<SynIdentifier>(), classDefinition->classType->name);

		TypeBase *accessorType = AnalyzeType(ctx, accessor->type);

		if(accessor->getBlock)
		{
			IntrusiveList<SynIdentifier> aliases;
			IntrusiveList<SynFunctionArgument> arguments;

			IntrusiveList<SynBase> expressions = accessor->getBlock->expressions;

			SynFunctionDefinition *function = new (ctx.get<SynFunctionDefinition>()) SynFunctionDefinition(accessor->begin, accessor->end, false, false, parentType, true, accessor->type, false, accessor->name, aliases, arguments, expressions);

			TypeFunction *instance = ctx.GetFunctionType(syntax, accessorType, IntrusiveList<TypeHandle>());

			ExprBase *definition = AnalyzeFunctionDefinition(ctx, function, instance, NULL, IntrusiveList<MatchData>(), false, false, false);

			if(ExprFunctionDefinition *node = getType<ExprFunctionDefinition>(definition))
				accessorType = node->function->type->returnType;

			classDefinition->functions.push_back(definition);
		}

		if(accessor->setBlock)
		{
			IntrusiveList<SynIdentifier> aliases;

			IntrusiveList<SynFunctionArgument> arguments;
			arguments.push_back(new (ctx.get<SynFunctionArgument>()) SynFunctionArgument(accessor->begin, accessor->end, false, accessor->type, accessor->setName.empty() ? InplaceStr("r") : accessor->setName, NULL));

			IntrusiveList<SynBase> expressions = accessor->setBlock->expressions;

			SynFunctionDefinition *function = new (ctx.get<SynFunctionDefinition>()) SynFunctionDefinition(accessor->begin, accessor->end, false, false, parentType, true, new (ctx.get<SynTypeAuto>()) SynTypeAuto(accessor->begin, accessor->end), false, accessor->name, aliases, arguments, expressions);

			IntrusiveList<TypeHandle> argTypes;
			argTypes.push_back(new (ctx.get<TypeHandle>()) TypeHandle(accessorType));

			TypeFunction *instance = ctx.GetFunctionType(syntax, ctx.typeAuto, argTypes);

			classDefinition->functions.push_back(AnalyzeFunctionDefinition(ctx, function, instance, NULL, IntrusiveList<MatchData>(), false, false, false));
		}
	}

	// TODO: The way SynClassElements is made, it could allow member re-ordering! class should contain in-order members and static if's
	// TODO: We should be able to analyze all static if typedefs before members and constants and analyze them before functions
	for(SynClassStaticIf *staticIf = syntax->staticIfs.head; staticIf; staticIf = getType<SynClassStaticIf>(staticIf->next))
		AnalyzeClassStaticIf(ctx, classDefinition, staticIf);
}

ExprBase* AnalyzeClassDefinition(ExpressionContext &ctx, SynClassDefinition *syntax, TypeGenericClassProto *proto, IntrusiveList<TypeHandle> generics)
{
	CheckTypeConflict(ctx, syntax, syntax->name);

	InplaceStr typeName = GetTypeNameInScope(ctx, ctx.scope, syntax->name);

	if(!proto && !syntax->aliases.empty())
	{
		for(SynIdentifier *curr = syntax->aliases.head; curr; curr = getType<SynIdentifier>(curr->next))
		{
			if(ctx.typeMap.find(curr->name.hash()))
				Stop(ctx, curr->pos, "ERROR: there is already a type or an alias with the same name");

			for(SynIdentifier *prev = syntax->aliases.head; prev && prev != curr; prev = getType<SynIdentifier>(prev->next))
			{
				if(prev->name == curr->name)
					Stop(ctx, curr->pos, "ERROR: there is already a type or an alias with the same name");
			}
		}

		if(TypeBase **type = ctx.typeMap.find(typeName.hash()))
		{
			TypeClass *originalDefinition = getType<TypeClass>(*type);

			if(originalDefinition)
				Stop(ctx, syntax->pos, "ERROR: type '%.*s' was forward declared as a non-generic type", FMT_ISTR(typeName));
		}

		TypeGenericClassProto *genericProtoType = new (ctx.get<TypeGenericClassProto>()) TypeGenericClassProto(syntax, ctx.scope, typeName, syntax);

		ctx.AddType(genericProtoType);

		return new (ctx.get<ExprGenericClassPrototype>()) ExprGenericClassPrototype(syntax, ctx.typeVoid, genericProtoType);
	}

	assert(generics.size() == syntax->aliases.size());

	InplaceStr className = generics.empty() ? typeName : GetGenericClassTypeName(ctx, proto, generics);

	if(className.length() > NULLC_MAX_TYPE_NAME_LENGTH)
		Stop(ctx, syntax->pos, "ERROR: generated type name exceeds maximum type length '%d'", NULLC_MAX_TYPE_NAME_LENGTH);

	TypeClass *originalDefinition = NULL;

	if(TypeBase **type = ctx.typeMap.find(className.hash()))
	{
		originalDefinition = getType<TypeClass>(*type);

		if(!originalDefinition || originalDefinition->completed)
			Stop(ctx, syntax->pos, "ERROR: '%.*s' is being redefined", FMT_ISTR(className));
	}

	if(!generics.empty())
	{
		// Check if type already exists
		assert(ctx.genericTypeMap.find(className.hash()) == NULL);

		if(ctx.GetGenericClassInstantiationDepth() > NULLC_MAX_GENERIC_INSTANCE_DEPTH)
			Stop(ctx, syntax->pos, "ERROR: reached maximum generic type instance depth (%d)", NULLC_MAX_GENERIC_INSTANCE_DEPTH);
	}

	unsigned alignment = syntax->align ? AnalyzeAlignment(ctx, syntax->align) : 0;

	IntrusiveList<MatchData> actualGenerics;

	{
		TypeHandle *currType = generics.head;
		SynIdentifier *currName = syntax->aliases.head;

		while(currType && currName)
		{
			actualGenerics.push_back(new (ctx.get<MatchData>()) MatchData(currName->name, currType->type));

			currType = currType->next;
			currName = getType<SynIdentifier>(currName->next);
		}
	}

	TypeClass *baseClass = NULL;

	if(syntax->baseClass)
	{
		ctx.PushTemporaryScope();

		for(MatchData *el = actualGenerics.head; el; el = el->next)
			ctx.AddAlias(new (ctx.get<AliasData>()) AliasData(syntax, ctx.scope, el->type, el->name, ctx.uniqueAliasId++));

		TypeBase *type = AnalyzeType(ctx, syntax->baseClass);

		ctx.PopScope(SCOPE_TEMPORARY);

		baseClass = getType<TypeClass>(type);

		if(!baseClass || !baseClass->extendable)
			Stop(ctx, syntax->pos, "ERROR: type '%.*s' is not extendable", FMT_ISTR(type->name));
	}
	
	bool extendable = syntax->extendable || baseClass;

	TypeClass *classType = NULL;
	
	if(originalDefinition)
	{
		classType = originalDefinition;

		classType->extendable = extendable;
		classType->baseClass = baseClass;
	}
	else
	{
		classType = new (ctx.get<TypeClass>()) TypeClass(className, syntax, ctx.scope, proto, actualGenerics, extendable, baseClass);

		ctx.AddType(classType);
	}

	if(!generics.empty())
		ctx.genericTypeMap.insert(className.hash(), classType);

	ExprClassDefinition *classDefinition = new (ctx.get<ExprClassDefinition>()) ExprClassDefinition(syntax, ctx.typeVoid, classType);

	ctx.PushScope(classType);

	classType->typeScope = ctx.scope;

	for(MatchData *el = classType->generics.head; el; el = el->next)
		ctx.AddAlias(new (ctx.get<AliasData>()) AliasData(syntax, ctx.scope, el->type, el->name, ctx.uniqueAliasId++));

	// Base class adds a typeid member
	if(extendable)
	{
		unsigned offset = AllocateVariableInScope(ctx, syntax, ctx.typeTypeID->alignment, ctx.typeTypeID);
		VariableData *member = new (ctx.get<VariableData>()) VariableData(ctx.allocator, syntax, ctx.scope, ctx.typeTypeID->alignment, ctx.typeTypeID, InplaceStr("$typeid"), offset, ctx.uniqueVariableId++);

		ctx.AddVariable(member);

		classType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(member));
	}

	if(baseClass)
	{
		// Use base class alignment at ths point to match member locations
		classType->alignment = baseClass->alignment;

		// Add members of base class
		for(MatchData *el = baseClass->aliases.head; el; el = el->next)
		{
			ctx.AddAlias(new (ctx.get<AliasData>()) AliasData(syntax, ctx.scope, el->type, el->name, ctx.uniqueAliasId++));

			classType->aliases.push_back(new (ctx.get<MatchData>()) MatchData(el->name, el->type));
		}

		for(VariableHandle *el = baseClass->members.head; el; el = el->next)
		{
			if(el->variable->name == InplaceStr("$typeid"))
				continue;

			CheckVariableConflict(ctx, syntax, el->variable->name);

			unsigned offset = AllocateVariableInScope(ctx, syntax, el->variable->alignment, el->variable->type);

			assert(offset == el->variable->offset);

			VariableData *member = new (ctx.get<VariableData>()) VariableData(ctx.allocator, syntax, ctx.scope, el->variable->alignment, el->variable->type, el->variable->name, offset, ctx.uniqueVariableId++);

			ctx.AddVariable(member);

			classType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(member));
		}

		for(ConstantData *el = baseClass->constants.head; el; el = el->next)
			classType->constants.push_back(new (ctx.get<ConstantData>()) ConstantData(el->name, el->value));

		FinalizeAlignment(classType);

		assert(classType->size == baseClass->size);
	}

	if(syntax->align)
		classType->alignment = alignment;

	AnalyzeClassElements(ctx, classDefinition, syntax->elements);

	ctx.PopScope(SCOPE_TYPE);

	if(classType->size >= 64 * 1024)
		Stop(ctx, syntax->pos, "ERROR: class size cannot exceed 65535 bytes");

	CreateDefaultClassMembers(ctx, syntax, classDefinition);

	return classDefinition;
}

ExprBase* AnalyzeClassPrototype(ExpressionContext &ctx, SynClassPrototype *syntax)
{
	CheckTypeConflict(ctx, syntax, syntax->name);

	InplaceStr typeName = GetTypeNameInScope(ctx, ctx.scope, syntax->name);

	if(TypeBase **type = ctx.typeMap.find(typeName.hash()))
	{
		TypeClass *originalDefinition = getType<TypeClass>(*type);

		if(!originalDefinition || originalDefinition->completed)
			Stop(ctx, syntax->pos, "ERROR: '%.*s' is being redefined", FMT_ISTR(syntax->name));

		return new (ctx.get<ExprClassPrototype>()) ExprClassPrototype(syntax, ctx.typeVoid, originalDefinition);
	}

	IntrusiveList<MatchData> actualGenerics;

	TypeClass *classType = new (ctx.get<TypeClass>()) TypeClass(typeName, syntax, ctx.scope, NULL, actualGenerics, false, NULL);

	ctx.AddType(classType);

	return new (ctx.get<ExprClassPrototype>()) ExprClassPrototype(syntax, ctx.typeVoid, classType);
}

void AnalyzeEnumConstants(ExpressionContext &ctx, SynBase *source, TypeBase *type, IntrusiveList<SynConstant> constants, IntrusiveList<ConstantData> &target)
{
	ExprIntegerLiteral *last = NULL;

	for(SynConstant *constant = constants.head; constant; constant = getType<SynConstant>(constant->next))
	{
		ExprIntegerLiteral *value = NULL;
			
		if(constant->value)
		{
			value = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateCast(ctx, constant, AnalyzeExpression(ctx, constant->value), ctx.typeInt, false)));
		}
		else if(last)
		{
			value = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateBinaryOp(ctx, constant, SYN_BINARY_OP_ADD, last, new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(constant, ctx.typeInt, 1))));
		}
		else
		{
			value = new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, ctx.typeInt, 0);
		}

		if(!value)
			Stop(ctx, source->pos, "ERROR: expression didn't evaluate to a constant number");

		last = value;

		for(ConstantData *curr = target.head; curr; curr = curr->next)
		{
			if(constant->name == curr->name)
				Stop(ctx, source->pos, "ERROR: name '%.*s' is already taken", FMT_ISTR(curr->name));
		}

		CheckVariableConflict(ctx, constant, constant->name);

		target.push_back(new (ctx.get<ConstantData>()) ConstantData(constant->name, new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, type, value->value)));
	}
}

ExprBase* AnalyzeEnumDefinition(ExpressionContext &ctx, SynEnumDefinition *syntax)
{
	InplaceStr typeName = GetTypeNameInScope(ctx, ctx.scope, syntax->name);

	TypeEnum *enumType = new (ctx.get<TypeEnum>()) TypeEnum(typeName, syntax, ctx.scope);

	ctx.AddType(enumType);

	ctx.PushScope(enumType);

	enumType->typeScope = ctx.scope;

	AnalyzeEnumConstants(ctx, syntax, enumType, syntax->values, enumType->constants);

	enumType->alignment = ctx.typeInt->alignment;

	ctx.PopScope(SCOPE_TYPE);
	
	ScopeData *scope = ctx.scope;

	// Switch to global scope
	ctx.SwitchToScopeAtPoint(NULL, ctx.globalScope, NULL);

	ExprBase *castToInt = NULL;
	ExprBase *castToEnum = NULL;

	jmp_buf prevErrorHandler;
	memcpy(&prevErrorHandler, &ctx.errorHandler, sizeof(jmp_buf));

	bool prevErrorHandlerNested = ctx.errorHandlerNested;
	ctx.errorHandlerNested = true;

	if(!setjmp(ctx.errorHandler))
	{
		// Create conversion operator int int(enum_type)
		{
			SmallArray<ArgumentData, 32> arguments(ctx.allocator);
			arguments.push_back(ArgumentData(syntax, false, InplaceStr("$x"), enumType, NULL));

			FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, syntax, ctx.scope, false, false, false, ctx.GetFunctionType(syntax, ctx.typeInt, arguments), ctx.GetReferenceType(ctx.typeVoid), SynIdentifier(InplaceStr("int")), IntrusiveList<MatchData>(), ctx.uniqueFunctionId++);

			// Fill in argument data
			for(unsigned i = 0; i < arguments.size(); i++)
				function->arguments.push_back(arguments[i]);

			CheckFunctionConflict(ctx, syntax, function->name.name);

			ctx.AddFunction(function);

			ctx.PushScope(function);

			function->functionScope = ctx.scope;

			IntrusiveList<ExprVariableDefinition> variables;

			CreateFunctionArgumentVariables(ctx, syntax, function, arguments, variables);

			ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, syntax, function);

			function->argumentsSize = function->functionScope->dataSize;

			IntrusiveList<ExprBase> expressions;
			expressions.push_back(new (ctx.get<ExprReturn>()) ExprReturn(syntax, ctx.typeVoid, new (ctx.get<ExprTypeCast>()) ExprTypeCast(syntax, ctx.typeInt, new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(syntax, enumType, variables.tail->variable), EXPR_CAST_REINTERPRET), NULL, CreateFunctionUpvalueClose(ctx, syntax, function, ctx.scope)));

			ClosePendingUpvalues(ctx, function);

			ctx.PopScope(SCOPE_FUNCTION);

			function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(syntax, function->type, function, contextArgumentDefinition, variables, NULL, expressions, NULL);

			ctx.definitions.push_back(function->declaration);

			castToInt = function->declaration;
		}

		// Create conversion operator enum_type enum_type(int)
		{
			SmallArray<ArgumentData, 32> arguments(ctx.allocator);
			arguments.push_back(ArgumentData(syntax, false, InplaceStr("$x"), ctx.typeInt, NULL));

			FunctionData *function = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, syntax, ctx.scope, false, false, false, ctx.GetFunctionType(syntax, enumType, arguments), ctx.GetReferenceType(ctx.typeVoid), SynIdentifier(typeName), IntrusiveList<MatchData>(), ctx.uniqueFunctionId++);

			// Fill in argument data
			for(unsigned i = 0; i < arguments.size(); i++)
				function->arguments.push_back(arguments[i]);

			CheckFunctionConflict(ctx, syntax, function->name.name);

			ctx.AddFunction(function);

			ctx.PushScope(function);

			function->functionScope = ctx.scope;

			IntrusiveList<ExprVariableDefinition> variables;

			CreateFunctionArgumentVariables(ctx, syntax, function, arguments, variables);

			ExprVariableDefinition *contextArgumentDefinition = CreateFunctionContextArgument(ctx, syntax, function);

			function->argumentsSize = function->functionScope->dataSize;

			IntrusiveList<ExprBase> expressions;
			expressions.push_back(new (ctx.get<ExprReturn>()) ExprReturn(syntax, ctx.typeVoid, new (ctx.get<ExprTypeCast>()) ExprTypeCast(syntax, enumType, new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(syntax, ctx.typeInt, variables.tail->variable), EXPR_CAST_REINTERPRET), NULL, CreateFunctionUpvalueClose(ctx, syntax, function, ctx.scope)));

			ClosePendingUpvalues(ctx, function);

			ctx.PopScope(SCOPE_FUNCTION);

			function->declaration = new (ctx.get<ExprFunctionDefinition>()) ExprFunctionDefinition(syntax, function->type, function, contextArgumentDefinition, variables, NULL, expressions, NULL);

			ctx.definitions.push_back(function->declaration);

			castToEnum = function->declaration;
		}
	}
	else
	{
		// Restore old scope
		ctx.SwitchToScopeAtPoint(NULL, scope, NULL);

		memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
		ctx.errorHandlerNested = prevErrorHandlerNested;

		longjmp(ctx.errorHandler, 1);
	}

	// Restore old scope
	ctx.SwitchToScopeAtPoint(NULL, scope, NULL);

	memcpy(&ctx.errorHandler, &prevErrorHandler, sizeof(jmp_buf));
	ctx.errorHandlerNested = prevErrorHandlerNested;

	return new (ctx.get<ExprEnumDefinition>()) ExprEnumDefinition(syntax, ctx.typeVoid, enumType, castToInt, castToEnum);
}

ExprBlock* AnalyzeNamespaceDefinition(ExpressionContext &ctx, SynNamespaceDefinition *syntax)
{
	if(ctx.scope != ctx.globalScope && ctx.scope->ownerNamespace == NULL)
		Stop(ctx, syntax->pos, "ERROR: a namespace definition must appear either at file scope or immediately within another namespace definition");

	for(SynIdentifier *name = syntax->path.head; name; name = getType<SynIdentifier>(name->next))
	{
		NamespaceData *parent = ctx.GetCurrentNamespace();

		NamespaceData *ns = new (ctx.get<NamespaceData>()) NamespaceData(ctx.allocator, syntax, ctx.scope, parent, name->name, ctx.uniqueNamespaceId++);

		CheckNamespaceConflict(ctx, syntax, ns);

		if(parent)
			parent->children.push_back(ns);
		else
			ctx.globalNamespaces.push_back(ns);

		ctx.namespaces.push_back(ns);

		ctx.PushScope(ns);
	}

	IntrusiveList<ExprBase> expressions;

	for(SynBase *expression = syntax->expressions.head; expression; expression = expression->next)
		expressions.push_back(AnalyzeStatement(ctx, expression));

	ExprBlock *block = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, expressions, NULL);

	for(SynIdentifier *name = syntax->path.head; name; name = getType<SynIdentifier>(name->next))
		ctx.PopScope(SCOPE_NAMESPACE);

	return block;
}

ExprAliasDefinition* AnalyzeTypedef(ExpressionContext &ctx, SynTypedef *syntax)
{
	if(ctx.typeMap.find(syntax->alias.hash()))
		Stop(ctx, syntax->pos, "ERROR: there is already a type or an alias with the same name");

	TypeBase *type = AnalyzeType(ctx, syntax->type);

	if(type == ctx.typeAuto)
		Stop(ctx, syntax->pos, "ERROR: can't alias 'auto' type");

	AliasData *alias = new (ctx.get<AliasData>()) AliasData(syntax, ctx.scope, type, syntax->alias, ctx.uniqueAliasId++);

	ctx.AddAlias(alias);

	return new (ctx.get<ExprAliasDefinition>()) ExprAliasDefinition(syntax, ctx.typeVoid, alias);
}

ExprBase* AnalyzeIfElse(ExpressionContext &ctx, SynIfElse *syntax)
{
	SynVariableDefinitions *definitions = getType<SynVariableDefinitions>(syntax->condition);

	ExprBase *condition = NULL;

	if(definitions)
	{
		ctx.PushScope(SCOPE_EXPLICIT);

		assert(definitions->definitions.size() == 1);

		TypeBase *type = AnalyzeType(ctx, definitions->type);

		ExprBase *definition = AnalyzeVariableDefinition(ctx, definitions->definitions.head, 0, type);

		if(isType<ExprError>(definition))
		{
			condition = new (ctx.get<ExprError>()) ExprError(syntax, ctx.typeBool);
		}
		else
		{
			ExprVariableDefinition *variableDefinition = getType<ExprVariableDefinition>(definition);

			assert(variableDefinition);

			condition = CreateSequence(ctx, syntax, definition, CreateVariableAccess(ctx, syntax, variableDefinition->variable, false));
		}
	}
	else
	{
		condition = AnalyzeExpression(ctx, syntax->condition);
	}

	condition = CreateConditionCast(ctx, condition->source, condition);

	if(syntax->staticIf)
	{
		if(ExprBoolLiteral *number = getType<ExprBoolLiteral>(EvaluateExpression(ctx, CreateCast(ctx, syntax, condition, ctx.typeBool, false))))
		{
			if(number->value)
			{
				if(SynBlock *node = getType<SynBlock>(syntax->trueBlock))
					return AnalyzeBlock(ctx, node, false);
				else
					return AnalyzeStatement(ctx, syntax->trueBlock);
			}
			else if(syntax->falseBlock)
			{
				if(SynBlock *node = getType<SynBlock>(syntax->falseBlock))
					return AnalyzeBlock(ctx, node, false);
				else
					return AnalyzeStatement(ctx, syntax->falseBlock);
			}

			return new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid);
		}

		Stop(ctx, syntax->pos, "ERROR: couldn't evaluate condition at compilation time");
	}

	ExprBase *trueBlock = AnalyzeStatement(ctx, syntax->trueBlock);

	if(definitions)
		ctx.PopScope(SCOPE_EXPLICIT);

	ExprBase *falseBlock = syntax->falseBlock ? AnalyzeStatement(ctx, syntax->falseBlock) : NULL;

	return new (ctx.get<ExprIfElse>()) ExprIfElse(syntax, ctx.typeVoid, condition, trueBlock, falseBlock);
}

ExprFor* AnalyzeFor(ExpressionContext &ctx, SynFor *syntax)
{
	ctx.PushLoopScope(true, true);

	ExprBase *initializer = NULL;

	if(SynBlock *block = getType<SynBlock>(syntax->initializer))
		initializer = AnalyzeBlock(ctx, block, false);
	else if(syntax->initializer)
		initializer = AnalyzeStatement(ctx, syntax->initializer);
	else
		initializer = new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid);

	ExprBase *condition = AnalyzeExpression(ctx, syntax->condition);
	ExprBase *increment = syntax->increment ? AnalyzeStatement(ctx, syntax->increment) : new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid);
	ExprBase *body = syntax->body ? AnalyzeStatement(ctx, syntax->body) : new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid);

	condition = CreateConditionCast(ctx, condition->source, condition);

	IntrusiveList<ExprBase> iteration;

	if(ExprBase *closures = CreateBlockUpvalueClose(ctx, syntax, ctx.GetCurrentFunction(), ctx.scope))
		iteration.push_back(closures);

	iteration.push_back(increment);

	ExprBlock *block = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, iteration, NULL);

	ctx.PopScope(SCOPE_LOOP);

	return new (ctx.get<ExprFor>()) ExprFor(syntax, ctx.typeVoid, initializer, condition, block, body);
}

ExprFor* AnalyzeForEach(ExpressionContext &ctx, SynForEach *syntax)
{
	ctx.PushLoopScope(true, true);

	IntrusiveList<ExprBase> initializers;
	IntrusiveList<ExprBase> conditions;
	IntrusiveList<ExprBase> definitions;
	IntrusiveList<ExprBase> increments;

	for(SynForEachIterator *curr = syntax->iterators.head; curr; curr = getType<SynForEachIterator>(curr->next))
	{
		ExprBase *value = AnalyzeExpression(ctx, curr->value);

		TypeBase *type = NULL;

		if(curr->type)
			type = AnalyzeType(ctx, curr->type);

		// Special implementation of for each for built-in arrays
		if(isType<TypeArray>(value->type) || isType<TypeUnsizedArray>(value->type))
		{
			if(!type)
			{
				if(TypeArray *valueType = getType<TypeArray>(value->type))
					type = valueType->subType;
				else if(TypeUnsizedArray *valueType = getType<TypeUnsizedArray>(value->type))
					type = valueType->subType;
			}

			ExprBase* wrapped = value;

			if(ExprVariableAccess *node = getType<ExprVariableAccess>(value))
			{
				wrapped = new (ctx.get<ExprGetAddress>()) ExprGetAddress(value->source, ctx.GetReferenceType(value->type), node->variable);
			}
			else if(ExprDereference *node = getType<ExprDereference>(value))
			{
				wrapped = node->value;
			}
			else if(!isType<TypeRef>(wrapped->type))
			{
				VariableData *storage = AllocateTemporary(ctx, value->source, wrapped->type);

				ExprBase *assignment = CreateAssignment(ctx, value->source, CreateVariableAccess(ctx, value->source, storage, false), value);

				ExprBase *definition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(value->source, ctx.typeVoid, storage, assignment);

				wrapped = CreateSequence(ctx, value->source, definition, CreateGetAddress(ctx, value->source, CreateVariableAccess(ctx, value->source, storage, false)));
			}

			// Create initializer
			VariableData *iterator = AllocateTemporary(ctx, curr, ctx.typeInt);

			ExprBase *iteratorAssignment = CreateAssignment(ctx, curr, CreateVariableAccess(ctx, curr, iterator, false), new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(curr, ctx.typeInt, 0));

			initializers.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(curr, ctx.typeVoid, iterator, iteratorAssignment));

			// Create condition
			conditions.push_back(CreateBinaryOp(ctx, curr, SYN_BINARY_OP_LESS, CreateVariableAccess(ctx, curr, iterator, false), CreateMemberAccess(ctx, curr, value, InplaceStr("size"), false)));

			// Create definition
			type = ctx.GetReferenceType(type);

			CheckVariableConflict(ctx, curr, curr->name);

			unsigned variableOffset = AllocateVariableInScope(ctx, curr, type->alignment, type);
			VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, curr, ctx.scope, type->alignment, type, curr->name, variableOffset, ctx.uniqueVariableId++);

			variable->isReference = true;

			if (IsLookupOnlyVariable(ctx, variable))
				variable->lookupOnly = true;

			ctx.AddVariable(variable);

			SmallArray<ArgumentData, 32> arguments(ctx.allocator);
			arguments.push_back(ArgumentData(curr, false, InplaceStr(), ctx.typeInt, CreateVariableAccess(ctx, curr, iterator, false)));

			ExprBase *arrayIndex = CreateArrayIndex(ctx, curr, value, arguments);

			assert(isType<ExprDereference>(arrayIndex));

			if(ExprDereference *node = getType<ExprDereference>(arrayIndex))
				arrayIndex = node->value;

			definitions.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(curr, ctx.typeVoid, variable, CreateAssignment(ctx, curr, CreateVariableAccess(ctx, curr, variable, false), arrayIndex)));

			// Create increment
			increments.push_back(new (ctx.get<ExprPreModify>()) ExprPreModify(curr, ctx.typeInt, CreateGetAddress(ctx, curr, CreateVariableAccess(ctx, curr, iterator, false)), true));
			continue;
		}

		TypeFunction *functionType = getType<TypeFunction>(value->type);
		ExprBase *startCall = NULL;
		
		// If we don't have a function, get an iterator
		if(!functionType)
		{
			startCall = CreateFunctionCall(ctx, curr, CreateMemberAccess(ctx, curr, value, InplaceStr("start"), false), IntrusiveList<TypeHandle>(), NULL, false);

			// Check if iteartor is a coroutine
			functionType = getType<TypeFunction>(startCall->type);

			if(functionType)
				value = startCall;
		}

		if(functionType)
		{
			// Store function pointer in a variable
			VariableData *functPtr = AllocateTemporary(ctx, curr, value->type);

			initializers.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(curr, ctx.typeVoid, functPtr, CreateAssignment(ctx, curr, CreateVariableAccess(ctx, curr, functPtr, false), value)));

			if(ExprFunctionAccess *access = getType<ExprFunctionAccess>(value))
			{
				if(!access->function->coroutine)
					Stop(ctx, curr->pos, "ERROR: function is not a coroutine");
			}
			else
			{
				initializers.push_back(CreateFunctionCall1(ctx, curr, InplaceStr("__assertCoroutine"), CreateVariableAccess(ctx, curr, functPtr, false), false, true));
			}

			// Create definition
			if(!type)
				type = functionType->returnType;

			CheckVariableConflict(ctx, curr, curr->name);

			unsigned variableOffset = AllocateVariableInScope(ctx, curr, type->alignment, type);
			VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, curr, ctx.scope, type->alignment, type, curr->name, variableOffset, ctx.uniqueVariableId++);

			if (IsLookupOnlyVariable(ctx, variable))
				variable->lookupOnly = true;

			ctx.AddVariable(variable);

			if(ExprBase *call = CreateFunctionCall(ctx, curr, CreateVariableAccess(ctx, curr, functPtr, false), IntrusiveList<TypeHandle>(), NULL, false))
			{
				if(ctx.GetReferenceType(type) == call->type)
					call = new (ctx.get<ExprDereference>()) ExprDereference(curr, type, call);

				initializers.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(curr, ctx.typeVoid, variable, CreateAssignment(ctx, curr, CreateVariableAccess(ctx, curr, variable, false), call)));
			}

			// Create condition
			conditions.push_back(new (ctx.get<ExprUnaryOp>()) ExprUnaryOp(curr, ctx.typeBool, SYN_UNARY_OP_LOGICAL_NOT, CreateFunctionCall1(ctx, curr, InplaceStr("isCoroutineReset"), CreateVariableAccess(ctx, curr, functPtr, false), false, false)));

			// Create increment
			if(ExprBase *call = CreateFunctionCall(ctx, curr, CreateVariableAccess(ctx, curr, functPtr, false), IntrusiveList<TypeHandle>(), NULL, false))
			{
				if(ctx.GetReferenceType(type) == call->type)
					call = new (ctx.get<ExprDereference>()) ExprDereference(curr, type, call);

				increments.push_back(CreateAssignment(ctx, curr, CreateVariableAccess(ctx, curr, variable, false), call));
			}
		}
		else
		{
			// Store iterator in a variable
			VariableData *iterator = AllocateTemporary(ctx, curr, startCall->type);

			initializers.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(curr, ctx.typeVoid, iterator, CreateAssignment(ctx, curr, CreateVariableAccess(ctx, curr, iterator, false), startCall)));

			// Create condition
			conditions.push_back(CreateFunctionCall(ctx, curr, CreateMemberAccess(ctx, curr, CreateVariableAccess(ctx, curr, iterator, false), InplaceStr("hasnext"), false), IntrusiveList<TypeHandle>(), NULL, false));

			// Create definition
			ExprBase *call = CreateFunctionCall(ctx, curr, CreateMemberAccess(ctx, curr, CreateVariableAccess(ctx, curr, iterator, false), InplaceStr("next"), false), IntrusiveList<TypeHandle>(), NULL, false);

			if(!type)
				type = call->type;
			else if(type == ctx.typeAuto)
				type = call->type;
			else
				type = ctx.GetReferenceType(type);

			CheckVariableConflict(ctx, curr, curr->name);

			unsigned variableOffset = AllocateVariableInScope(ctx, curr, type->alignment, type);
			VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, curr, ctx.scope, type->alignment, type, curr->name, variableOffset, ctx.uniqueVariableId++);

			variable->isReference = isType<TypeRef>(type);

			if (IsLookupOnlyVariable(ctx, variable))
				variable->lookupOnly = true;

			ctx.AddVariable(variable);

			definitions.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(curr, ctx.typeVoid, variable, CreateAssignment(ctx, curr, CreateVariableAccess(ctx, curr, variable, false), call)));
		}
	}

	ExprBase *initializer = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, initializers, NULL);

	ExprBase *condition = NULL;

	for(ExprBase *curr = conditions.head; curr; curr = curr->next)
	{
		if(!condition)
			condition = curr;
		else
			condition = CreateBinaryOp(ctx, syntax, SYN_BINARY_OP_LOGICAL_AND, condition, curr);
	}

	ExprBase *increment = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, increments, NULL);

	if(syntax->body)
		definitions.push_back(AnalyzeStatement(ctx, syntax->body));

	ExprBase *body = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, definitions, CreateBlockUpvalueClose(ctx, syntax, ctx.GetCurrentFunction(), ctx.scope));

	ctx.PopScope(SCOPE_LOOP);

	return new (ctx.get<ExprFor>()) ExprFor(syntax, ctx.typeVoid, initializer, condition, increment, body);
}

ExprWhile* AnalyzeWhile(ExpressionContext &ctx, SynWhile *syntax)
{
	ctx.PushLoopScope(true, true);

	ExprBase *condition = AnalyzeExpression(ctx, syntax->condition);
	ExprBase *body = syntax->body ? AnalyzeStatement(ctx, syntax->body) : new (ctx.get<ExprVoid>()) ExprVoid(syntax, ctx.typeVoid);

	condition = CreateConditionCast(ctx, condition->source, condition);

	ctx.PopScope(SCOPE_LOOP);

	return new (ctx.get<ExprWhile>()) ExprWhile(syntax, ctx.typeVoid, condition, body);
}

ExprDoWhile* AnalyzeDoWhile(ExpressionContext &ctx, SynDoWhile *syntax)
{
	ctx.PushLoopScope(true, true);

	IntrusiveList<ExprBase> expressions;

	for(SynBase *expression = syntax->expressions.head; expression; expression = expression->next)
		expressions.push_back(AnalyzeStatement(ctx, expression));

	ExprBase *condition = AnalyzeExpression(ctx, syntax->condition);

	condition = CreateConditionCast(ctx, condition->source, condition);

	ExprBase *block = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, expressions, CreateBlockUpvalueClose(ctx, syntax, ctx.GetCurrentFunction(), ctx.scope));

	ctx.PopScope(SCOPE_LOOP);

	return new (ctx.get<ExprDoWhile>()) ExprDoWhile(syntax, ctx.typeVoid, block, condition);
}

ExprSwitch* AnalyzeSwitch(ExpressionContext &ctx, SynSwitch *syntax)
{
	ctx.PushLoopScope(true, false);

	ExprBase *condition = AnalyzeExpression(ctx, syntax->condition);

	if(condition->type == ctx.typeVoid)
		Stop(ctx, syntax->condition->pos, "ERROR: condition type cannot be '%.*s'", FMT_ISTR(condition->type->name));

	VariableData *conditionVariable = AllocateTemporary(ctx, syntax, condition->type);

	condition = new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax->condition, ctx.typeVoid, conditionVariable, CreateAssignment(ctx, syntax->condition, CreateVariableAccess(ctx, syntax, conditionVariable, false), condition));

	IntrusiveList<ExprBase> cases;
	IntrusiveList<ExprBase> blocks;
	ExprBase *defaultBlock = NULL;

	for(SynSwitchCase *curr = syntax->cases.head; curr; curr = getType<SynSwitchCase>(curr->next))
	{
		if(curr->value)
		{
			ExprBase *caseValue = AnalyzeExpression(ctx, curr->value);

			if(caseValue->type == ctx.typeVoid)
				Stop(ctx, syntax->condition->pos, "ERROR: case value type cannot be '%.*s'", FMT_ISTR(caseValue->type->name));

			ExprBase *condition = CreateBinaryOp(ctx, curr->value, SYN_BINARY_OP_EQUAL, caseValue, CreateVariableAccess(ctx, syntax, conditionVariable, false));

			if(!ctx.IsIntegerType(condition->type) || condition->type == ctx.typeLong)
				Stop(ctx, curr->pos, "ERROR: '==' operator result type must be bool, char, short or int");

			cases.push_back(condition);
		}

		IntrusiveList<ExprBase> expressions;

		for(SynBase *expression = curr->expressions.head; expression; expression = expression->next)
			expressions.push_back(AnalyzeStatement(ctx, expression));

		ExprBase *block = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, expressions, NULL);

		if(curr->value)
			blocks.push_back(block);
		else
			defaultBlock = block;
	}

	ctx.PopScope(SCOPE_LOOP);

	return new (ctx.get<ExprSwitch>()) ExprSwitch(syntax, ctx.typeVoid, condition, cases, blocks, defaultBlock);
}

ExprBreak* AnalyzeBreak(ExpressionContext &ctx, SynBreak *syntax)
{
	unsigned depth = 1;

	if(syntax->number)
	{
		ExprBase *numberValue = AnalyzeExpression(ctx, syntax->number);

		if(ExprIntegerLiteral *number = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateCast(ctx, syntax->number, numberValue, ctx.typeLong, false))))
		{
			if(number->value <= 0)
				Stop(ctx, syntax->number->pos, "ERROR: break level can't be negative or zero");

			depth = unsigned(number->value);
		}
		else
		{
			Stop(ctx, syntax->number->pos, "ERROR: break statement must be followed by ';' or a constant");
		}
	}

	if(ctx.scope->breakDepth < depth)
		Stop(ctx, syntax->pos, "ERROR: break level is greater that loop depth");

	return new (ctx.get<ExprBreak>()) ExprBreak(syntax, ctx.typeVoid, depth, CreateBreakUpvalueClose(ctx, syntax, ctx.GetCurrentFunction(), ctx.scope, depth));
}

ExprContinue* AnalyzeContinue(ExpressionContext &ctx, SynContinue *syntax)
{
	unsigned depth = 1;

	if(syntax->number)
	{
		ExprBase *numberValue = AnalyzeExpression(ctx, syntax->number);

		if(ExprIntegerLiteral *number = getType<ExprIntegerLiteral>(EvaluateExpression(ctx, CreateCast(ctx, syntax->number, numberValue, ctx.typeLong, false))))
		{
			if(number->value <= 0)
				Stop(ctx, syntax->number->pos, "ERROR: continue level can't be negative or zero");

			depth = unsigned(number->value);
		}
		else
		{
			Stop(ctx, syntax->number->pos, "ERROR: continue statement must be followed by ';' or a constant");
		}
	}

	if(ctx.scope->contiueDepth < depth)
		Stop(ctx, syntax->pos, "ERROR: continue level is greater that loop depth");

	return new (ctx.get<ExprContinue>()) ExprContinue(syntax, ctx.typeVoid, depth, CreateContinueUpvalueClose(ctx, syntax, ctx.GetCurrentFunction(), ctx.scope, depth));
}

ExprBlock* AnalyzeBlock(ExpressionContext &ctx, SynBlock *syntax, bool createScope)
{
	if(createScope)
	{
		ctx.PushScope(SCOPE_EXPLICIT);

		IntrusiveList<ExprBase> expressions;

		for(SynBase *expression = syntax->expressions.head; expression; expression = expression->next)
			expressions.push_back(AnalyzeStatement(ctx, expression));

		ExprBlock *block = new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, expressions, CreateBlockUpvalueClose(ctx, syntax, ctx.GetCurrentFunction(), ctx.scope));

		ctx.PopScope(SCOPE_EXPLICIT);

		return block;
	}

	IntrusiveList<ExprBase> expressions;

	for(SynBase *expression = syntax->expressions.head; expression; expression = expression->next)
		expressions.push_back(AnalyzeStatement(ctx, expression));

	return new (ctx.get<ExprBlock>()) ExprBlock(syntax, ctx.typeVoid, expressions, NULL);
}

ExprBase* AnalyzeExpression(ExpressionContext &ctx, SynBase *syntax)
{
	if(SynBool *node = getType<SynBool>(syntax))
	{
		return new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(node, ctx.typeBool, node->value);
	}

	if(SynCharacter *node = getType<SynCharacter>(syntax))
	{
		unsigned char result = (unsigned char)node->value.begin[1];

		if(result == '\\')
			result = ParseEscapeSequence(ctx, node->value.begin + 1);

		return new (ctx.get<ExprCharacterLiteral>()) ExprCharacterLiteral(node, ctx.typeChar, result);
	}

	if(SynString *node = getType<SynString>(syntax))
	{
		unsigned length = 0;

		if(node->rawLiteral)
		{
			length = node->value.length() - 2;
		}
		else
		{
			// Find the length of the string with collapsed escape-sequences
			for(const char *curr = node->value.begin + 1, *end = node->value.end - 1 ; curr < end; curr++, length++)
			{
				if(*curr == '\\')
					curr++;
			}
		}

		unsigned memory = length ? ((length + 1) + 3) & ~3 : 4;

		char *value = (char*)ctx.allocator->alloc(memory);

		for(unsigned i = length; i < memory; i++)
			value[i] = 0;

		if(node->rawLiteral)
		{
			for(unsigned i = 0; i < length; i++)
				value[i] = node->value.begin[i + 1];

			value[length] = 0;
		}
		else
		{
			unsigned i = 0;

			// Find the length of the string with collapsed escape-sequences
			for(const char *curr = node->value.begin + 1, *end = node->value.end - 1 ; curr < end;)
			{
				if(*curr == '\\')
				{
					value[i++] = ParseEscapeSequence(ctx, curr);
					curr += 2;
				}
				else
				{
					value[i++] = *curr;
					curr += 1;
				}
			}

			value[length] = 0;
		}

		return new (ctx.get<ExprStringLiteral>()) ExprStringLiteral(node, ctx.GetArrayType(ctx.typeChar, length + 1), value, length);
	}
	
	if(SynNullptr *node = getType<SynNullptr>(syntax))
	{
		return new (ctx.get<ExprNullptrLiteral>()) ExprNullptrLiteral(node, ctx.typeNullPtr);
	}

	if(SynNumber *node = getType<SynNumber>(syntax))
	{
		return AnalyzeNumber(ctx, node);
	}

	if(SynArray *node = getType<SynArray>(syntax))
	{
		return AnalyzeArray(ctx, node);
	}

	if(SynPreModify *node = getType<SynPreModify>(syntax))
	{
		return AnalyzePreModify(ctx, node);
	}

	if(SynPostModify *node = getType<SynPostModify>(syntax))
	{
		return AnalyzePostModify(ctx, node);
	}

	if(SynUnaryOp *node = getType<SynUnaryOp>(syntax))
	{
		return AnalyzeUnaryOp(ctx, node);
	}

	if(SynBinaryOp *node = getType<SynBinaryOp>(syntax))
	{
		return AnalyzeBinaryOp(ctx, node);
	}
	
	if(SynGetAddress *node = getType<SynGetAddress>(syntax))
	{
		return AnalyzeGetAddress(ctx, node);
	}

	if(SynDereference *node = getType<SynDereference>(syntax))
	{
		return AnalyzeDereference(ctx, node);
	}

	if(SynTypeof *node = getType<SynTypeof>(syntax))
	{
		ExprBase *value = AnalyzeExpression(ctx, node->value);

		if(value->type == ctx.typeAuto)
			Stop(ctx, syntax->pos, "ERROR: cannot take typeid from auto type");

		if(isType<ExprTypeLiteral>(value))
			return value;

		ResolveInitializerValue(ctx, syntax, value);

		assert(!isType<TypeArgumentSet>(value->type) && !isType<TypeMemberSet>(value->type) && !isType<TypeFunctionSet>(value->type));

		return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(node, ctx.typeTypeID, value->type);
	}

	if(SynIdentifier *node = getType<SynIdentifier>(syntax))
	{
		return AnalyzeVariableAccess(ctx, node);
	}

	if(SynTypeSimple *node = getType<SynTypeSimple>(syntax))
	{
		// It could be a typeid
		if(TypeBase *type = AnalyzeType(ctx, node, false))
		{
			if(type == ctx.typeAuto)
				Stop(ctx, syntax->pos, "ERROR: cannot take typeid from auto type");

			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(node, ctx.typeTypeID, type);
		}

		return AnalyzeVariableAccess(ctx, node);
	}

	if(SynSizeof *node = getType<SynSizeof>(syntax))
	{
		if(TypeBase *type = AnalyzeType(ctx, node->value, false))
		{
			if(type->isGeneric)
				Stop(ctx, syntax->pos, "ERROR: sizeof generic type is illegal");

			if(type == ctx.typeAuto)
				Stop(ctx, syntax->pos, "ERROR: sizeof auto type is illegal");

			if(TypeClass *typeClass = getType<TypeClass>(type))
			{
				if(!typeClass->completed)
					Stop(ctx, syntax->pos, "ERROR: type '%.*s' is not fully defined", FMT_ISTR(type->name));
			}

			assert(!isType<TypeArgumentSet>(type) && !isType<TypeMemberSet>(type) && !isType<TypeFunctionSet>(type));

			return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(node, ctx.typeInt, type->size);
		}

		ExprBase *value = AnalyzeExpression(ctx, node->value);

		if(value->type == ctx.typeAuto)
			Stop(ctx, syntax->pos, "ERROR: sizeof auto type is illegal");

		if(TypeClass *typeClass = getType<TypeClass>(value->type))
		{
			if(!typeClass->completed)
				Stop(ctx, syntax->pos, "ERROR: type '%.*s' is not fully defined", FMT_ISTR(value->type->name));
		}

		ResolveInitializerValue(ctx, syntax, value);

		assert(!isType<TypeArgumentSet>(value->type) && !isType<TypeMemberSet>(value->type) && !isType<TypeFunctionSet>(value->type));

		return new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(node, ctx.typeInt, value->type->size);
	}

	if(SynConditional *node = getType<SynConditional>(syntax))
	{
		return AnalyzeConditional(ctx, node);
	}

	if(SynAssignment *node = getType<SynAssignment>(syntax))
	{
		return AnalyzeAssignment(ctx, node);
	}

	if(SynModifyAssignment *node = getType<SynModifyAssignment>(syntax))
	{
		return AnalyzeModifyAssignment(ctx, node);
	}

	if(SynMemberAccess *node = getType<SynMemberAccess>(syntax))
	{
		// It could be a typeid
		if(TypeBase *type = AnalyzeType(ctx, syntax, false))
		{
			if(type == ctx.typeAuto)
				Stop(ctx, syntax->pos, "ERROR: cannot take typeid from auto type");

			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(node, ctx.typeTypeID, type);
		}

		return AnalyzeMemberAccess(ctx, node);
	}

	if(SynTypeArray *node = getType<SynTypeArray>(syntax))
	{
		// It could be a typeid
		if(TypeBase *type = AnalyzeType(ctx, syntax, false))
		{
			if(type == ctx.typeAuto)
				Stop(ctx, syntax->pos, "ERROR: cannot take typeid from auto type");

			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(node, ctx.typeTypeID, type);
		}

		return AnalyzeArrayIndex(ctx, node);
	}

	if(SynArrayIndex *node = getType<SynArrayIndex>(syntax))
	{
		return AnalyzeArrayIndex(ctx, node);
	}

	if(SynFunctionCall *node = getType<SynFunctionCall>(syntax))
	{
		return AnalyzeFunctionCall(ctx, node);
	}

	if(SynNew *node = getType<SynNew>(syntax))
	{
		return AnalyzeNew(ctx, node);
	}

	if(SynFunctionDefinition *node = getType<SynFunctionDefinition>(syntax))
	{
		return AnalyzeFunctionDefinition(ctx, node, NULL, NULL, IntrusiveList<MatchData>(), true, true, true);
	}

	if(isType<SynShortFunctionDefinition>(syntax))
	{
		Stop(ctx, syntax->pos, "ERROR: cannot infer type for inline function outside of the function call");
	}

	if(SynGenerator *node = getType<SynGenerator>(syntax))
	{
		return AnalyzeGenerator(ctx, node);
	}

	if(SynTypeReference *node = getType<SynTypeReference>(syntax))
		return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(node, ctx.typeTypeID, AnalyzeType(ctx, syntax));

	if(SynTypeFunction *node = getType<SynTypeFunction>(syntax))
	{
		if(TypeBase *type = AnalyzeType(ctx, syntax, false))
			return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(node, ctx.typeTypeID, type);

		// Transform 'type ref(arguments)' into a 'type ref' constructor call
		SynBase* value = new (ctx.get<SynTypeReference>()) SynTypeReference(node->begin, node->end, node->returnType);

		IntrusiveList<SynCallArgument> arguments;

		for(SynBase *curr = node->arguments.head; curr; curr = curr->next)
			arguments.push_back(new (ctx.get<SynCallArgument>()) SynCallArgument(curr->begin, curr->end, InplaceStr(), curr));

		return AnalyzeFunctionCall(ctx, new (ctx.get<SynFunctionCall>()) SynFunctionCall(node->begin, node->end, value, IntrusiveList<SynBase>(), arguments));
	}

	if(SynTypeGenericInstance *node = getType<SynTypeGenericInstance>(syntax))
		return new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(node, ctx.typeTypeID, AnalyzeType(ctx, syntax));

	if(isType<SynTypeAuto>(syntax))
		Stop(ctx, syntax->pos, "ERROR: cannot take typeid from auto type");

	if(isType<SynTypeAlias>(syntax))
		Stop(ctx, syntax->pos, "ERROR: cannot take typeid from generic type");

	if(isType<SynTypeGeneric>(syntax))
		Stop(ctx, syntax->pos, "ERROR: cannot take typeid from generic type");

	Stop(ctx, syntax->pos, "ERROR: unknown expression type");

	return NULL;
}

ExprBase* AnalyzeStatement(ExpressionContext &ctx, SynBase *syntax)
{
	if(SynReturn *node = getType<SynReturn>(syntax))
	{
		return AnalyzeReturn(ctx, node);
	}

	if(SynYield *node = getType<SynYield>(syntax))
	{
		return AnalyzeYield(ctx, node);
	}

	if(SynVariableDefinitions *node = getType<SynVariableDefinitions>(syntax))
	{
		return AnalyzeVariableDefinitions(ctx, node);
	}

	if(SynFunctionDefinition *node = getType<SynFunctionDefinition>(syntax))
	{
		return AnalyzeFunctionDefinition(ctx, node, NULL, NULL, IntrusiveList<MatchData>(), true, false, true);
	}

	if(SynClassDefinition *node = getType<SynClassDefinition>(syntax))
	{
		IntrusiveList<TypeHandle> generics;

		return AnalyzeClassDefinition(ctx, node, NULL, generics);
	}

	if(SynClassPrototype *node = getType<SynClassPrototype>(syntax))
	{
		return AnalyzeClassPrototype(ctx, node);
	}

	if(SynEnumDefinition *node = getType<SynEnumDefinition>(syntax))
	{
		return AnalyzeEnumDefinition(ctx, node);
	}

	if(SynNamespaceDefinition *node = getType<SynNamespaceDefinition>(syntax))
	{
		return AnalyzeNamespaceDefinition(ctx, node);
	}

	if(SynTypedef *node = getType<SynTypedef>(syntax))
	{
		return AnalyzeTypedef(ctx, node);
	}

	if(SynIfElse *node = getType<SynIfElse>(syntax))
	{
		return AnalyzeIfElse(ctx, node);
	}

	if(SynFor *node = getType<SynFor>(syntax))
	{
		return AnalyzeFor(ctx, node);
	}

	if(SynForEach *node = getType<SynForEach>(syntax))
	{
		return AnalyzeForEach(ctx, node);
	}

	if(SynWhile *node = getType<SynWhile>(syntax))
	{
		return AnalyzeWhile(ctx, node);
	}

	if(SynDoWhile *node = getType<SynDoWhile>(syntax))
	{
		return AnalyzeDoWhile(ctx, node);
	}

	if(SynSwitch *node = getType<SynSwitch>(syntax))
	{
		return AnalyzeSwitch(ctx, node);
	}

	if(SynBreak *node = getType<SynBreak>(syntax))
	{
		return AnalyzeBreak(ctx, node);
	}

	if(SynContinue *node = getType<SynContinue>(syntax))
	{
		return AnalyzeContinue(ctx, node);
	}

	if(SynBlock *node = getType<SynBlock>(syntax))
	{
		return AnalyzeBlock(ctx, node, true);
	}

	ExprBase *expression = AnalyzeExpression(ctx, syntax);

	AssertValueExpression(ctx, syntax, expression);

	return expression;
}

struct ModuleContext
{
	ModuleContext(Allocator *allocator): types(allocator)
	{
		data = NULL;

		dependencyDepth = 1;
	}

	SmallArray<TypeBase*, 32> types;

	ModuleData *data;

	unsigned dependencyDepth;
};

void ImportModuleDependencies(ExpressionContext &ctx, SynBase *source, ModuleContext &moduleCtx, ByteCode *moduleBytecode)
{
	char *symbols = FindSymbols(moduleBytecode);

	ExternModuleInfo *moduleList = FindFirstModule(moduleBytecode);

	for(unsigned i = 0; i < moduleBytecode->dependsCount; i++)
	{
		ExternModuleInfo &moduleInfo = moduleList[i];

		const char *moduleFileName = symbols + moduleInfo.nameOffset;

		const char *importPath = BinaryCache::GetImportPath();

		InplaceStr path = GetImportPath(ctx.allocator, importPath, InplaceStr(moduleFileName));
		InplaceStr pathNoImport = importPath ? InplaceStr(path.begin + strlen(importPath)) : path;

		const char *bytecode = BinaryCache::GetBytecode(path.begin);
		unsigned lexStreamSize = 0;
		Lexeme *lexStream = BinaryCache::GetLexems(path.begin, lexStreamSize);

		if(!bytecode)
		{
			bytecode = BinaryCache::GetBytecode(pathNoImport.begin);
			lexStream = BinaryCache::GetLexems(pathNoImport.begin, lexStreamSize);
		}

		if(!bytecode)
			Stop(ctx, source->pos, "ERROR: module dependency import is not implemented");

#ifdef IMPORT_VERBOSE_DEBUG_OUTPUT
		for(unsigned k = 0; k < moduleCtx.dependencyDepth; k++)
			printf("  ");
		printf("  importing module %.*s as dependency #%d\n", FMT_ISTR(pathNoImport), ctx.dependencies.size() + 1);
#endif

		ModuleData *moduleData = new (ctx.get<ModuleData>()) ModuleData(source, pathNoImport);

		ctx.dependencies.push_back(moduleData);
		moduleData->dependencyIndex = ctx.dependencies.size();

		moduleData->bytecode = (ByteCode*)bytecode;

		if(!lexStream)
		{
			moduleData->lexer = new (ctx.get<Lexer>()) Lexer(ctx.allocator);

			moduleData->lexer->Lexify(FindSource(moduleData->bytecode));
			lexStream = moduleData->lexer->GetStreamStart();
			lexStreamSize = moduleData->lexer->GetStreamSize();
		}

		moduleData->lexStream = lexStream;
		moduleData->lexStreamSize = lexStreamSize;

		moduleCtx.dependencyDepth++;

		ImportModuleDependencies(ctx, source, moduleCtx, moduleData->bytecode);

		moduleCtx.dependencyDepth--;
	}
}

void ImportModuleNamespaces(ExpressionContext &ctx, SynBase *source, ModuleContext &moduleCtx)
{
	ByteCode *bCode = moduleCtx.data->bytecode;
	char *symbols = FindSymbols(bCode);

	// Import namespaces
	ExternNamespaceInfo *namespaceList = FindFirstNamespace(bCode);

	for(unsigned i = 0; i < bCode->namespaceCount; i++)
	{
		ExternNamespaceInfo &namespaceData = namespaceList[i];

		NamespaceData *parent = NULL;

		if(namespaceData.parentHash != ~0u)
		{
			for(unsigned k = 0; k < ctx.namespaces.size(); k++)
			{
				if(ctx.namespaces[k]->nameHash == namespaceData.parentHash)
				{
					parent = ctx.namespaces[k];
					break;
				}
			}

			if(!parent)
				Stop(ctx, source->pos, "ERROR: namespace %s parent not found", symbols + namespaceData.offsetToName);
		}

		NamespaceData *ns = new (ctx.get<NamespaceData>()) NamespaceData(ctx.allocator, source, ctx.scope, parent, InplaceStr(symbols + namespaceData.offsetToName), ctx.uniqueNamespaceId++);

		if(parent)
			parent->children.push_back(ns);
		else
			ctx.globalNamespaces.push_back(ns);

		ctx.namespaces.push_back(ns);
	}
}

struct DelayedType
{
	DelayedType(): index(0), constants(0){}
	DelayedType(unsigned index, ExternConstantInfo *constants): index(index), constants(constants){}

	unsigned index;
	ExternConstantInfo *constants;
};

void ImportModuleTypes(ExpressionContext &ctx, SynBase *source, ModuleContext &moduleCtx)
{
	ByteCode *bCode = moduleCtx.data->bytecode;
	char *symbols = FindSymbols(bCode);

	// Import types
	ExternTypeInfo *typeList = FindFirstType(bCode);
	ExternMemberInfo *memberList = (ExternMemberInfo*)(typeList + bCode->typeCount);
	ExternConstantInfo *constantList = FindFirstConstant(bCode);
	ExternTypedefInfo *aliasList = FindFirstTypedef(bCode);

	unsigned prevSize = moduleCtx.types.size();

	moduleCtx.types.resize(bCode->typeCount);

	for(unsigned i = prevSize; i < moduleCtx.types.size(); i++)
		moduleCtx.types[i] = NULL;

	SmallArray<DelayedType, 32> delayedTypes;

	ExternConstantInfo *currentConstant = constantList;

	for(unsigned i = 0; i < bCode->typeCount; i++)
	{
		ExternTypeInfo &type = typeList[i];

		// Skip existing types
		if(TypeBase **prev = ctx.typeMap.find(type.nameHash))
		{
			moduleCtx.types[i] = *prev;

			currentConstant += type.constantCount;
			continue;
		}

		InplaceStr typeName = InplaceStr(symbols + type.offsetToName);

		switch(type.subCat)
		{
		case ExternTypeInfo::CAT_NONE:
			if(strcmp(symbols + type.offsetToName, "generic") == 0)
			{
				// TODO: explicit category
				moduleCtx.types[i] = ctx.typeGeneric;

				moduleCtx.types[i]->importModule = moduleCtx.data;

				assert(moduleCtx.types[i]->name == typeName);
			}
			else if(*(symbols + type.offsetToName) == '@')
			{
				// TODO: explicit category
				moduleCtx.types[i] = ctx.GetGenericAliasType(InplaceStr(symbols + type.offsetToName + 1));

				moduleCtx.types[i]->importModule = moduleCtx.data;

				assert(moduleCtx.types[i]->name == typeName);
			}
			else
			{
				Stop(ctx, source->pos, "ERROR: new type in module %.*s named %s unsupported", FMT_ISTR(moduleCtx.data->name), symbols + type.offsetToName);
			}
			break;
		case ExternTypeInfo::CAT_ARRAY:
			if(TypeBase *subType = moduleCtx.types[type.subType])
			{
				if(type.arrSize == ~0u)
					moduleCtx.types[i] = ctx.GetUnsizedArrayType(subType);
				else
					moduleCtx.types[i] = ctx.GetArrayType(subType, type.arrSize);

				moduleCtx.types[i]->importModule = moduleCtx.data;

				assert(moduleCtx.types[i]->name == typeName);
			}
			else
			{
				Stop(ctx, source->pos, "ERROR: can't find sub type for '%s' in module %.*s", symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));
			}
			break;
		case ExternTypeInfo::CAT_POINTER:
			if(TypeBase *subType = moduleCtx.types[type.subType])
			{
				moduleCtx.types[i] = ctx.GetReferenceType(subType);

				moduleCtx.types[i]->importModule = moduleCtx.data;

				assert(moduleCtx.types[i]->name == typeName);
			}
			else
			{
				Stop(ctx, source->pos, "ERROR: can't find sub type for '%s' in module %.*s", symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));
			}
			break;
		case ExternTypeInfo::CAT_FUNCTION:
			if(TypeBase *returnType = moduleCtx.types[memberList[type.memberOffset].type])
			{
				IntrusiveList<TypeHandle> arguments;

				for(unsigned n = 0; n < type.memberCount; n++)
				{
					TypeBase *argType = moduleCtx.types[memberList[type.memberOffset + n + 1].type];

					if(!argType)
						Stop(ctx, source->pos, "ERROR: can't find argument %d type for '%s' in module %.*s", n + 1, symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));

					arguments.push_back(new (ctx.get<TypeHandle>()) TypeHandle(argType));
				}

				moduleCtx.types[i] = ctx.GetFunctionType(source, returnType, arguments);

				moduleCtx.types[i]->importModule = moduleCtx.data;

				assert(moduleCtx.types[i]->name == typeName);
			}
			else
			{
				Stop(ctx, source->pos, "ERROR: can't find return type for '%s' in module %.*s", symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));
			}
			break;
		case ExternTypeInfo::CAT_CLASS:
			{
				TypeBase *importedType = NULL;

				ModuleData *importModule = moduleCtx.data;

				NamespaceData *parentNamespace = NULL;

				for(unsigned k = 0; k < ctx.namespaces.size(); k++)
				{
					if(ctx.namespaces[k]->fullNameHash == type.namespaceHash)
					{
						parentNamespace = ctx.namespaces[k];
						break;
					}
				}

				if(parentNamespace)
					ctx.PushScope(parentNamespace);

				// Find all generics for this type
				bool isGeneric = false;

				IntrusiveList<TypeHandle> generics;
				IntrusiveList<MatchData> actualGenerics;

				IntrusiveList<MatchData> aliases;

				for(unsigned k = 0; k < bCode->typedefCount; k++)
				{
					ExternTypedefInfo &alias = aliasList[k];

					if(alias.parentType == i)
					{
						InplaceStr aliasName = InplaceStr(symbols + alias.offsetToName);

						TypeBase *targetType = moduleCtx.types[alias.targetType];

						if(!targetType)
							Stop(ctx, source->pos, "ERROR: can't find alias '%s' target type in module %.*s", symbols + alias.offsetToName, FMT_ISTR(moduleCtx.data->name));

						isGeneric |= targetType->isGeneric;

						generics.push_back(new (ctx.get<TypeHandle>()) TypeHandle(targetType));

						if(actualGenerics.size() < type.genericTypeCount)
							actualGenerics.push_back(new (ctx.get<MatchData>()) MatchData(aliasName, targetType));
						else
							aliases.push_back(new (ctx.get<MatchData>()) MatchData(aliasName, targetType));
					}
				}

				TypeClass *baseType = NULL;

				if(type.baseType)
				{
					baseType = getType<TypeClass>(moduleCtx.types[type.baseType]);

					if(!baseType)
						Stop(ctx, source->pos, "ERROR: can't find type '%.*s' base type in module %.*s", FMT_ISTR(typeName), FMT_ISTR(moduleCtx.data->name));
				}

				if(type.definitionOffset != ~0u && type.definitionOffset & 0x80000000)
				{
					TypeBase *proto = moduleCtx.types[type.definitionOffset & ~0x80000000];

					if(!proto)
						Stop(ctx, source->pos, "ERROR: can't find proto type for '%s' in module %.*s", symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));

					TypeGenericClassProto *protoClass = getType<TypeGenericClassProto>(proto);

					if(!protoClass)
						Stop(ctx, source->pos, "ERROR: can't find correct proto type for '%s' in module %.*s", symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));

					if(isGeneric)
					{
						importedType = ctx.GetGenericClassType(source, protoClass, generics);

						// TODO: assert that alias list is empty and that correct number of generics was exported
					}
					else
					{
						TypeClass *classType = new (ctx.get<TypeClass>()) TypeClass(typeName, source, ctx.scope, protoClass, actualGenerics, (type.typeFlags & ExternTypeInfo::TYPE_IS_EXTENDABLE) != 0, baseType);

						classType->completed = true;

						importedType = classType;

						ctx.AddType(importedType);

						classType->aliases = aliases;

						assert(type.genericTypeCount == generics.size());

						if(!generics.empty())
							ctx.genericTypeMap.insert(typeName.hash(), classType);
					}
				}
				else if(type.definitionOffsetStart != ~0u)
				{
					if(type.definitionModule != 0)
						importModule = ctx.dependencies[moduleCtx.data->startingDependencyIndex + type.definitionModule - 1];

					assert(type.definitionOffsetStart < importModule->lexStreamSize);
					Lexeme *start = type.definitionOffsetStart + importModule->lexStream;

					ParseContext *parser = new (ctx.get<ParseContext>()) ParseContext(ctx.allocator, ArrayView<InplaceStr>());

					parser->currentLexeme = start;

					SynClassDefinition *definition = getType<SynClassDefinition>(ParseClassDefinition(*parser));

					if(!definition)
						Stop(ctx, source->pos, "ERROR: failed to import generic class body");

					definition->imported = true;

					importedType = new (ctx.get<TypeGenericClassProto>()) TypeGenericClassProto(source, ctx.scope, typeName, definition);

					ctx.AddType(importedType);

					// TODO: check that type doesn't have generics or aliases
				}
				else if(type.type != ExternTypeInfo::TYPE_COMPLEX)
				{
					TypeEnum *enumType = new (ctx.get<TypeEnum>()) TypeEnum(typeName, source, ctx.scope);

					importedType = enumType;

					ctx.AddType(importedType);

					assert(generics.empty() && aliases.empty());
				}
				else
				{
					IntrusiveList<MatchData> actualGenerics;

					TypeClass *classType = new (ctx.get<TypeClass>()) TypeClass(typeName, source, ctx.scope, NULL, actualGenerics, (type.typeFlags & ExternTypeInfo::TYPE_IS_EXTENDABLE) != 0, baseType);
					classType->completed = true;

					importedType = classType;

					ctx.AddType(importedType);

					classType->aliases = aliases;
				}

				moduleCtx.types[i] = importedType;

				moduleCtx.types[i]->importModule = importModule;

				assert(moduleCtx.types[i]->name == typeName);

				importedType->alignment = type.defaultAlign;
				importedType->size = type.size;

				importedType->hasPointers = type.pointerCount != 0;

				if(getType<TypeStruct>(importedType))
				{
					delayedTypes.push_back(DelayedType(i, currentConstant));

					currentConstant += type.constantCount;
				}

				if(TypeClass *classType = getType<TypeClass>(importedType))
					classType->hasFinalizer = type.typeFlags & ExternTypeInfo::TYPE_HAS_FINALIZER;

				if(parentNamespace)
					ctx.PopScope(SCOPE_NAMESPACE);
			}
			break;
		default:
			Stop(ctx, source->pos, "ERROR: new type in module %.*s named %s unsupported", FMT_ISTR(moduleCtx.data->name), symbols + type.offsetToName);
		}
	}

	for(unsigned i = 0; i < delayedTypes.size(); i++)
	{
		DelayedType &delayedType = delayedTypes[i];
		ExternTypeInfo &type = typeList[delayedType.index];

		switch(type.subCat)
		{
		case ExternTypeInfo::CAT_CLASS:
			{
				InplaceStr className = InplaceStr(symbols + type.offsetToName);

				TypeBase *importedType = moduleCtx.types[delayedType.index];

				const char *memberNames = className.end + 1;

				if(TypeStruct *structType = getType<TypeStruct>(importedType))
				{
					ctx.PushScope(importedType);

					if(TypeStruct *classType = getType<TypeStruct>(structType))
						classType->typeScope = ctx.scope;

					for(unsigned n = 0; n < type.memberCount; n++)
					{
						InplaceStr memberName = InplaceStr(memberNames);
						memberNames = memberName.end + 1;

						TypeBase *memberType = moduleCtx.types[memberList[type.memberOffset + n].type];

						if(!memberType)
							Stop(ctx, source->pos, "ERROR: can't find member %d type for '%s' in module %.*s", n + 1, symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));

						VariableData *member = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, 0, memberType, memberName, memberList[type.memberOffset + n].offset, ctx.uniqueVariableId++);

						structType->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(member));
					}

					ExternConstantInfo *constantInfo = delayedType.constants;

					for(unsigned int n = 0; n < type.constantCount; n++)
					{
						InplaceStr memberName = InplaceStr(memberNames);
						memberNames = memberName.end + 1;

						TypeBase *constantType = moduleCtx.types[constantInfo->type];

						if(!constantType)
							Stop(ctx, source->pos, "ERROR: can't find constant %d type for '%s' in module %.*s", n + 1, symbols + type.offsetToName, FMT_ISTR(moduleCtx.data->name));

						ExprBase *value = NULL;

						if(constantType == ctx.typeBool)
						{
							value = new (ctx.get<ExprBoolLiteral>()) ExprBoolLiteral(source, constantType, constantInfo->value != 0);
						}
						else if(ctx.IsIntegerType(constantType) || isType<TypeEnum>(constantType))
						{
							value = new (ctx.get<ExprIntegerLiteral>()) ExprIntegerLiteral(source, constantType, constantInfo->value);
						}
						else if(ctx.IsFloatingPointType(constantType))
						{
							double data = 0.0;
							memcpy(&data, &constantInfo->value, sizeof(double));
							value = new (ctx.get<ExprRationalLiteral>()) ExprRationalLiteral(source, constantType, data);
						}
							
						if(!value)
							Stop(ctx, source->pos, "ERROR: can't import constant %d of type '%.*s'", n + 1, FMT_ISTR(constantType->name));

						structType->constants.push_back(new (ctx.get<ConstantData>()) ConstantData(memberName, value));

						constantInfo++;
					}

					ctx.PopScope(SCOPE_TYPE);
				}
			}
			break;
		default:
			break;
		}
	}
}

void ImportModuleVariables(ExpressionContext &ctx, SynBase *source, ModuleContext &moduleCtx)
{
	ByteCode *bCode = moduleCtx.data->bytecode;
	char *symbols = FindSymbols(bCode);

	// Import variables
	ExternVarInfo *variableList = FindFirstVar(bCode);

	for(unsigned i = 0; i < bCode->variableExportCount; i++)
	{
		ExternVarInfo &variable = variableList[i];

		InplaceStr name = InplaceStr(symbols + variable.offsetToName);

		// Exclude temporary variables from import
		if(name.length() >= 5 && InplaceStr(name.begin, name.begin + 5) == InplaceStr("$temp"))
			continue;

		TypeBase *type = moduleCtx.types[variable.type];

		if(!type)
			Stop(ctx, source->pos, "ERROR: can't find variable '%s' type in module %.*s", symbols + variable.offsetToName, FMT_ISTR(moduleCtx.data->name));

		VariableData *data = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, 0, type, name, variable.offset, ctx.uniqueVariableId++);

		data->importModule = moduleCtx.data;

		ctx.AddVariable(data);

		if(name.length() > 5 && memcmp(name.begin, "$vtbl", 5) == 0)
			ctx.vtables.push_back(data);
	}
}

void ImportModuleTypedefs(ExpressionContext &ctx, SynBase *source, ModuleContext &moduleCtx)
{
	ByteCode *bCode = moduleCtx.data->bytecode;
	char *symbols = FindSymbols(bCode);

	// Import type aliases
	ExternTypedefInfo *aliasList = FindFirstTypedef(bCode);

	for(unsigned i = 0; i < bCode->typedefCount; i++)
	{
		ExternTypedefInfo &alias = aliasList[i];

		InplaceStr aliasName = InplaceStr(symbols + alias.offsetToName);

		TypeBase *targetType = moduleCtx.types[alias.targetType];

		if(!targetType)
			Stop(ctx, source->pos, "ERROR: can't find alias '%s' target type in module %.*s", symbols + alias.offsetToName, FMT_ISTR(moduleCtx.data->name));

		if(TypeBase **prev = ctx.typeMap.find(aliasName.hash()))
		{
			TypeBase *type = *prev;

			if(type->name == aliasName)
				Stop(ctx, source->pos, "ERROR: type '%.*s' alias '%s' is equal to previously imported class", FMT_ISTR(targetType->name), symbols + alias.offsetToName);

			if(type != targetType)
				Stop(ctx, source->pos, "ERROR: type '%.*s' alias '%s' is equal to previously imported alias", FMT_ISTR(targetType->name), symbols + alias.offsetToName);
		}
		else if(alias.parentType != ~0u)
		{
			// Type alises were imported during type import
		}
		else
		{
			AliasData *alias = new (ctx.get<AliasData>()) AliasData(source, ctx.scope, targetType, aliasName, ctx.uniqueAliasId++);

			alias->importModule = moduleCtx.data;

			ctx.AddAlias(alias);
		}
	}
}

void ImportModuleFunctions(ExpressionContext &ctx, SynBase *source, ModuleContext &moduleCtx)
{
	ByteCode *bCode = moduleCtx.data->bytecode;
	char *symbols = FindSymbols(bCode);

	ExternVarInfo *explicitTypeInfo = FindFirstVar(bCode) + bCode->variableCount;

	// Import functions
	ExternFuncInfo *functionList = FindFirstFunc(bCode);
	ExternLocalInfo *localList = FindFirstLocal(bCode);

	unsigned currCount = ctx.functions.size();

	for(unsigned i = 0; i < bCode->functionCount - bCode->moduleFunctionCount; i++)
	{
		ExternFuncInfo &function = functionList[i];

		InplaceStr functionName = InplaceStr(symbols + function.offsetToName);

		TypeBase *functionType = moduleCtx.types[function.funcType];

		if(!functionType)
			Stop(ctx, source->pos, "ERROR: can't find function '%s' type in module %.*s", symbols + function.offsetToName, FMT_ISTR(moduleCtx.data->name));

		// Import function explicit type list
		IntrusiveList<MatchData> generics;

		bool hasGenericExplicitType = false;

		for(unsigned k = 0; k < function.explicitTypeCount; k++)
		{
			InplaceStr name = InplaceStr(symbols + explicitTypeInfo[k].offsetToName);

			TypeBase *type = explicitTypeInfo[k].type == ~0u ? ctx.typeGeneric : moduleCtx.types[explicitTypeInfo[k].type];

			if(!type)
				Stop(ctx, source->pos, "ERROR: can't find function '%s' explicit type '%d' in module %.*s", symbols + function.offsetToName, k, FMT_ISTR(moduleCtx.data->name));

			if(type->isGeneric)
				hasGenericExplicitType = true;

			generics.push_back(new (ctx.get<MatchData>()) MatchData(name, type));
		}

		explicitTypeInfo += function.explicitTypeCount;

		FunctionData *prev = NULL;
		FunctionData *prototype = NULL;

		for(HashMap<FunctionData*>::Node *curr = ctx.functionMap.first(function.nameHash); curr; curr = ctx.functionMap.next(curr))
		{
			if(curr->value->isPrototype)
			{
				prototype = curr->value;
				continue;
			}

			if(curr->value->type == functionType)
			{
				bool explicitTypeMatch = true;

				for(unsigned k = 0; k < function.explicitTypeCount; k++)
				{
					TypeBase *prevType = curr->value->generics[k]->type;
					TypeBase *type = generics[k]->type;

					if(&prevType != &type)
						explicitTypeMatch = false;
				}

				if(explicitTypeMatch)
				{
					prev = curr->value;
					break;
				}
			}
		}

		if(prev)
		{
			if(*prev->name.name.begin == '$' || prev->isGenericInstance)
				ctx.functions.push_back(prev);
			else
				Stop(ctx, source->pos, "ERROR: function %.*s (type %.*s) is already defined. While importing %.*s", FMT_ISTR(prev->name.name), FMT_ISTR(prev->type->name), FMT_ISTR(moduleCtx.data->name));

			continue;
		}

		NamespaceData *parentNamespace = NULL;

		for(unsigned k = 0; k < ctx.namespaces.size(); k++)
		{
			if(ctx.namespaces[k]->fullNameHash == function.namespaceHash)
			{
				parentNamespace = ctx.namespaces[k];
				break;
			}
		}

		if(parentNamespace)
			ctx.PushScope(parentNamespace);

		TypeBase *parentType = NULL;

		if(function.parentType != ~0u)
		{
			parentType = moduleCtx.types[function.parentType];

			if(!parentType)
				Stop(ctx, source->pos, "ERROR: can't find function '%s' parent type in module %.*s", symbols + function.offsetToName, FMT_ISTR(moduleCtx.data->name));
		}

		TypeBase *contextType = NULL;

		if(function.contextType != ~0u)
		{
			contextType = moduleCtx.types[function.contextType];

			if(!contextType)
				Stop(ctx, source->pos, "ERROR: can't find function '%s' context type in module %.*s", symbols + function.offsetToName, FMT_ISTR(moduleCtx.data->name));
		}

		if(!contextType)
			contextType = ctx.GetReferenceType(parentType ? parentType : ctx.typeVoid);

		bool coroutine = function.funcCat == ExternFuncInfo::COROUTINE;
		bool accessor = *(functionName.end - 1) == '$';
		bool isOperator = false; // TODO: detect operators

		if(parentType)
			ctx.PushScope(parentType);

		FunctionData *data = new (ctx.get<FunctionData>()) FunctionData(ctx.allocator, source, ctx.scope, coroutine, accessor, isOperator, getType<TypeFunction>(functionType), contextType, SynIdentifier(functionName), generics, ctx.uniqueFunctionId++);

		data->importModule = moduleCtx.data;

		data->isPrototype = (function.codeSize & 0x80000000) != 0;

		if(prototype)
			prototype->implementation = data;

		// TODO: find function proto
		data->isGenericInstance = !!function.isGenericInstance;

		if(data->name.name == InplaceStr("__newS") || data->name.name == InplaceStr("__newA") || data->name.name == InplaceStr("__closeUpvalue"))
			data->isInternal = true;

		if(function.funcCat == ExternFuncInfo::LOCAL)
			data->isHidden = true;

		ctx.AddFunction(data);

		ctx.PushScope(data);

		data->functionScope = ctx.scope;

		for(unsigned n = 0; n < function.paramCount; n++)
		{
			ExternLocalInfo &argument = localList[function.offsetToFirstLocal + n];

			bool isExplicit = (argument.paramFlags & ExternLocalInfo::IS_EXPLICIT) != 0;

			TypeBase *argType = argument.type == ~0u ? ctx.typeGeneric : moduleCtx.types[argument.type];

			if(!argType)
				Stop(ctx, source->pos, "ERROR: can't find argument %d type for '%s' in module %.*s", n + 1, symbols + function.offsetToName, FMT_ISTR(moduleCtx.data->name));

			InplaceStr argName = InplaceStr(symbols + argument.offsetToName);

			data->arguments.push_back(ArgumentData(source, isExplicit, argName, argType, NULL));

			unsigned offset = AllocateArgumentInScope(ctx, source, 0, argType);
			VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, 0, argType, argName, offset, ctx.uniqueVariableId++);

			ctx.AddVariable(variable);
		}

		assert(contextType);

		if(parentType)
		{
			TypeBase *type = ctx.GetReferenceType(parentType);

			unsigned offset = AllocateArgumentInScope(ctx, source, 0, type);
			VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, 0, type, InplaceStr("this"), offset, ctx.uniqueVariableId++);

			ctx.AddVariable(variable);
		}
		else if(contextType)
		{
			unsigned offset = AllocateArgumentInScope(ctx, source, 0, contextType);
			VariableData *variable = new (ctx.get<VariableData>()) VariableData(ctx.allocator, source, ctx.scope, 0, contextType, InplaceStr("$context"), offset, ctx.uniqueVariableId++);

			ctx.AddVariable(variable);
		}

		data->argumentsSize = data->functionScope->dataSize;

		// TODO: explicit flag
		if(function.funcType == 0 || functionType->isGeneric || hasGenericExplicitType || (parentType && parentType->isGeneric))
		{
			if(function.genericModuleIndex != 0)
				data->importModule = ctx.dependencies[moduleCtx.data->startingDependencyIndex + function.genericModuleIndex - 1];

			assert(function.genericOffsetStart < data->importModule->lexStreamSize);
			Lexeme *start = function.genericOffsetStart + data->importModule->lexStream;

			ParseContext *parser = new (ctx.get<ParseContext>()) ParseContext(ctx.allocator, ArrayView<InplaceStr>());

			parser->currentLexeme = start;

			SynFunctionDefinition *definition = ParseFunctionDefinition(*parser);

			if(!definition)
				Stop(ctx, source->pos, "ERROR: failed to import generic functions body");

			data->definition = definition;

			TypeBase *returnType = ctx.typeAuto;

			if(function.genericReturnType != ~0u)
				returnType = moduleCtx.types[function.genericReturnType];

			if(!returnType)
				Stop(ctx, source->pos, "ERROR: can't find generic function '%s' return type in module %.*s", symbols + function.offsetToName, FMT_ISTR(moduleCtx.data->name));

			IntrusiveList<TypeHandle> argTypes;

			for(unsigned n = 0; n < function.paramCount; n++)
			{
				ExternLocalInfo &argument = localList[function.offsetToFirstLocal + n];

				argTypes.push_back(new (ctx.get<TypeHandle>()) TypeHandle(argument.type == ~0u ? ctx.typeGeneric : moduleCtx.types[argument.type]));
			}

			data->type = ctx.GetFunctionType(source, returnType, argTypes);
		}

		if(function.funcCat == ExternFuncInfo::COROUTINE)
		{
			InplaceStr contextVariableName = GetFunctionContextVariableName(ctx, data, i + bCode->moduleFunctionCount);

			if(VariableData **variable = ctx.variableMap.find(contextVariableName.hash()))
				data->contextVariable = *variable;
		}

		assert(data->type);

		ctx.PopScope(SCOPE_FUNCTION);

		if(data->isPrototype)
			ctx.HideFunction(data);
		else if(data->isHidden)
			ctx.HideFunction(data);

		if(parentType)
			ctx.PopScope(SCOPE_TYPE);

		if(parentNamespace)
			ctx.PopScope(SCOPE_NAMESPACE);
	}

	for(unsigned i = 0; i < bCode->functionCount - bCode->moduleFunctionCount; i++)
	{
		ExternFuncInfo &function = functionList[i];

		FunctionData *data = ctx.functions[currCount + i];

		for(unsigned n = 0; n < function.paramCount; n++)
		{
			ExternLocalInfo &argument = localList[function.offsetToFirstLocal + n];

			if(argument.defaultFuncId != 0xffff)
			{
				FunctionData *target = ctx.functions[currCount + argument.defaultFuncId - bCode->moduleFunctionCount];

				ExprBase *access = new (ctx.get<ExprFunctionAccess>()) ExprFunctionAccess(source, target->type, target, new (ctx.get<ExprNullptrLiteral>()) ExprNullptrLiteral(source, target->contextType));

				data->arguments[n].value = new (ctx.get<ExprFunctionCall>()) ExprFunctionCall(source, target->type->returnType, access, IntrusiveList<ExprBase>());
			}
		}
	}
}

void ImportModule(ExpressionContext &ctx, SynBase *source, ByteCode* bytecode, Lexeme *lexStream, unsigned lexStreamSize, InplaceStr name)
{
#ifdef IMPORT_VERBOSE_DEBUG_OUTPUT
	printf("  importing module %.*s as dependency #%d\n", FMT_ISTR(name), ctx.imports.size() + 1, ctx.dependencies.size() + 1);
#endif

	assert(bytecode);

	ModuleData *moduleData = new (ctx.get<ModuleData>()) ModuleData(source, name);

	ctx.imports.push_back(moduleData);
	moduleData->importIndex = ctx.imports.size();

	ctx.dependencies.push_back(moduleData);
	moduleData->dependencyIndex = ctx.dependencies.size();

	moduleData->bytecode = bytecode;

	if(!lexStream)
	{
		moduleData->lexer = new (ctx.get<Lexer>()) Lexer(ctx.allocator);

		moduleData->lexer->Lexify(FindSource(bytecode));
		lexStream = moduleData->lexer->GetStreamStart();
		lexStreamSize = moduleData->lexer->GetStreamSize();
	}

	moduleData->lexStream = lexStream;
	moduleData->lexStreamSize = lexStreamSize;

	moduleData->startingFunctionIndex = ctx.functions.size();

	moduleData->startingDependencyIndex = ctx.dependencies.size();

	ModuleContext moduleCtx(ctx.allocator);

	moduleCtx.data = moduleData;

	ImportModuleDependencies(ctx, source, moduleCtx, moduleData->bytecode);

	ImportModuleNamespaces(ctx, source, moduleCtx);

	ImportModuleTypes(ctx, source, moduleCtx);

	ImportModuleVariables(ctx, source, moduleCtx);

	ImportModuleTypedefs(ctx, source, moduleCtx);

	ImportModuleFunctions(ctx, source, moduleCtx);

	moduleData->functionCount = ctx.functions.size() - moduleData->startingFunctionIndex;
}

void AnalyzeModuleImport(ExpressionContext &ctx, SynModuleImport *syntax)
{
	const char *importPath = BinaryCache::GetImportPath();

	InplaceStr path = GetImportPath(ctx.allocator, importPath, syntax->path);
	InplaceStr pathNoImport = importPath ? InplaceStr(path.begin + strlen(importPath)) : path;

	const char *bytecode = BinaryCache::GetBytecode(path.begin);
	unsigned lexStreamSize = 0;
	Lexeme *lexStream = BinaryCache::GetLexems(path.begin, lexStreamSize);

	if(!bytecode)
	{
		bytecode = BinaryCache::GetBytecode(pathNoImport.begin);
		lexStream = BinaryCache::GetLexems(pathNoImport.begin, lexStreamSize);
	}

	if(!bytecode)
		Stop(ctx, syntax->pos, "ERROR: module import is not implemented");

	ImportModule(ctx, syntax, (ByteCode*)bytecode, lexStream, lexStreamSize, pathNoImport);
}

void CreateDefaultArgumentFunctionWrappers(ExpressionContext &ctx)
{
	for(unsigned i = 0; i < ctx.functions.size(); i++)
	{
		FunctionData *function = ctx.functions[i];

		if(function->importModule)
			continue;

		// Handle only global visible functions
		if(function->scope != ctx.globalScope && !function->scope->ownerType)
			continue;

		if(function->isHidden)
			continue;

		// Go through all function arguments
		for(unsigned k = 0; k < function->arguments.size(); k++)
		{
			ArgumentData &argument = function->arguments[k];

			if(argument.value)
			{
				assert(argument.valueFunction == NULL);

				ExprBase *value = argument.value;

				if(isType<TypeFunctionSet>(value->type))
					value = CreateCast(ctx, argument.source, argument.value, argument.type, true);

				InplaceStr functionName = GetDefaultArgumentWrapperFunctionName(ctx, function, argument.name);

				ExprFunctionAccess *access = CreateValueFunctionWrapper(ctx, argument.source, value, functionName);

				argument.valueFunction = access->function;
			}
		}
	}
}

ExprBase* CreateVirtualTableUpdate(ExpressionContext &ctx, SynBase *source, VariableData *vtable)
{
	IntrusiveList<ExprBase> expressions;

	// Find function name
	InplaceStr name = InplaceStr(vtable->name.begin + 15); // 15 to skip $vtbl0123456789 from name

	// Find function type from name
	unsigned typeNameHash = strtoul(vtable->name.begin + 5, NULL, 10);

	TypeBase *functionType = NULL;

	for(unsigned i = 0; i < ctx.types.size(); i++)
	{
		if(ctx.types[i]->nameHash == typeNameHash)
		{
			functionType = getType<TypeFunction>(ctx.types[i]);
			break;
		}
	}

	if(!functionType)
		Stop(ctx, source->pos, "ERROR: Can't find function type for virtual function table '%.*s'", FMT_ISTR(vtable->name));

	if(vtable->importModule == NULL)
	{
		ExprBase *count = CreateFunctionCall0(ctx, source, InplaceStr("__typeCount"), false, true);

		ExprBase *alloc = CreateArrayAllocation(ctx, source, ctx.typeFunctionID, count);

		ExprBase *assignment = CreateAssignment(ctx, source, new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, vtable->type, vtable), alloc);

		expressions.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(source, ctx.typeVoid, vtable, assignment));
	}

	// Find all functions with called name that are member functions and have target type
	SmallArray<FunctionData*, 32> functions(ctx.allocator);

	for(unsigned i = 0; i < ctx.functions.size(); i++)
	{
		FunctionData *function = ctx.functions[i];

		TypeBase *parentType = function->scope->ownerType;

		if(!parentType)
			continue;

		// If both type and table are imported, then it should have been filled up inside the module for that type
		if(parentType->importModule && vtable->importModule && parentType->importModule == vtable->importModule)
			continue;

		const char *pos = strstr(function->name.name.begin, "::");

		if(!pos)
			continue;

		if(InplaceStr(pos + 2) == name && function->type == functionType)
			functions.push_back(function);
	}

	for(unsigned i = 0; i < ctx.types.size(); i++)
	{
		for(unsigned k = 0; k < functions.size(); k++)
		{
			TypeBase *type = ctx.types[i];
			FunctionData *function = functions[k];

			while(type)
			{
				if(function->scope->ownerType == type)
				{
					ExprBase *vtableAccess = new (ctx.get<ExprVariableAccess>()) ExprVariableAccess(source, vtable->type, vtable);

					ExprBase *typeId = new (ctx.get<ExprTypeLiteral>()) ExprTypeLiteral(source, ctx.typeTypeID, ctx.types[i]);

					SmallArray<ArgumentData, 32> arguments(ctx.allocator);
					arguments.push_back(ArgumentData(source, false, InplaceStr(), ctx.typeInt, new (ctx.get<ExprTypeCast>()) ExprTypeCast(source, ctx.typeInt, typeId, EXPR_CAST_REINTERPRET)));

					ExprBase *arraySlot = CreateArrayIndex(ctx, source, vtableAccess, arguments);

					ExprBase *assignment = CreateAssignment(ctx, source, arraySlot, new (ctx.get<ExprFunctionIndexLiteral>()) ExprFunctionIndexLiteral(source, ctx.typeFunctionID, function));

					expressions.push_back(assignment);
					break;
				}

				// Stepping through the class inheritance tree will ensure that the base class function will be used if the derived class function is not available
				if(TypeClass *classType = getType<TypeClass>(type))
					type = classType->baseClass;
				else
					type = NULL;
			}
		}
	}

	return new (ctx.get<ExprBlock>()) ExprBlock(source, ctx.typeVoid, expressions, NULL);
}

ExprModule* AnalyzeModule(ExpressionContext &ctx, SynModule *syntax)
{
	// Import base module
	if(const char *bytecode = BinaryCache::GetBytecode("$base$.nc"))
	{
		unsigned lexStreamSize = 0;
		Lexeme *lexStream = BinaryCache::GetLexems("$base$.nc", lexStreamSize);

		if(bytecode)
			ImportModule(ctx, syntax, (ByteCode*)bytecode, lexStream, lexStreamSize, InplaceStr("$base$.nc"));
		else
			Stop(ctx, syntax->pos, "ERROR: base module couldn't be imported");

		ctx.baseModuleFunctionCount = ctx.functions.size();
	}

	for(SynModuleImport *import = syntax->imports.head; import; import = getType<SynModuleImport>(import->next))
		AnalyzeModuleImport(ctx, import);

	IntrusiveList<ExprBase> expressions;

	for(SynBase *expr = syntax->expressions.head; expr; expr = expr->next)
		expressions.push_back(AnalyzeStatement(ctx, expr));

	for(unsigned i = 0; i < ctx.types.size(); i++)
	{
		if(TypeStruct *typeStruct = getType<TypeStruct>(ctx.types[i]))
		{
			if(TypeClass *typeClass = getType<TypeClass>(typeStruct))
			{
				if(!typeClass->completed)
					Stop(ctx, syntax->pos, "ERROR: type '%.*s' is not fully defined", FMT_ISTR(typeClass->name));
			}

			assert(typeStruct->typeScope);
		}
	}

	CreateDefaultArgumentFunctionWrappers(ctx);

	ExprModule *module = new (ctx.get<ExprModule>()) ExprModule(ctx.allocator, syntax, ctx.typeVoid, ctx.globalScope, expressions);

	for(unsigned i = 0; i < ctx.definitions.size(); i++)
		module->definitions.push_back(ctx.definitions[i]);

	for(unsigned i = 0; i < ctx.setup.size(); i++)
		module->setup.push_back(ctx.setup[i]);

	for(unsigned i = 0; i < ctx.vtables.size(); i++)
		module->setup.push_back(CreateVirtualTableUpdate(ctx, syntax, ctx.vtables[i]));

	for(unsigned i = 0; i < ctx.upvalues.size(); i++)
		module->setup.push_back(new (ctx.get<ExprVariableDefinition>()) ExprVariableDefinition(syntax, ctx.typeVoid, ctx.upvalues[i], NULL));

	return module;
}

ExprModule* Analyze(ExpressionContext &ctx, SynModule *syntax, const char *code)
{
	assert(!ctx.globalScope);

	ctx.code = code;

	ctx.PushScope(SCOPE_EXPLICIT);
	ctx.globalScope = ctx.scope;

	ctx.AddType(ctx.typeVoid = new (ctx.get<TypeVoid>()) TypeVoid(InplaceStr("void")));

	ctx.AddType(ctx.typeBool = new (ctx.get<TypeBool>()) TypeBool(InplaceStr("bool")));

	ctx.AddType(ctx.typeChar = new (ctx.get<TypeChar>()) TypeChar(InplaceStr("char")));
	ctx.AddType(ctx.typeShort = new (ctx.get<TypeShort>()) TypeShort(InplaceStr("short")));
	ctx.AddType(ctx.typeInt = new (ctx.get<TypeInt>()) TypeInt(InplaceStr("int")));
	ctx.AddType(ctx.typeLong = new (ctx.get<TypeLong>()) TypeLong(InplaceStr("long")));

	ctx.AddType(ctx.typeFloat = new (ctx.get<TypeFloat>()) TypeFloat(InplaceStr("float")));
	ctx.AddType(ctx.typeDouble = new (ctx.get<TypeDouble>()) TypeDouble(InplaceStr("double")));

	ctx.AddType(ctx.typeTypeID = new (ctx.get<TypeTypeID>()) TypeTypeID(InplaceStr("typeid")));
	ctx.AddType(ctx.typeFunctionID = new (ctx.get<TypeFunctionID>()) TypeFunctionID(InplaceStr("__function")));
	ctx.AddType(ctx.typeNullPtr = new (ctx.get<TypeNullptr>()) TypeNullptr(InplaceStr("__nullptr")));

	ctx.AddType(ctx.typeGeneric = new (ctx.get<TypeGeneric>()) TypeGeneric(InplaceStr("generic")));

	ctx.AddType(ctx.typeAuto = new (ctx.get<TypeAuto>()) TypeAuto(InplaceStr("auto")));

	ctx.AddType(ctx.typeAutoRef = new (ctx.get<TypeAutoRef>()) TypeAutoRef(InplaceStr("auto ref")));
	ctx.PushScope(ctx.typeAutoRef);
	ctx.typeAutoRef->typeScope = ctx.scope;
	ctx.typeAutoRef->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(AllocateClassMember(ctx, syntax, 0, ctx.typeTypeID, InplaceStr("type"), true, ctx.uniqueVariableId++)));
	ctx.typeAutoRef->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(AllocateClassMember(ctx, syntax, 0, ctx.GetReferenceType(ctx.typeVoid), InplaceStr("ptr"), true, ctx.uniqueVariableId++)));
	FinalizeAlignment(ctx.typeAutoRef);
	ctx.PopScope(SCOPE_TYPE);

	ctx.AddType(ctx.typeAutoArray = new (ctx.get<TypeAutoArray>()) TypeAutoArray(InplaceStr("auto[]")));
	ctx.PushScope(ctx.typeAutoArray);
	ctx.typeAutoArray->typeScope = ctx.scope;
	ctx.typeAutoArray->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(AllocateClassMember(ctx, syntax, 0, ctx.typeTypeID, InplaceStr("type"), true, ctx.uniqueVariableId++)));
	ctx.typeAutoArray->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(AllocateClassMember(ctx, syntax, 0, ctx.GetReferenceType(ctx.typeVoid), InplaceStr("ptr"), true, ctx.uniqueVariableId++)));
	ctx.typeAutoArray->members.push_back(new (ctx.get<VariableHandle>()) VariableHandle(AllocateClassMember(ctx, syntax, 0, ctx.typeInt, InplaceStr("size"), true, ctx.uniqueVariableId++)));
	FinalizeAlignment(ctx.typeAutoArray);
	ctx.PopScope(SCOPE_TYPE);

	// Analyze module
	if(!setjmp(ctx.errorHandler))
	{
		ctx.errorHandlerActive = true;

		ExprModule *module = AnalyzeModule(ctx, syntax);

		ctx.errorHandlerActive = false;

		ctx.PopScope(SCOPE_EXPLICIT);

		assert(ctx.scope == NULL);

		ctx.code = NULL;

		return module;
	}

	assert(ctx.errorPos != NULL);

	ctx.code = NULL;

	return NULL;
}

void VisitExpressionTreeNodes(ExprBase *expression, void *context, void(*accept)(void *context, ExprBase *child))
{
	if(!expression)
		return;

	accept(context, expression);

	if(ExprPassthrough *node = getType<ExprPassthrough>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprArray *node = getType<ExprArray>(expression))
	{
		for(ExprBase *value = node->values.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);
	}
	else if(ExprPreModify *node = getType<ExprPreModify>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprPostModify *node = getType<ExprPostModify>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprTypeCast *node = getType<ExprTypeCast>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprUnaryOp *node = getType<ExprUnaryOp>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprBinaryOp *node = getType<ExprBinaryOp>(expression))
	{
		VisitExpressionTreeNodes(node->lhs, context, accept);
		VisitExpressionTreeNodes(node->rhs, context, accept);
	}
	else if(ExprDereference *node = getType<ExprDereference>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprUnboxing *node = getType<ExprUnboxing>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprConditional *node = getType<ExprConditional>(expression))
	{
		VisitExpressionTreeNodes(node->condition, context, accept);
		VisitExpressionTreeNodes(node->trueBlock, context, accept);
		VisitExpressionTreeNodes(node->falseBlock, context, accept);
	}
	else if(ExprAssignment *node = getType<ExprAssignment>(expression))
	{
		VisitExpressionTreeNodes(node->lhs, context, accept);
		VisitExpressionTreeNodes(node->rhs, context, accept);
	}
	else if(ExprMemberAccess *node = getType<ExprMemberAccess>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
	}
	else if(ExprArrayIndex *node = getType<ExprArrayIndex>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);
		VisitExpressionTreeNodes(node->index, context, accept);
	}
	else if(ExprReturn *node = getType<ExprReturn>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);

		VisitExpressionTreeNodes(node->coroutineStateUpdate, context, accept);

		VisitExpressionTreeNodes(node->closures, context, accept);
	}
	else if(ExprYield *node = getType<ExprYield>(expression))
	{
		VisitExpressionTreeNodes(node->value, context, accept);

		VisitExpressionTreeNodes(node->coroutineStateUpdate, context, accept);

		VisitExpressionTreeNodes(node->closures, context, accept);
	}
	else if(ExprVariableDefinition *node = getType<ExprVariableDefinition>(expression))
	{
		VisitExpressionTreeNodes(node->initializer, context, accept);
	}
	else if(ExprArraySetup *node = getType<ExprArraySetup>(expression))
	{
		VisitExpressionTreeNodes(node->lhs, context, accept);
		VisitExpressionTreeNodes(node->initializer, context, accept);
	}
	else if(ExprVariableDefinitions *node = getType<ExprVariableDefinitions>(expression))
	{
		for(ExprBase *value = node->definitions.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);
	}
	else if(ExprFunctionDefinition *node = getType<ExprFunctionDefinition>(expression))
	{
		VisitExpressionTreeNodes(node->contextArgument, context, accept);

		for(ExprBase *arg = node->arguments.head; arg; arg = arg->next)
			VisitExpressionTreeNodes(arg, context, accept);

		VisitExpressionTreeNodes(node->coroutineStateRead, context, accept);

		for(ExprBase *expr = node->expressions.head; expr; expr = expr->next)
			VisitExpressionTreeNodes(expr, context, accept);
	}
	else if(ExprGenericFunctionPrototype *node = getType<ExprGenericFunctionPrototype>(expression))
	{
		for(ExprBase *expr = node->contextVariables.head; expr; expr = expr->next)
			VisitExpressionTreeNodes(expr, context, accept);
	}
	else if(ExprFunctionAccess *node = getType<ExprFunctionAccess>(expression))
	{
		VisitExpressionTreeNodes(node->context, context, accept);
	}
	else if(ExprFunctionOverloadSet *node = getType<ExprFunctionOverloadSet>(expression))
	{
		VisitExpressionTreeNodes(node->context, context, accept);
	}
	else if(ExprFunctionCall *node = getType<ExprFunctionCall>(expression))
	{
		VisitExpressionTreeNodes(node->function, context, accept);

		for(ExprBase *arg = node->arguments.head; arg; arg = arg->next)
			VisitExpressionTreeNodes(arg, context, accept);
	}
	else if(ExprGenericClassPrototype *node = getType<ExprGenericClassPrototype>(expression))
	{
		for(ExprBase *value = node->genericProtoType->instances.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);
	}
	else if(ExprClassDefinition *node = getType<ExprClassDefinition>(expression))
	{
		for(ExprBase *value = node->functions.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);

		for(ConstantData *value = node->classType->constants.head; value; value = value->next)
			VisitExpressionTreeNodes(value->value, context, accept);
	}
	else if(ExprEnumDefinition *node = getType<ExprEnumDefinition>(expression))
	{
		for(ConstantData *value = node->enumType->constants.head; value; value = value->next)
			VisitExpressionTreeNodes(value->value, context, accept);

		VisitExpressionTreeNodes(node->toInt, context, accept);
		VisitExpressionTreeNodes(node->toEnum, context, accept);
	}
	else if(ExprIfElse *node = getType<ExprIfElse>(expression))
	{
		VisitExpressionTreeNodes(node->condition, context, accept);
		VisitExpressionTreeNodes(node->trueBlock, context, accept);
		VisitExpressionTreeNodes(node->falseBlock, context, accept);
	}
	else if(ExprFor *node = getType<ExprFor>(expression))
	{
		VisitExpressionTreeNodes(node->initializer, context, accept);
		VisitExpressionTreeNodes(node->condition, context, accept);
		VisitExpressionTreeNodes(node->increment, context, accept);
		VisitExpressionTreeNodes(node->body, context, accept);
	}
	else if(ExprWhile *node = getType<ExprWhile>(expression))
	{
		VisitExpressionTreeNodes(node->condition, context, accept);
		VisitExpressionTreeNodes(node->body, context, accept);
	}
	else if(ExprDoWhile *node = getType<ExprDoWhile>(expression))
	{
		VisitExpressionTreeNodes(node->body, context, accept);
		VisitExpressionTreeNodes(node->condition, context, accept);
	}
	else if(ExprSwitch *node = getType<ExprSwitch>(expression))
	{
		VisitExpressionTreeNodes(node->condition, context, accept);

		for(ExprBase *value = node->cases.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);

		for(ExprBase *value = node->blocks.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);

		VisitExpressionTreeNodes(node->defaultBlock, context, accept);
	}
	else if(ExprBreak *node = getType<ExprBreak>(expression))
	{
		VisitExpressionTreeNodes(node->closures, context, accept);
	}
	else if(ExprContinue *node = getType<ExprContinue>(expression))
	{
		VisitExpressionTreeNodes(node->closures, context, accept);
	}
	else if(ExprBlock *node = getType<ExprBlock>(expression))
	{
		for(ExprBase *value = node->expressions.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);

		VisitExpressionTreeNodes(node->closures, context, accept);
	}
	else if(ExprSequence *node = getType<ExprSequence>(expression))
	{
		for(ExprBase *value = node->expressions.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);
	}
	else if(ExprModule *node = getType<ExprModule>(expression))
	{
		for(unsigned i = 0; i < node->definitions.size(); i++)
			VisitExpressionTreeNodes(node->definitions[i], context, accept);

		for(ExprBase *value = node->setup.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);

		for(ExprBase *value = node->expressions.head; value; value = value->next)
			VisitExpressionTreeNodes(value, context, accept);
	}
}
