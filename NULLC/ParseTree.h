#pragma once

#include "Lexer.h"
#include "IntrusiveList.h"
#include "Array.h"

struct SynBase;
struct SynBinaryOpElement;

struct ParseContext
{
	ParseContext();

	LexemeType Peek();
	bool At(LexemeType type);
	bool Consume(LexemeType type);
	bool Consume(const char *str);
	InplaceStr Consume();
	void Skip();

	const char* Position();

	Lexeme *currentLexeme;

	FastVector<SynBinaryOpElement> binaryOpStack;

	const char *errorPos;
	InplaceStr errorMsg;
};

struct SynBase
{
	SynBase(unsigned typeID, const char *pos): typeID(typeID), pos(pos), next(0)
	{
	}

	virtual ~SynBase()
	{
	}

	unsigned typeID;

	const char *pos;
	SynBase *next;
};

struct SynIdentifier: SynBase
{
	SynIdentifier(const char* pos, InplaceStr name): SynBase(myTypeID, pos), name(name)
	{
	}

	InplaceStr name;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeAuto: SynBase
{
	SynTypeAuto(const char *pos): SynBase(myTypeID, pos)
	{
	}

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeGeneric: SynBase
{
	SynTypeGeneric(const char *pos): SynBase(myTypeID, pos)
	{
	}

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeSimple: SynBase
{
	SynTypeSimple(const char *pos, InplaceStr name): SynBase(myTypeID, pos), name(name)
	{
	}

	InplaceStr name;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeAlias: SynBase
{
	SynTypeAlias(const char *pos, InplaceStr name): SynBase(myTypeID, pos), name(name)
	{
	}

	InplaceStr name;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeArray: SynBase
{
	SynTypeArray(const char *pos, SynBase *type, SynBase *size): SynBase(myTypeID, pos), type(type), size(size)
	{
	}

	SynBase *type;
	SynBase *size;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeReference: SynBase
{
	SynTypeReference(const char *pos, SynBase *type): SynBase(myTypeID, pos), type(type)
	{
	}

	SynBase *type;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeFunction: SynBase
{
	SynTypeFunction(const char *pos, SynBase *returnType, IntrusiveList<SynBase> arguments): SynBase(myTypeID, pos), returnType(returnType), arguments(arguments)
	{
	}

	SynBase *returnType;
	IntrusiveList<SynBase> arguments;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeGenericInstance: SynBase
{
	SynTypeGenericInstance(const char *pos, InplaceStr name, IntrusiveList<SynBase> types): SynBase(myTypeID, pos), name(name), types(types)
	{
	}

	InplaceStr name;
	IntrusiveList<SynBase> types;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypeof: SynBase
{
	SynTypeof(const char *pos, SynBase* value): SynBase(myTypeID, pos), value(value)
	{
	}

	SynBase *value;

	static const unsigned myTypeID = __LINE__;
};

struct SynBool: SynBase
{
	SynBool(const char* pos, bool value): SynBase(myTypeID, pos), value(value)
	{
	}

	bool value;

	static const unsigned myTypeID = __LINE__;
};

struct SynNumber: SynBase
{
	SynNumber(const char* pos, InplaceStr value, InplaceStr suffix): SynBase(myTypeID, pos), value(value), suffix(suffix)
	{
	}

	InplaceStr value;
	InplaceStr suffix;

	static const unsigned myTypeID = __LINE__;
};

struct SynNullptr: SynBase
{
	SynNullptr(const char* pos): SynBase(myTypeID, pos)
	{
	}

	static const unsigned myTypeID = __LINE__;
};

struct SynCharacter: SynBase
{
	SynCharacter(const char* pos, InplaceStr value): SynBase(myTypeID, pos), value(value)
	{
	}

	InplaceStr value;

	static const unsigned myTypeID = __LINE__;
};

struct SynString: SynBase
{
	SynString(const char* pos, InplaceStr value): SynBase(myTypeID, pos), value(value)
	{
	}

	InplaceStr value;

	static const unsigned myTypeID = __LINE__;
};

struct SynArray: SynBase
{
	SynArray(const char* pos, IntrusiveList<SynBase> values): SynBase(myTypeID, pos), values(values)
	{
	}

	IntrusiveList<SynBase> values;

	static const unsigned myTypeID = __LINE__;
};

struct SynTypedef: SynBase
{
	SynTypedef(const char* pos, SynBase *type, InplaceStr alias): SynBase(myTypeID, pos), type(type), alias(alias)
	{
	}

	SynBase *type;
	InplaceStr alias;

	static const unsigned myTypeID = __LINE__;
};

struct SynMemberAccess: SynBase
{
	SynMemberAccess(const char* pos, SynBase* value, InplaceStr member): SynBase(myTypeID, pos), value(value), member(member)
	{
	}

	SynBase* value;
	InplaceStr member;

	static const unsigned myTypeID = __LINE__;
};

struct SynCallArgument: SynBase
{
	SynCallArgument(const char* pos, InplaceStr name, SynBase* value): SynBase(myTypeID, pos), name(name), value(value)
	{
	}

	InplaceStr name;
	SynBase* value;

	static const unsigned myTypeID = __LINE__;
};

struct SynArrayIndex: SynBase
{
	SynArrayIndex(const char* pos, SynBase* value, IntrusiveList<SynCallArgument> arguments): SynBase(myTypeID, pos), value(value), arguments(arguments)
	{
	}

	SynBase* value;
	IntrusiveList<SynCallArgument> arguments;

	static const unsigned myTypeID = __LINE__;
};

struct SynFunctionCall: SynBase
{
	SynFunctionCall(const char* pos, SynBase* value, IntrusiveList<SynCallArgument> arguments): SynBase(myTypeID, pos), value(value), arguments(arguments)
	{
	}

	SynBase* value;
	IntrusiveList<SynCallArgument> arguments;

	static const unsigned myTypeID = __LINE__;
};

struct SynPostModify: SynBase
{
	SynPostModify(const char* pos, SynBase* value, bool isIncrement): SynBase(myTypeID, pos), value(value), isIncrement(isIncrement)
	{
	}

	SynBase* value;
	bool isIncrement;

	static const unsigned myTypeID = __LINE__;
};

struct SynGetAddress: SynBase
{
	SynGetAddress(const char* pos, SynBase* value): SynBase(myTypeID, pos), value(value)
	{
	}

	SynBase* value;

	static const unsigned myTypeID = __LINE__;
};

struct SynDereference: SynBase
{
	SynDereference(const char* pos, SynBase* value): SynBase(myTypeID, pos), value(value)
	{
	}

	SynBase* value;

	static const unsigned myTypeID = __LINE__;
};

struct SynSizeof: SynBase
{
	SynSizeof(const char* pos, SynBase* value): SynBase(myTypeID, pos), value(value)
	{
	}

	SynBase* value;

	static const unsigned myTypeID = __LINE__;
};

struct SynNew: SynBase
{
	SynNew(const char* pos, SynBase *type, IntrusiveList<SynCallArgument> arguments, SynBase *count): SynBase(myTypeID, pos), type(type), arguments(arguments), count(count)
	{
	}

	SynBase *type;
	IntrusiveList<SynCallArgument> arguments;
	SynBase *count;

	static const unsigned myTypeID = __LINE__;
};

struct SynReturn: SynBase
{
	SynReturn(const char* pos, SynBase* value): SynBase(myTypeID, pos), value(value)
	{
	}

	SynBase *value;

	static const unsigned myTypeID = __LINE__;
};

struct SynYield: SynBase
{
	SynYield(const char* pos, SynBase* value): SynBase(myTypeID, pos), value(value)
	{
	}

	SynBase *value;

	static const unsigned myTypeID = __LINE__;
};

struct SynBreak: SynBase
{
	SynBreak(const char* pos, SynNumber* number): SynBase(myTypeID, pos), number(number)
	{
	}

	SynNumber* number;

	static const unsigned myTypeID = __LINE__;
};

struct SynContinue: SynBase
{
	SynContinue(const char* pos, SynNumber* number): SynBase(myTypeID, pos), number(number)
	{
	}

	SynNumber* number;

	static const unsigned myTypeID = __LINE__;
};

struct SynBlock: SynBase
{
	SynBlock(const char* pos, IntrusiveList<SynBase> expressions): SynBase(myTypeID, pos), expressions(expressions)
	{
	}

	IntrusiveList<SynBase> expressions;

	static const unsigned myTypeID = __LINE__;
};

struct SynIfElse: SynBase
{
	SynIfElse(const char* pos, SynBase* condition, SynBase* trueBlock, SynBase* falseBlock): SynBase(myTypeID, pos), condition(condition), trueBlock(trueBlock), falseBlock(falseBlock)
	{
	}

	SynBase* condition;
	SynBase* trueBlock;
	SynBase* falseBlock;

	static const unsigned myTypeID = __LINE__;
};

struct SynFor: SynBase
{
	SynFor(const char* pos, SynBase* initializer, SynBase* condition, SynBase* increment, SynBase* body): SynBase(myTypeID, pos), initializer(initializer), condition(condition), increment(increment), body(body)
	{
	}

	SynBase* initializer;
	SynBase* condition;
	SynBase* increment;
	SynBase* body;

	static const unsigned myTypeID = __LINE__;
};

enum SynUnaryOpType
{
	SYN_UNARY_OP_UNKNOWN,

	SYN_UNARY_OP_PLUS,
	SYN_UNARY_OP_NEGATE,
	SYN_UNARY_OP_BIT_NOT,
	SYN_UNARY_OP_LOGICAL_NOT,
};

struct SynUnaryOp: SynBase
{
	SynUnaryOp(const char* pos, SynUnaryOpType type, SynBase* value): SynBase(myTypeID, pos), type(type), value(value)
	{
	}

	SynUnaryOpType type;
	SynBase* value;

	static const unsigned myTypeID = __LINE__;
};

enum SynBinaryOpType
{
	SYN_BINARY_OP_UNKNOWN,

	SYN_BINARY_OP_ADD,
	SYN_BINARY_OP_SUB,
	SYN_BINARY_OP_MUL,
	SYN_BINARY_OP_DIV,
	SYN_BINARY_OP_MOD,
	SYN_BINARY_OP_POW,
	SYN_BINARY_OP_SHL,
	SYN_BINARY_OP_SHR,
	SYN_BINARY_OP_LESS,
	SYN_BINARY_OP_LESS_EQUAL,
	SYN_BINARY_OP_GREATER,
	SYN_BINARY_OP_GREATER_EQUAL,
	SYN_BINARY_OP_EQUAL,
	SYN_BINARY_OP_NOT_EQUAL,
	SYN_BINARY_OP_BIT_AND,
	SYN_BINARY_OP_BIT_OR,
	SYN_BINARY_OP_BIT_XOR,
	SYN_BINARY_OP_LOGICAL_AND,
	SYN_BINARY_OP_LOGICAL_OR,
	SYN_BINARY_OP_LOGICAL_XOR,
	SYN_BINARY_OP_IN,
};

struct SynBinaryOpElement
{
	SynBinaryOpElement(): pos(0), type(SYN_BINARY_OP_UNKNOWN), value(0)
	{
	}

	SynBinaryOpElement(const char* pos, SynBinaryOpType type, SynBase* value): pos(pos), type(type), value(value)
	{
	}

	const char* pos;
	SynBinaryOpType type;
	SynBase* value;
};

struct SynBinaryOp: SynBase
{
	SynBinaryOp(const char* pos, SynBinaryOpType type, SynBase* lhs, SynBase* rhs): SynBase(myTypeID, pos), type(type), lhs(lhs), rhs(rhs)
	{
	}

	SynBinaryOpType type;
	SynBase* lhs;
	SynBase* rhs;

	static const unsigned myTypeID = __LINE__;
};

struct SynAssignment: SynBase
{
	SynAssignment(const char* pos, SynBase* lhs, SynBase* rhs): SynBase(myTypeID, pos), lhs(lhs), rhs(rhs)
	{
	}

	SynBase* lhs;
	SynBase* rhs;

	static const unsigned myTypeID = __LINE__;
};

enum SynModifyAssignType
{
	SYN_MODIFY_ASSIGN_UNKNOWN,

	SYN_MODIFY_ASSIGN_ADD,
	SYN_MODIFY_ASSIGN_SUB,
	SYN_MODIFY_ASSIGN_MUL,
	SYN_MODIFY_ASSIGN_DIV,
	SYN_MODIFY_ASSIGN_POW,
	SYN_MODIFY_ASSIGN_MOD,
	SYN_MODIFY_ASSIGN_SHL,
	SYN_MODIFY_ASSIGN_SHR,
	SYN_MODIFY_ASSIGN_BIT_AND,
	SYN_MODIFY_ASSIGN_BIT_OR,
	SYN_MODIFY_ASSIGN_BIT_XOR,
};

struct SynModifyAssignment: SynBase
{
	SynModifyAssignment(const char* pos, SynModifyAssignType type, SynBase* lhs, SynBase* rhs): SynBase(myTypeID, pos), type(type), lhs(lhs), rhs(rhs)
	{
	}

	SynModifyAssignType type;
	SynBase* lhs;
	SynBase* rhs;

	static const unsigned myTypeID = __LINE__;
};

struct SynVariableDefinition: SynBase
{
	SynVariableDefinition(const char* pos, InplaceStr name, SynBase *initializer): SynBase(myTypeID, pos), name(name), initializer(initializer)
	{
	}

	InplaceStr name;
	SynBase *initializer;

	static const unsigned myTypeID = __LINE__;
};

struct SynVariableDefinitions: SynBase
{
	SynVariableDefinitions(const char* pos, SynBase *type, IntrusiveList<SynVariableDefinition> definitions): SynBase(myTypeID, pos), type(type), definitions(definitions)
	{
	}

	SynBase *type;
	IntrusiveList<SynVariableDefinition> definitions;

	static const unsigned myTypeID = __LINE__;
};

struct SynAccessor: SynBase
{
	SynAccessor(const char* pos, SynBase *type, InplaceStr name, SynBase *getBlock, SynBase *setBlock, InplaceStr setName): SynBase(myTypeID, pos), type(type), name(name), getBlock(getBlock), setBlock(setBlock), setName(setName)
	{
	}

	SynBase *type;
	InplaceStr name;
	SynBase *getBlock;
	SynBase *setBlock;
	InplaceStr setName;

	static const unsigned myTypeID = __LINE__;
};

struct SynFunctionArgument: SynBase
{
	SynFunctionArgument(const char* pos, SynBase* type, InplaceStr name, SynBase* defaultValue): SynBase(myTypeID, pos), type(type), name(name), defaultValue(defaultValue)
	{
	}

	SynBase* type;
	InplaceStr name;
	SynBase* defaultValue;

	static const unsigned myTypeID = __LINE__;
};

struct SynFunctionDefinition: SynBase
{
	SynFunctionDefinition(const char* pos, bool prototype, bool coroutine, SynBase *parentType, bool accessor, SynBase *returnType, InplaceStr name, IntrusiveList<SynFunctionArgument> arguments, IntrusiveList<SynBase> expressions): SynBase(myTypeID, pos), prototype(prototype), coroutine(coroutine), parentType(parentType), accessor(accessor), returnType(returnType), name(name), arguments(arguments), expressions(expressions)
	{
	}

	bool prototype;
	bool coroutine;
	SynBase *parentType;
	bool accessor;
	SynBase *returnType;
	InplaceStr name;
	IntrusiveList<SynFunctionArgument> arguments;
	IntrusiveList<SynBase> expressions;

	static const unsigned myTypeID = __LINE__;
};

struct SynClassPototype: SynBase
{
	SynClassPototype(const char* pos, InplaceStr name): SynBase(myTypeID, pos), name(name)
	{
	}

	InplaceStr name;

	static const unsigned myTypeID = __LINE__;
};

struct SynClassDefinition: SynBase
{
	SynClassDefinition(const char* pos, InplaceStr name, IntrusiveList<SynIdentifier> aliases, bool extendable, SynBase *baseClass, IntrusiveList<SynTypedef> typedefs, IntrusiveList<SynFunctionDefinition> functions, IntrusiveList<SynAccessor> accessors, IntrusiveList<SynVariableDefinitions> members): SynBase(myTypeID, pos), name(name), aliases(aliases), extendable(extendable), baseClass(baseClass), typedefs(typedefs), functions(functions), accessors(accessors), members(members)
	{
	}

	InplaceStr name;
	IntrusiveList<SynIdentifier> aliases;
	bool extendable;
	SynBase *baseClass;
	IntrusiveList<SynTypedef> typedefs;
	IntrusiveList<SynFunctionDefinition> functions;
	IntrusiveList<SynAccessor> accessors;
	IntrusiveList<SynVariableDefinitions> members;

	static const unsigned myTypeID = __LINE__;
};

struct SynNamespaceDefinition: SynBase
{
	SynNamespaceDefinition(const char* pos, IntrusiveList<SynIdentifier> path, IntrusiveList<SynBase> expressions): SynBase(myTypeID, pos), path(path), expressions(expressions)
	{
	}

	IntrusiveList<SynIdentifier> path;
	IntrusiveList<SynBase> expressions;

	static const unsigned myTypeID = __LINE__;
};

struct SynModuleImport: SynBase
{
	SynModuleImport(const char* pos, IntrusiveList<SynIdentifier> path): SynBase(myTypeID, pos), path(path)
	{
	}

	IntrusiveList<SynIdentifier> path;

	static const unsigned myTypeID = __LINE__;
};

struct SynModule: SynBase
{
	SynModule(const char* pos, IntrusiveList<SynModuleImport> imports, IntrusiveList<SynBase> expressions): SynBase(myTypeID, pos), imports(imports), expressions(expressions)
	{
	}

	IntrusiveList<SynModuleImport> imports;

	IntrusiveList<SynBase> expressions;

	static const unsigned myTypeID = __LINE__;
};

SynBase* Parse(ParseContext &context, const char *code);
