#include "runtime.h"
#include <memory>
#include <math.h>
#include <time.h>
#include <stdarg.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _MSC_VER
	#include <stdint.h>
#endif

#undef assert
#define __assert(_Expression) if(!(_Expression)){ printf("assertion failed"); abort(); };

typedef uintptr_t markerType;

int	SafeSprintf(char* dst, size_t size, const char* src, ...)
{
	va_list args;
	va_start(args, src);

	int result = vsnprintf(dst, size, src, args);
	dst[size-1] = '\0';
	return (result == -1 || (size_t)result >= size) ? (int)size : result;
}

template<typename T>
class FastVector
{
public:
	FastVector()
	{
		data = (T*)malloc(sizeof(T) * 128);
		memset(data, 0, sizeof(T));
		max = 128;
		count = 0;
	}
	explicit FastVector(unsigned int reserved)
	{
		data = (T*)malloc(sizeof(T) * reserved);
		memset(data, 0, reserved * sizeof(T));
		max = reserved;
		count = 0;
	}
	~FastVector()
	{
		free(data);
	}
	void	reset()
	{
		free(data);
		data = (T*)malloc(sizeof(T) * 128);
		memset(data, 0, sizeof(T));
		max = 128;
		count = 0;
	}

	T*		push_back()
	{
		count++;
		if(count == max)
			grow(count);
		return &data[count - 1];
	};
	void		push_back(const T& val)
	{
		data[count++] = val;
		if(count == max)
			grow(count);
	};
	void		push_back(const T* valPtr, unsigned int elem)
	{
		if(count + elem >= max)
			grow(count + elem);
		for(unsigned int i = 0; i < elem; i++)
			data[count++] = valPtr[i];
	};
	T&		back()
	{
		return data[count-1];
	}
	unsigned int		size()
	{
		return count;
	}
	void		pop_back()
	{
		count--;
	}
	void		clear()
	{
		count = 0;
	}
	T&		operator[](unsigned int index)
	{
		return data[index];
	}
	void		resize(unsigned int newSize)
	{
		if(newSize >= max)
			grow(newSize);
		count = newSize;
	}
	void		shrink(unsigned int newSize)
	{
		count = newSize;
	}
	void		reserve(unsigned int resSize)
	{
		if(resSize >= max)
			grow(resSize);
	}

	void	grow(unsigned int newSize)
	{
		if(max + (max >> 1) > newSize)
			newSize = max + (max >> 1);
		else
			newSize += 32;
		T* newData;
		newData = (T*)malloc(sizeof(T) * newSize);
		memset(newData, 0, newSize * sizeof(T));
		memcpy(newData, data, max * sizeof(T));
		free(data);
		data = newData;
		max = newSize;
	}
	T	*data;
	unsigned int	max, count;
private:
	// Disable assignment and copy constructor
	void operator =(FastVector &r);
	FastVector(FastVector &r);
};

FastVector<NULLCTypeInfo> __nullcTypeList;
FastVector<NULLCMemberInfo> __nullcTypePart;

FastVector<__nullcFunction> funcTable;

FastVector<NULLCFuncInfo> funcTableExt;

unsigned __nullcRegisterType(unsigned hash, const char *name, unsigned size, unsigned subTypeID, int memberCount, unsigned category)
{
	for(unsigned int i = 0; i < __nullcTypeList.size(); i++)
	{
		if(__nullcTypeList[i].hash == hash)
			return i;
	}
	__nullcTypeList.push_back(NULLCTypeInfo());
	__nullcTypeList.back().hash = hash;
	__nullcTypeList.back().name = name;
	__nullcTypeList.back().size = size;
	__nullcTypeList.back().subTypeID = subTypeID;
	__nullcTypeList.back().memberCount = memberCount;
	__nullcTypeList.back().category = category;
	__nullcTypeList.back().members = 0;
	return __nullcTypeList.size() - 1;
}

void __nullcRegisterMembers(unsigned id, unsigned count, ...)
{
	if(__nullcTypeList[id].members || !count)
		return;

	va_list args;
	va_start(args, count);
	__nullcTypeList[id].members = __nullcTypePart.size();

	for(unsigned i = 0; i < count * 3; i++)
	{
		NULLCMemberInfo member;

		member.typeID = va_arg(args, unsigned);
		member.offset = va_arg(args, unsigned);
		member.name = va_arg(args, const char*);

		__nullcTypePart.push_back(member);
	}

	va_end(args);
}

unsigned __nullcGetTypeCount()
{
	return __nullcTypeList.size();
}

NULLCTypeInfo* __nullcGetTypeInfo(unsigned id)
{
	return &__nullcTypeList[id];
}

NULLCMemberInfo* __nullcGetTypeMembers(unsigned id)
{
	return &__nullcTypePart[__nullcTypeList[id].members];
}

bool nullcIsArray(unsigned int typeID)
{
	return __nullcGetTypeInfo(typeID)->category == NULLC_ARRAY;
}

const char* nullcGetTypeName(unsigned int typeID)
{
	return __nullcGetTypeInfo(typeID)->name;
}

unsigned int nullcGetArraySize(unsigned int typeID)
{
	return __nullcGetTypeInfo(typeID)->memberCount;
}

unsigned int nullcGetSubType(unsigned int typeID)
{
	return __nullcGetTypeInfo(typeID)->subTypeID;
}

unsigned int nullcGetTypeSize(unsigned int typeID)
{
	return __nullcGetTypeInfo(typeID)->size;
}

int			__nullcPow(int number, int power)
{
	if(power < 0)
		return number == 1 ? 1 : (number == -1 ? (power & 1 ? -1 : 1) : 0);

	int result = 1;
	while(power)
	{
		if(power & 1)
		{
			result *= number;
			power--;
		}
		number *= number;
		power >>= 1;
	}
	return result;
}

double		__nullcPow(double a, double b)
{
	return pow(a, b);
}

long long	__nullcPow(long long num, long long pow)
{
	if(pow < 0)
		return (num == 1 ? 1 : 0);
	if(pow == 0)
		return 1;
	if(pow == 1)
		return num;
	if(pow > 64)
		return num;
	long long res = 1;
	int power = (int)pow;
	while(power)
	{
		if(power & 0x01)
		{
			res *= num;
			power--;
		}
		num *= num;
		power >>= 1;
	}
	return res;
}

double		__nullcMod(double a, double b)
{
	return fmod(a, b);
}
int			__nullcPowSet(char *a, int b)
{
	return *a = (char)__nullcPow((int)*a, b);
}
int			__nullcPowSet(short *a, int b)
{
	return *a = (short)__nullcPow((int)*a, b);
}
int			__nullcPowSet(int *a, int b)
{
	return *a = __nullcPow(*a, b);
}
double		__nullcPowSet(float *a, double b)
{
	return *a = (float)__nullcPow((double)*a, b);
}
double		__nullcPowSet(double *a, double b)
{
	return *a = __nullcPow(*a, b);
}
long long	__nullcPowSet(long long *a, long long b)
{
	return *a = __nullcPow(*a, b);
}
void	__nullcSetArray(short arr[], short val, unsigned int count)
{
	for(unsigned int i = 0; i < count; i++)
		arr[i] = val;
}
void	__nullcSetArray(int arr[], int val, unsigned int count)
{
	for(unsigned int i = 0; i < count; i++)
		arr[i] = val;
}
void	__nullcSetArray(float arr[], float val, unsigned int count)
{
	for(unsigned int i = 0; i < count; i++)
		arr[i] = val;
}
void	__nullcSetArray(double arr[], double val, unsigned int count)
{
	for(unsigned int i = 0; i < count; i++)
		arr[i] = val;
}
void	__nullcSetArray(long long arr[], long long val, unsigned int count)
{
	for(unsigned int i = 0; i < count; i++)
		arr[i] = val;
}

namespace GC
{
	// Range of memory that is not checked. Used to exclude pointers to stack from marking and GC
	char	*unmanageableBase = NULL;
	char	*unmanageableTop = NULL;
}

void __nullcCloseUpvalue(__nullcUpvalue *&head, void *ptr)
{
	__nullcUpvalue *curr = head;
	
	GC::unmanageableBase = (char*)&curr;
	// close upvalue if it's target is equal to local variable, or it's address is out of stack
	while(curr && ((char*)curr->ptr == ptr || (char*)curr->ptr < GC::unmanageableBase || (char*)curr->ptr > GC::unmanageableTop))
	{
		__nullcUpvalue *next = curr->next;
		unsigned int size = curr->size;
		head = curr->next;
		memcpy(&curr->next, curr->ptr, size);
		curr->ptr = (unsigned int*)&curr->next;
		curr = next;
	}
}
NULLCFuncPtr<> __nullcMakeFunction(unsigned int id, void* context)
{
	NULLCFuncPtr<> ret;
	ret.id = id;
	ret.context = context;
	return ret;
}

NULLCRef __nullcMakeAutoRef(void* ptr, unsigned int typeID)
{
	NULLCRef ret;
	ret.ptr = (char*)ptr;
	ret.typeID = typeID;
	return ret;
}

NULLCRef __nullcMakeExtendableAutoRef(void* ptr)
{
	NULLCRef ret;
	ret.ptr = (char*)ptr;
	ret.typeID = ptr ? *(unsigned*)ptr : 0; // Take type from first class typeid member
	return ret;
}

void* __nullcGetAutoRef(const NULLCRef &ref, unsigned int typeID)
{
	__assert(ref.typeID == typeID);
	return (void*)ref.ptr;
}

NULLCAutoArray	__makeAutoArray(unsigned type, NULLCArray<void> arr)
{
	NULLCAutoArray ret;
	ret.size = arr.size;
	ret.ptr = arr.ptr;
	ret.typeID = type;
	return ret;
}

bool operator ==(const NULLCRef& a, const NULLCRef& b)
{
	return a.ptr == b.ptr && a.typeID == b.typeID;
}
bool operator !=(const NULLCRef& a, const NULLCRef& b)
{
	return a.ptr != b.ptr || a.typeID != b.typeID;
}
bool operator !(const NULLCRef& a)
{
	return !a.ptr;
}

struct const_string
{
	NULLCArray<char> arr;
};

void  assert(int val, const char* message, void* unused)
{
	if(!val)
		printf("%s\n", message);
	__assert(val);
}

void assert_void_ref_int_(int val, void* __context)
{
	__assert(val);
}

void assert_void_ref_int_char___(int val, NULLCArray<char> message, void* __context)
{
	if(!val)
		printf("%s\n", message.ptr);
	__assert(val);
}

int  __operatorEqual_int_ref_char___char___(NULLCArray<char> a, NULLCArray<char> b, void* unused)
{
	if(a.size != b.size)
		return 0;
	if(memcmp(a.ptr, b.ptr, a.size) == 0)
		return 1;
	return 0;
}

int __operatorNEqual_int_ref_char___char___(NULLCArray<char> a, NULLCArray<char> b, void* unused)
{
	return !__operatorEqual_int_ref_char___char___(a, b, 0);
}

NULLCArray<char>  __operatorAdd_char___ref_char___char___(NULLCArray<char> a, NULLCArray<char> b, void* unused)
{
	NULLCArray<char> ret;

	ret.size = a.size + b.size - 1;
	ret.ptr = (char*)(intptr_t)__newS_void_ref_ref_int_int_(ret.size, NULLC_BASETYPE_CHAR, 0);
	if(!ret.ptr)
		return ret;

	memcpy(ret.ptr, a.ptr, a.size);
	memcpy(ret.ptr + a.size - 1, b.ptr, b.size);

	return ret;
}

NULLCArray<char> __operatorAddSet_char___ref_char___ref_char___(NULLCArray<char> * a, NULLCArray<char> b, void* unused)
{
	return *a = __operatorAdd_char___ref_char___char___(*a, b, 0);
}

bool bool_bool_ref_bool_(bool a, void* __context)
{
	return a;
}

char char_char_ref_char_(char a, void* __context)
{
	return a;
}

short short_short_ref_short_(short a, void* __context)
{
	return a;
}

int int_int_ref_int_(int a, void* __context)
{
	return a;
}

long long long_long_ref_long_(long long a, void* __context)
{
	return a;
}

float float_float_ref_float_(float a, void* __context)
{
	return a;
}

double double_double_ref_double_(double a, void* __context)
{
	return a;
}

void bool__bool_void_ref_bool_(bool a, bool *target)
{
	*target = a;
}

void char__char_void_ref_char_(char a, char *target)
{
	*target = a;
}

void short__short_void_ref_short_(short a, short *target)
{
	*target = a;
}

void int__int_void_ref_int_(int a, int *target)
{
	*target = a;
}

void long__long_void_ref_long_(long long a, long long *target)
{
	*target = a;
}

void float__float_void_ref_float_(float a, float *target)
{
	*target = a;
}

void double__double_void_ref_double_(double a, double *target)
{
	*target = a;
}

int as_unsigned_int_ref_char_(char a, void* __context)
{
	return (int)a;
}

int as_unsigned_int_ref_short_(short a, void* __context)
{
	return (int)a;
}

long long as_unsigned_long_ref_int_(int a, void* __context)
{
	return (long long)a;
}

short short_short_ref_char___(NULLCArray<char> str, void* __context)
{
	return short(atoi(str.ptr));
}

void short__short_void_ref_char___(NULLCArray<char> str, short *__context)
{
	*__context = str.ptr ? short(atoi(str.ptr)) : 0;
}

NULLCArray<char> short__str_char___ref__(short* __context)
{
	int number = *__context;
	bool sign = 0;
	char buf[16];
	char *curr = buf;
	if(number < 0)
		sign = 1;

	*curr++ = (char)(abs(number % 10) + '0');
	while(number /= 10)
		*curr++ = (char)(abs(number % 10) + '0');
	if(sign)
		*curr++ = '-';
	NULLCArray<char> arr = __newA_int___ref_int_int_int_(1, (int)(curr - buf) + 1, 0, 0);
	char *str = arr.ptr;
	do 
	{
		--curr;
		*str++ = *curr;
	}while(curr != buf);
	return arr;
}

int int_int_ref_char___(NULLCArray<char> str, void* __context)
{
	return atoi(str.ptr);
}

void int__int_void_ref_char___(NULLCArray<char> str, int* __context)
{
	*__context = str.ptr ? atoi(str.ptr) : 0;
}

NULLCArray<char> int__str_char___ref__(int* __context)
{
	int number = *__context;
	bool sign = 0;
	char buf[16];
	char *curr = buf;
	if(number < 0)
		sign = 1;

	*curr++ = (char)(abs(number % 10) + '0');
	while(number /= 10)
		*curr++ = (char)(abs(number % 10) + '0');
	if(sign)
		*curr++ = '-';
	NULLCArray<char> arr = __newA_int___ref_int_int_int_(1, (int)(curr - buf) + 1, 0, 0);
	char *str = arr.ptr;
	do 
	{
		--curr;
		*str++ = *curr;
	}while(curr != buf);
	return arr;
}

long long long_long_ref_char___(NULLCArray<char> str, void* __context)
{
	return strtoll(str.ptr, 0, 10);
}

void long__long_void_ref_char___(NULLCArray<char> str, long long* __context)
{
	*__context = str.ptr ? strtoll(str.ptr, 0, 10) : 0;
}

NULLCArray<char> long__str_char___ref__(long long* __context)
{
	long long number = *__context;
	bool sign = 0;
	char buf[32];
	char *curr = buf;
	if(number < 0)
		sign = 1;

	*curr++ = (char)(abs(number % 10) + '0');
	while(number /= 10)
		*curr++ = (char)(abs(number % 10) + '0');
	if(sign)
		*curr++ = '-';
	NULLCArray<char> arr = __newA_int___ref_int_int_int_(1, (int)(curr - buf) + 1, 0, 0);
	char *str = arr.ptr;
	do 
	{
		--curr;
		*str++ = *curr;
	}while(curr != buf);
	return arr;
}

float float_float_ref_char___(NULLCArray<char> str, void* __context)
{
	return (float)atof(str.ptr);
}

void float__float_void_ref_char___(NULLCArray<char> str, float* __context)
{
	*__context = str.ptr ? (float)atof(str.ptr) : 0;
}

NULLCArray<char> float__str_char___ref_int_bool_(int precision, bool showExponent, float* __context)
{
	char buf[256];
	SafeSprintf(buf, 256, showExponent ? "%.*e" : "%.*f", precision, *__context);
	NULLCArray<char> arr = __newA_int___ref_int_int_int_(1, (int)strlen(buf) + 1, 0, 0);
	memcpy(arr.ptr, buf, arr.size);
	return arr;
}

double double_double_ref_char___(NULLCArray<char> str, void* __context)
{
	return atof(str.ptr);
}

void double__double_void_ref_char___(NULLCArray<char> str, double* __context)
{
	*__context = str.ptr ? atof(str.ptr) : 0;
}

NULLCArray<char> double__str_char___ref_int_bool_(int precision, bool showExponent, double* r)
{
	char buf[256];
	SafeSprintf(buf, 256, showExponent ? "%.*e" : "%.*f", precision, *r);
	NULLCArray<char> arr = __newA_int___ref_int_int_int_(1, (int)strlen(buf) + 1, 0, 0);
	memcpy(arr.ptr, buf, arr.size);
	return arr;
}

void* __newS_void_ref_ref_int_int_(int size, int type, void* __context)
{
	return NULLC::AllocObject(size, type);
}

NULLCArray<int> __newA_int___ref_int_int_int_(int size, int count, int type, void* __context)
{
	return NULLC::AllocArray(size, count, type);
}

NULLCRef duplicate_auto_ref_ref_auto_ref_(NULLCRef obj, void* unused)
{
	NULLCRef ret;
	ret.typeID = obj.typeID;
	unsigned int objSize = nullcGetTypeSize(ret.typeID);
	ret.ptr = (char*)__newS_void_ref_ref_int_int_(objSize, 0, 0);
	memcpy(ret.ptr, obj.ptr, objSize);
	return ret;
}

void __duplicate_array_void_ref_auto___ref_auto___(NULLCAutoArray* dst, NULLCAutoArray src, void* unused)
{
	dst->typeID = src.typeID;
	dst->len = src.len;
	dst->ptr = (char*)NULLC::AllocArray(nullcGetTypeSize(src.typeID), src.len, src.typeID).ptr;
	memcpy(dst->ptr, src.ptr, src.len * nullcGetTypeSize(src.typeID));
}

NULLCAutoArray duplicate_auto___ref_auto___(NULLCAutoArray arr, void* unused)
{
	NULLCAutoArray ret;
	__duplicate_array_void_ref_auto___ref_auto___(&ret, arr, NULL);
	return ret;
}

NULLCRef replace_auto_ref_ref_auto_ref_auto_ref_(NULLCRef l, NULLCRef r, void* unused)
{
	if(l.typeID != r.typeID)
	{
		nullcThrowError("ERROR: cannot convert from %s ref to %s ref", __nullcGetTypeInfo(r.typeID)->name, __nullcGetTypeInfo(l.typeID)->name);
		return l;
	}
	memcpy(l.ptr, r.ptr, nullcGetTypeSize(r.typeID));
	return l;
}

void swap_void_ref_auto_ref_auto_ref_(NULLCRef l, NULLCRef r, void* unused)
{
	if(l.typeID != r.typeID)
	{
		nullcThrowError("ERROR: types don't match (%s ref, %s ref)", __nullcGetTypeInfo(r.typeID)->name, __nullcGetTypeInfo(l.typeID)->name);
		return;
	}
	unsigned size = nullcGetTypeSize(l.typeID);

	char tmpStack[512];
	// $$ should use some extendable static storage for big objects
	char *tmp = size < 512 ? tmpStack : (char*)NULLC::AllocObject(size, l.typeID);
	memcpy(tmp, l.ptr, size);
	memcpy(l.ptr, r.ptr, size);
	memcpy(r.ptr, tmp, size);
}

int equal_int_ref_auto_ref_auto_ref_(NULLCRef l, NULLCRef r, void* unused)
{
	if(l.typeID != r.typeID)
	{
		nullcThrowError("ERROR: types don't match (%s ref, %s ref)", __nullcGetTypeInfo(r.typeID)->name, __nullcGetTypeInfo(l.typeID)->name);
		return 0;
	}
	return 0 == memcmp(l.ptr, r.ptr, nullcGetTypeSize(l.typeID));
}

void assign_void_ref_auto_ref_auto_ref_(NULLCRef l, NULLCRef r, void* unused)
{
	if(nullcGetSubType(l.typeID) != r.typeID)
	{
		nullcThrowError("ERROR: can't assign value of type %s to a pointer of type %s", __nullcGetTypeInfo(r.typeID)->name, __nullcGetTypeInfo(l.typeID)->name);
		return;
	}
	memcpy(l.ptr, &r.ptr, nullcGetTypeSize(l.typeID));
}

void array_copy_void_ref_auto___auto___(NULLCAutoArray l, NULLCAutoArray r, void* __context)
{
	if(l.ptr == r.ptr)
		return;

	if(l.typeID != r.typeID)
	{
		nullcThrowError("ERROR: destination element type '%s' doesn't match source element type '%s'", nullcGetTypeName(l.typeID), nullcGetTypeName(r.typeID));
		return;
	}
	if(l.len < r.len)
	{
		nullcThrowError("ERROR: destination array size '%d' is smaller than source array size '%s'", l.len, r.len);
		return;
	}

	memcpy(l.ptr, r.ptr, nullcGetTypeSize(l.typeID) * r.len);
}

NULLCFuncPtr<__typeProxy_void_ref__> __redirect_void_ref___ref_auto_ref___function___ref_(NULLCRef r, NULLCArray<__function>* arr, void* __context)
{
	unsigned int *funcs = (unsigned int*)arr->ptr;
	NULLCFuncPtr<> ret;
	if(r.typeID > arr->size)
	{
		nullcThrowError("ERROR: type index is out of bounds of redirection table");
		return ret;
	}
	// If there is no implementation for a method
	if(!funcs[r.typeID])
	{
		// Find implemented function ID as a type reference
		unsigned int found = 0;
		for(; found < arr->size; found++)
		{
			if(funcs[found])
				break;
		}
		//if(found == arr->size)
		nullcThrowError("ERROR: type '%s' doesn't implement method", nullcGetTypeName(r.typeID));
		//else
		//	nullcThrowError("ERROR: type '%s' doesn't implement method '%s%s' of type '%s'", nullcGetTypeName(r.typeID), nullcGetTypeName(r.typeID), strchr(nullcGetFunctionName(funcs[found]), ':'), nullcGetTypeName(nullcGetFunctionType(funcs[found])));
		return ret;
	}
	ret.context = r.ptr;
	ret.id = funcs[r.typeID];
	return ret;
}

NULLCFuncPtr<__typeProxy_void_ref__> __redirect_ptr_void_ref___ref_auto_ref___function___ref_(NULLCRef r, NULLCArray<__function>* arr, void* __context)
{
	NULLCFuncPtr<> ret;

	if(!arr)
	{
		nullcThrowError("ERROR: null pointer access");
		return ret;
	}

	unsigned int *funcs = (unsigned int*)arr->ptr;

	if(r.typeID >= arr->size)
	{
		nullcThrowError("ERROR: type index is out of bounds of redirection table");
		return ret;
	}

	ret.context = funcs[r.typeID] ? r.ptr : 0;
	ret.id = funcs[r.typeID];

	return ret;
}

NULLCArray<char>* __operatorSet_char___ref_ref_char___ref_int___(NULLCArray<char>* dst, NULLCArray<int> src, void* __context)
{
	if(dst->size < src.size)
		*dst = __newA_int___ref_int_int_int_(1, src.size, 0, 0);
	for(int i = 0; i < src.size; i++)
		((char*)dst->ptr)[i] = ((int*)src.ptr)[i];
	return dst;
}

NULLCArray<short>* __operatorSet_short___ref_ref_short___ref_int___(NULLCArray<short>* dst, NULLCArray<int> src, void* __context)
{
	if(dst->size < src.size)
		*dst = __newA_int___ref_int_int_int_(2, src.size, 0, 0);
	for(int i = 0; i < src.size; i++)
		((short*)dst->ptr)[i] = ((int*)src.ptr)[i];
	return dst;
}

NULLCArray<float>* __operatorSet_float___ref_ref_float___ref_double___(NULLCArray<float>* dst, NULLCArray<double> src, void* __context)
{
	if(dst->size < src.size)
		*dst = __newA_int___ref_int_int_int_(4, src.size, 0, 0);
	for(int i = 0; i < src.size; i++)
		((float*)dst->ptr)[i] = ((double*)src.ptr)[i];
	return dst;
}

unsigned typeid_typeid_ref_auto_ref_(NULLCRef type, void* __context)
{
	// TODO: extendable type redirect

	return type.typeID;
}

int typeid__size__int_ref___(unsigned * __context)
{
	return __nullcGetTypeInfo(*__context)->size;
}

int  __operatorEqual_int_ref_typeid_typeid_(unsigned a, unsigned b, void* unused)
{
	return a == b;
}
int  __operatorNEqual_int_ref_typeid_typeid_(unsigned a, unsigned b, void* unused)
{
	return a != b;
}

int __rcomp_int_ref_auto_ref_auto_ref_(NULLCRef a, NULLCRef b, void* unused)
{
	return a.ptr == b.ptr;
}

int __rncomp_int_ref_auto_ref_auto_ref_(NULLCRef a, NULLCRef b, void* unused)
{
	return a.ptr != b.ptr;
}

bool __operatorLess_bool_ref_auto_ref_auto_ref_(NULLCRef a, NULLCRef b, void* unused)
{
	return uintptr_t(a.ptr) < uintptr_t(b.ptr);
}

bool __operatorLEqual_bool_ref_auto_ref_auto_ref_(NULLCRef a, NULLCRef b, void* unused)
{
	return uintptr_t(a.ptr) <= uintptr_t(b.ptr);
}

bool __operatorGreater_bool_ref_auto_ref_auto_ref_(NULLCRef a, NULLCRef b, void* unused)
{
	return uintptr_t(a.ptr) > uintptr_t(b.ptr);
}

bool __operatorGEqual_bool_ref_auto_ref_auto_ref_(NULLCRef a, NULLCRef b, void* unused)
{
	return uintptr_t(a.ptr) >= uintptr_t(b.ptr);
}

int hash_value_int_ref_auto_ref_(NULLCRef a, void* unused)
{
	long long value = (long long)(intptr_t)(a.ptr);
	return (int)((value >> 32) ^ value);
}

int __pcomp_int_ref_void_ref_int__void_ref_int__(NULLCFuncPtr<__typeProxy_void_ref_int_> a, NULLCFuncPtr<__typeProxy_void_ref_int_> b, void* __context)
{
	return a.context == b.context && a.id == b.id;
}

int __pncomp_int_ref_void_ref_int__void_ref_int__(NULLCFuncPtr<__typeProxy_void_ref_int_> a, NULLCFuncPtr<__typeProxy_void_ref_int_> b, void* __context)
{
	return a.context != b.context || a.id != b.id;
}

int __acomp_int_ref_auto___auto___(NULLCAutoArray a, NULLCAutoArray b, void* __context)
{
	return a.size == b.size && a.ptr == b.ptr;
}

int __ancomp_int_ref_auto___auto___(NULLCAutoArray a, NULLCAutoArray b, void* __context)
{
	return a.size != b.size || a.ptr != b.ptr;
}

int __typeCount_int_ref__(void* __context)
{
	return __nullcTypeList.size() + 1024;
}

NULLCAutoArray* __operatorSet_auto___ref_ref_auto___ref_auto_ref_(NULLCAutoArray* left, NULLCRef right, void* unused)
{
	if(!nullcIsArray(right.typeID))
	{
		nullcThrowError("ERROR: cannot convert from '%s' to 'auto[]'", nullcGetTypeName(right.typeID));
		return NULL;
	}
	left->len = nullcGetArraySize(right.typeID);
	if(left->len == ~0u)
	{
		NULLCArray<char> *arr = (NULLCArray<char>*)right.ptr;
		left->len = arr->size;
		left->ptr = arr->ptr;
	}else{
		left->ptr = right.ptr;
	}
	left->typeID = nullcGetSubType(right.typeID);
	return left;
}

NULLCRef __aaassignrev_auto_ref_ref_auto_ref_auto___ref_(NULLCRef left, NULLCAutoArray *right, void* unused)
{
	NULLCRef ret = { 0, 0 };
	if(!nullcIsArray(left.typeID))
	{
		nullcThrowError("ERROR: cannot convert from 'auto[]' to '%s'", nullcGetTypeName(left.typeID));
		return ret;
	}
	if(nullcGetSubType(left.typeID) != right->typeID)
	{
		nullcThrowError("ERROR: cannot convert from 'auto[]' (actual type '%s[%d]') to '%s'", nullcGetTypeName(right->typeID), right->len, nullcGetTypeName(left.typeID));
		return ret;
	}
	unsigned int leftLength = nullcGetArraySize(left.typeID);
	if(leftLength == ~0u)
	{
		NULLCArray<char> *arr = (NULLCArray<char>*)left.ptr;
		arr->size = right->len;
		arr->ptr = right->ptr;
	}else{
		if(leftLength != right->len)
		{
			nullcThrowError("ERROR: cannot convert from 'auto[]' (actual type '%s[%d]') to '%s'", nullcGetTypeName(right->typeID), right->len, nullcGetTypeName(left.typeID));
			return ret;
		}
		memcpy(left.ptr, right->ptr, leftLength * nullcGetTypeSize(right->typeID));
	}
	return left;
}

NULLCRef __operatorIndex_auto_ref_ref_auto___ref_int_(NULLCAutoArray* left, int index, void* unused)
{
	NULLCRef ret = { 0, 0 };
	if(index >= left->len)
	{
		nullcThrowError("ERROR: array index out of bounds");
		return ret;
	}
	ret.typeID = left->typeID;
	ret.ptr = (char*)left->ptr + index * nullcGetTypeSize(ret.typeID);
	return ret;
}

int const_string__size__int_ref___(const_string* str)
{
	return str->arr.size;
}

const_string* __operatorSet_const_string_ref_ref_const_string_ref_char___(const_string* l, NULLCArray<char> arr, void* __context)
{
	l->arr = arr;
	return l;
}

const_string const_string_const_string_ref_char___(NULLCArray<char> arr, void* unused)
{
	const_string str;
	str.arr = arr;
	return str;
}


char __operatorIndex_char_ref_const_string_ref_int_(const_string* l, int index, void* __context)
{
	// TODO: index check
	return l->arr.ptr[index];
}

int __operatorEqual_int_ref_const_string_const_string_(const_string a, const_string b, void* __context)
{
	return __operatorEqual_int_ref_char___char___(a.arr, b.arr, 0);
}

int __operatorEqual_int_ref_const_string_char___(const_string a, NULLCArray<char> b, void* __context)
{
	return __operatorEqual_int_ref_char___char___(a.arr, b, 0);
}

int __operatorEqual_int_ref_char___const_string_(NULLCArray<char> a, const_string b, void* __context)
{
	return __operatorEqual_int_ref_char___char___(a, b.arr, 0);
}

int __operatorNEqual_int_ref_const_string_const_string_(const_string a, const_string b, void* __context)
{
	return __operatorNEqual_int_ref_char___char___(a.arr, b.arr, 0);
}

int __operatorNEqual_int_ref_const_string_char___(const_string a, NULLCArray<char> b, void* __context)
{
	return __operatorNEqual_int_ref_char___char___(a.arr, b, 0);
}

int __operatorNEqual_int_ref_char___const_string_(NULLCArray<char> a, const_string b, void* __context)
{
	return __operatorNEqual_int_ref_char___char___(a, b.arr, 0);
}

const_string __operatorAdd_const_string_ref_const_string_const_string_(const_string a, const_string b, void* __context)
{
	const_string ret;
	ret.arr = __operatorAdd_char___ref_char___char___(a.arr, b.arr, 0);
	return ret;
}

const_string __operatorAdd_const_string_ref_const_string_char___(const_string a, NULLCArray<char> b, void* __context)
{
	const_string ret;
	ret.arr = __operatorAdd_char___ref_char___char___(a.arr, b, 0);
	return ret;
}

const_string __operatorAdd_const_string_ref_char___const_string_(NULLCArray<char> a, const_string b, void* __context)
{
	const_string ret;
	ret.arr = __operatorAdd_char___ref_char___char___(a, b.arr, 0);
	return ret;
}

int isStackPointer_int_ref_auto_ref_(NULLCRef ptr, void* unused)
{
	GC::unmanageableBase = (char*)&ptr;
	return ptr.ptr >= GC::unmanageableBase && ptr.ptr <= GC::unmanageableTop;
}

void auto_array_impl_void_ref_auto___ref_typeid_int_(NULLCAutoArray* arr, unsigned type, int count, void* unused)
{
	arr->typeID = type;
	arr->len = count;
	arr->ptr = (char*)__newS_void_ref_ref_int_int_(typeid__size__int_ref___(&type) * (count), type, 0);
}

NULLCAutoArray auto_array_auto___ref_typeid_int_(unsigned type, int count, void* unused)
{
	NULLCAutoArray res;
	auto_array_impl_void_ref_auto___ref_typeid_int_(&res, type, count, NULL);
	return res;
}

void auto____set_void_ref_auto_ref_int_(NULLCRef x, int pos, NULLCAutoArray* arr)
{
	if(x.typeID != arr->typeID)
	{
		nullcThrowError("ERROR: cannot convert from '%s' to an 'auto[]' element type '%s'", nullcGetTypeName(x.typeID), nullcGetTypeName(arr->typeID));
		return;
	}
	unsigned elemSize = __nullcGetTypeInfo(arr->typeID)->size;
	if(pos >= arr->len)
	{
		unsigned newSize = 1 + arr->len + (arr->len >> 1);
		if(pos >= newSize)
			newSize = pos;
		NULLCAutoArray n;
		auto_array_impl_void_ref_auto___ref_typeid_int_(&n, arr->typeID, newSize, NULL);
		memcpy(n.ptr, arr->ptr, arr->len * elemSize);
		*arr = n;
	}
	memcpy(arr->ptr + elemSize * pos, x.ptr, elemSize);
}

void __force_size_void_ref_auto___ref_int_(NULLCAutoArray* arr, int size, void* unused)
{
	if(size > (unsigned)arr->len)
	{
		nullcThrowError("ERROR: cannot extend array");
		return;
	}
	arr->len = size;
}

int isCoroutineReset_int_ref_auto_ref_(NULLCRef f, void* unused)
{
	if(__nullcGetTypeInfo(f.typeID)->category != NULLC_FUNCTION)
	{
		nullcThrowError("Argument is not a function");
		return 0;
	}
	NULLCFuncPtr<> *fPtr = (NULLCFuncPtr<>*)f.ptr;
	if(funcTableExt[fPtr->id].funcType != FunctionCategory::COROUTINE)
	{
		nullcThrowError("Function is not a coroutine");
		return 0;
	}

	unsigned jmpOffset = *(unsigned*)fPtr->context;
	return jmpOffset == 0;
}

void __assertCoroutine_void_ref_auto_ref_(NULLCRef f, void* unused)
{
	if(__nullcGetTypeInfo(f.typeID)->category != NULLC_FUNCTION)
		nullcThrowError("Argument is not a function");
	NULLCFuncPtr<> *fPtr = (NULLCFuncPtr<>*)f.ptr;
	if(funcTableExt[fPtr->id].funcType != FunctionCategory::COROUTINE)
		nullcThrowError("ERROR: function is not a coroutine");
}

NULLCArray<NULLCRef> __getFinalizeList_auto_ref___ref__(void* __context)
{
	NULLCArray<NULLCRef> ret;
	return ret;
}

void __FinalizeProxy__finalize_void_ref__(__FinalizeProxy* __context)
{
}

void __finalizeObjects_void_ref__(void* __context)
{
}

void* assert_derived_from_base_void_ref_ref_void_ref_typeid_(void* derived, unsigned base, void* unused)
{
	if(!derived)
		return derived;

	unsigned typeId = *(unsigned*)derived;

	for(;;)
	{
		if(base == typeId)
			return derived;

		NULLCTypeInfo *info = __nullcGetTypeInfo(typeId);

		if(info->category == NULLC_CLASS && info->baseClassID != 0)
		{
			typeId = info->baseClassID;
		}
		else
		{
			break;
		}
	}

	nullcThrowError("ERROR: cannot convert from '%s' to '%s'", nullcGetTypeName(*(unsigned*)derived), nullcGetTypeName(base));

	return derived;
}

void __closeUpvalue_void_ref_void_ref_ref_void_ref_int_int_(void **upvalueList, void *variable, int offset, int size, void* __context)
{
	if (!upvalueList || !variable)
	{
		nullcThrowError("ERROR: null pointer access");
		return;
	}

	struct Upvalue
	{
		void *target;
		Upvalue *next;
	};

	Upvalue *upvalue = *(Upvalue**)upvalueList;

	while (upvalue && upvalue->target == variable)
	{
		Upvalue *next = upvalue->next;

		char *copy = (char*)upvalue + offset;
		memcpy(copy, variable, unsigned(size));
		upvalue->target = copy;
		upvalue->next = NULL;

		upvalue = next;
	}

	*(Upvalue**)upvalueList = upvalue;
}

int float__str_610894668_precision__int_ref___(void* __context)
{
	return 6;
}

bool float__str_610894668_showExponent__bool_ref___(void* __context)
{
	return false;
}

int double__str_610894668_precision__int_ref___(void* __context)
{
	return 6;
}

bool double__str_610894668_showExponent__bool_ref___(void* __context)
{
	return false;
}

void nullcThrowError(const char* error, ...)
{
	va_list args;
	va_start(args, error);
	vprintf(error, args);
	va_end(args);
}

__nullcFunctionArray* __nullcGetFunctionTable()
{
	return &funcTable.data;
}

unsigned int __nullcGetStringHash(const char *str)
{
	unsigned int hash = 5381;
	int c;
	while((c = *str++) != 0)
		hash = ((hash << 5) + hash) + c;
	return hash;
}

unsigned __nullcRegisterFunction(const char* name, void* fPtr, unsigned extraType, unsigned funcType)
{
	unsigned hash = __nullcGetStringHash(name);

	for(unsigned int i = 0; i < funcTable.size(); i++)
	{
		if(funcTableExt[i].hash == hash)
			return i;
	}

	funcTable.push_back(fPtr);

	NULLCFuncInfo info;

	info.hash = hash;
	info.name = name;
	info.extraType = extraType;
	info.funcType = funcType;

	funcTableExt.push_back(info);

	return funcTable.size() - 1;
}

NULLCFuncInfo* __nullcGetFunctionInfo(unsigned id)
{
	return &funcTableExt[id];
}

// Memory allocation and GC

#define GC_DEBUG_PRINT(...)
//#define GC_DEBUG_PRINT printf

#define NULLC_PTR_SIZE sizeof(void*)
namespace GC
{
	unsigned int	objectName = __nullcGetStringHash("auto ref");
	unsigned int	autoArrayName = __nullcGetStringHash("auto[]");

	void CheckArray(char* ptr, const NULLCTypeInfo& type);
	void CheckClass(char* ptr, const NULLCTypeInfo& type);
	void CheckFunction(char* ptr);
	void CheckVariable(char* ptr, const NULLCTypeInfo& type);

	// Function that marks memory blocks belonging to GC
	void MarkPointer(char* ptr, const NULLCTypeInfo& type, bool takeSubtype)
	{
		// We have pointer to stack that has a pointer inside, so 'ptr' is really a pointer to pointer
		char **rPtr = (char**)ptr;
		// Check for unmanageable ranges. Range of 0x00000000-0x00010000 is unmanageable by default due to upvalues with offsets inside closures.
		if(*rPtr > (char*)0x00010000 && (*rPtr < unmanageableBase || *rPtr > unmanageableTop))
		{
			// Get type that pointer points to
			GC_DEBUG_PRINT("\tGlobal pointer %s %p (at %p)\r\n", type.name, *rPtr, ptr);

			// Get pointer to the start of memory block. Some pointers may point to the middle of memory blocks
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(*rPtr);
			// If there is no base, this pointer points to memory that is not GCs memory
			if(!basePtr)
				return;
			GC_DEBUG_PRINT("\tPointer base is %p\r\n", basePtr);
			// Marker is 4 bytes before the block
			markerType *marker = (markerType*)(basePtr)-1;
			GC_DEBUG_PRINT("\tMarker is %d\r\n", *marker & 0xff);

			// If block is unmarked
			if((*marker & 0xff) == 0)
			{
				// Mark block as used
				*marker |= 1;
				GC_DEBUG_PRINT("Type near memory %d, type %d (%d)\n", *marker >> 8, type.subTypeID, takeSubtype);
				// And if type is not simple, check memory to which pointer points to
				if(type.category != NULLC_NONE)
					CheckVariable(*rPtr, takeSubtype ? __nullcTypeList[/*type.subTypeID*/*marker >> 8] : type);
			}else if(takeSubtype && __nullcTypeList[type.subTypeID].category == NULLC_POINTER){
				MarkPointer(*rPtr, __nullcTypeList[type.subTypeID], true); 
			}
		}
	}

	// Function that checks arrays for pointers
	void CheckArray(char* ptr, const NULLCTypeInfo& type)
	{
		// Get array element type
		NULLCTypeInfo *subType = type.hash == autoArrayName ? NULL : &__nullcTypeList[type.subTypeID];
		// Real array size (changed for unsized arrays)
		unsigned int size = type.memberCount;
		// If array type is an unsized array, check pointer that points to actual array contents
		if(size == -1)
		{
			// Get real array size
			size = ((NULLCArray<void>*)ptr)->size;
			// Switch pointer to array data
			char **rPtr = (char**)ptr;
			ptr = *rPtr;
			// If uninitialized or points to stack memory, return
			if(!ptr || ptr <= (char*)0x00010000 || (ptr >= unmanageableBase && ptr <= unmanageableTop))
				return;
			GC_DEBUG_PRINT("\tGlobal pointer %p\r\n", ptr);
			// Get base pointer
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(ptr);
			// If there is no base pointer or memory already marked, exit
			if(!basePtr || (*((unsigned int*)(basePtr) - 1) & 0xff))
				return;
			// Mark memory as used
			*((unsigned int*)(basePtr) - 1) |= 1;
		}else if(type.hash == autoArrayName){
			NULLCAutoArray *data = (NULLCAutoArray*)ptr;
			// Get real variable type
			subType = &__nullcTypeList[data->typeID];
			// skip uninitialized array
			if(!data->ptr)
				return;
			// Mark target data
			MarkPointer((char*)&data->ptr, *subType, false);
			// Switch pointer to target
			ptr = data->ptr;
			// Get array size
			size = data->len;
		}
		// Otherwise, check every array element is it's either array, pointer of class
		switch(subType->category)
		{
		case NULLC_ARRAY:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				CheckArray(ptr, *subType);
			break;
		case NULLC_POINTER:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				MarkPointer(ptr, *subType, true);
			break;
		case NULLC_CLASS:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				CheckClass(ptr, *subType);
			break;
		case NULLC_FUNCTION:
			for(unsigned int i = 0; i < size; i++, ptr += subType->size)
				CheckFunction(ptr);
			break;
		}
	}

	// Function that checks classes for pointers
	void CheckClass(char* ptr, const NULLCTypeInfo& type)
	{
		const NULLCTypeInfo *realType = &type;
		if(type.hash == objectName)
		{
			// Get real variable type
			realType = &__nullcTypeList[*(int*)ptr];
			// Switch pointer to target
			ptr = ((NULLCRef*)ptr)->ptr;
			// If uninitialized or points to stack memory, return
			if(!ptr || ptr <= (char*)0x00010000 || (ptr >= unmanageableBase && ptr <= unmanageableTop))
				return;
			// Get base pointer
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(ptr);
			// If there is no base pointer or memory already marked, exit
			if(!basePtr || (*((unsigned int*)(basePtr) - 1) & 0xff))
				return;
			// Mark memory as used
			*((unsigned int*)(basePtr) - 1) |= 1;
			// Fixup target
			CheckVariable(ptr, *realType);
			// Exit
			return;
		}else if(type.hash == autoArrayName){
			CheckArray(ptr, type);
			// Exit
			return;
		}
		// Get class member type list
		NULLCMemberInfo *memberList = &__nullcTypePart[realType->members];
		// Check pointer members
		for(unsigned int n = 0; n < realType->memberCount; n++)
		{
			// Get member type
			NULLCTypeInfo &subType = __nullcTypeList[memberList[n].typeID];
			unsigned int pos = memberList[n].offset;
			// Check member
			CheckVariable(ptr + pos, subType);
		}
	}

	// Function that checks function context for pointers
	void CheckFunction(char* ptr)
	{
		NULLCFuncPtr<> *fPtr = (NULLCFuncPtr<>*)ptr;
		// If there's no context, there's nothing to check
		if(!fPtr->context)
			return;
		const NULLCFuncInfo &func = funcTableExt[fPtr->id];
		// If context is "this" pointer
		if(func.extraType != ~0u)
			MarkPointer((char*)&fPtr->context, __nullcTypeList[func.extraType], true);
	}

	// Function that decides, how variable of type 'type' should be checked for pointers
	void CheckVariable(char* ptr, const NULLCTypeInfo& type)
	{
		switch(type.category)
		{
		case NULLC_ARRAY:
			CheckArray(ptr, type);
			break;
		case NULLC_POINTER:
			MarkPointer(ptr, type, true);
			break;
		case NULLC_CLASS:
			CheckClass(ptr, type);
			break;
		case NULLC_FUNCTION:
			CheckFunction(ptr);
			break;
		}
	}
}

struct GlobalRoot
{
	void	*ptr;
	unsigned typeID;
};
FastVector<GlobalRoot> rootSet;

// Main function for marking all pointers in a program
void MarkUsedBlocks()
{
	GC_DEBUG_PRINT("Unmanageable range: %p-%p\r\n", GC::unmanageableBase, GC::unmanageableTop);

	// Mark global variables
	for(unsigned int i = 0; i < rootSet.size(); i++)
	{
		GC_DEBUG_PRINT("Global %s (at %p)\r\n", __nullcTypeList[rootSet[i].typeID].name, rootSet[i].ptr);
		GC::CheckVariable((char*)rootSet[i].ptr, __nullcTypeList[rootSet[i].typeID]);
	}
	// Check that temporary stack range is correct
	assert(GC::unmanageableTop >= GC::unmanageableBase, "ERROR: GC - incorrect stack range", 0);
	char* tempStackBase = GC::unmanageableBase;
	// Check temporary stack for pointers
	while(tempStackBase < GC::unmanageableTop)
	{
		char *ptr = *(char**)(tempStackBase);
		// Check for unmanageable ranges. Range of 0x00000000-0x00010000 is unmanageable by default due to upvalues with offsets inside closures.
		if(ptr > (char*)0x00010000 && (ptr < GC::unmanageableBase || ptr > GC::unmanageableTop))
		{
			// Get pointer base
			unsigned int *basePtr = (unsigned int*)NULLC::GetBasePointer(ptr);
			// If there is no base, this pointer points to memory that is not GCs memory
			if(basePtr)
			{
				markerType *marker = (markerType*)(basePtr)-1;
				// If block is unmarked, mark it as used
				if((*marker & 0xff) == 0)
				{
					*marker |= 1;
					GC_DEBUG_PRINT("Found %s type %d on stack at %p\n", __nullcTypeList[*marker >> 8].name, *marker >> 8, ptr);
					GC::CheckVariable(ptr, __nullcTypeList[*marker >> 8]);
				}
			}
		}
		tempStackBase += 4;
	}
}

void __nullcRegisterGlobal(void* ptr, unsigned typeID)
{
	GlobalRoot entry;
	entry.ptr = ptr;
	entry.typeID = typeID;
	rootSet.push_back(entry);
}
void __nullcRegisterBase(void* ptr)
{
	GC::unmanageableTop = GC::unmanageableTop ? ((char*)ptr > GC::unmanageableTop ? (char*)ptr : GC::unmanageableTop) : (char*)ptr;
}

namespace NULLC
{
	void*	defaultAlloc(int size);
	void	defaultDealloc(void* ptr);

	void*	alignedAlloc(int size);
	void*	alignedAlloc(int size, int extraSize);
	void	alignedDealloc(void* ptr);
}

void*	NULLC::defaultAlloc(int size)
{
	return malloc(size);
}

void	NULLC::defaultDealloc(void* ptr)
{
	free(ptr);
}

void* NULLC::alignedAlloc(int size)
{
	void *unaligned = defaultAlloc((size + 16 - 1) + sizeof(void*));
	if(!unaligned)
		return NULL;
	void *ptr = (void*)(((intptr_t)unaligned + sizeof(void*) + 16 - 1) & ~(16 - 1));
	*((void**)ptr - 1) = unaligned;
	return ptr;
}

void* NULLC::alignedAlloc(int size, int extraSize)
{
	void *unaligned = defaultAlloc((size + 16 - 1) + sizeof(void*) + extraSize);
	if(!unaligned)
		return NULL;
	void *ptr = (void*)((((intptr_t)unaligned + sizeof(void*) + extraSize + 16 - 1) & ~(16 - 1)) - extraSize);
	*((void**)ptr - 1) = unaligned;
	return ptr;
}

void NULLC::alignedDealloc(void* ptr)
{
	defaultDealloc(*((void **)ptr - 1));
}

template<int elemSize>
union SmallBlock
{
	char			data[elemSize];
	markerType		marker;
	SmallBlock		*next;
};

template<int elemSize, int countInBlock>
struct LargeBlock
{
	typedef SmallBlock<elemSize> Block;

	// Padding is used to break the 16 byte alignment of pages in a way that after a marker offset is added to the block, the object pointer will be correctly aligned
	char padding[16 - sizeof(markerType)];

	Block		page[countInBlock];
	LargeBlock	*next;
};

template<int elemSize, int countInBlock>
class ObjectBlockPool
{
	typedef SmallBlock<elemSize> MySmallBlock;
	typedef LargeBlock<elemSize, countInBlock> MyLargeBlock;
public:
	ObjectBlockPool()
	{
		freeBlocks = &lastBlock;
		activePages = NULL;
		lastNum = countInBlock;
	}
	~ObjectBlockPool()
	{
		if(!activePages)
			return;
		do
		{
			MyLargeBlock* following = activePages->next;
			NULLC::alignedDealloc(activePages);
			activePages = following;
		}while(activePages != NULL);
		freeBlocks = &lastBlock;
		activePages = NULL;
		lastNum = countInBlock;
		sortedPages.reset();
	}

	void* Alloc()
	{
		MySmallBlock*	result;
		if(freeBlocks && freeBlocks != &lastBlock)
		{
			result = freeBlocks;
			freeBlocks = freeBlocks->next;
		}else{
			if(lastNum == countInBlock)
			{
				MyLargeBlock* newPage = (MyLargeBlock*)NULLC::alignedAlloc(sizeof(MyLargeBlock));
				//memset(newPage, 0, sizeof(MyLargeBlock));
				newPage->next = activePages;
				activePages = newPage;
				lastNum = 0;
				sortedPages.push_back(newPage);
				int index = sortedPages.size() - 1;
				while(index > 0 && sortedPages[index] < sortedPages[index - 1])
				{
					MyLargeBlock *tmp = sortedPages[index];
					sortedPages[index] = sortedPages[index - 1];
					sortedPages[index - 1] = tmp;
					index--;
				}
			}
			result = &activePages->page[lastNum++];
		}
		return result;
	}

	void Free(void* ptr)
	{
		if(!ptr)
			return;
		MySmallBlock* freedBlock = static_cast<MySmallBlock*>(static_cast<void*>(ptr));
		freedBlock->next = freeBlocks;
		freeBlocks = freedBlock;
	}
	bool IsBasePointer(void* ptr)
	{
		MyLargeBlock *curr = activePages;
		while(curr)
		{
			if((char*)ptr >= (char*)curr->page && (char*)ptr <= (char*)curr->page + sizeof(MyLargeBlock))
			{
				if(((unsigned int)(intptr_t)((char*)ptr - (char*)curr->page) & (elemSize - 1)) == 4)
					return true;
			}
			curr = curr->next;
		}
		return false;
	}
	void* GetBasePointer(void* ptr)
	{
		if(!sortedPages.size() || ptr < sortedPages[0] || ptr > (char*)sortedPages.back() + sizeof(MyLargeBlock))
			return NULL;
		// Binary search
		unsigned int lowerBound = 0;
		unsigned int upperBound = sortedPages.size() - 1;
		unsigned int pointer = 0;
		while(upperBound - lowerBound > 1)
		{
			pointer = (lowerBound + upperBound) >> 1;
			if(ptr < sortedPages[pointer])
				upperBound = pointer;
			if(ptr > sortedPages[pointer])
				lowerBound = pointer;
		}
		if(ptr < sortedPages[pointer])
			pointer--;
		if(ptr > (char*)sortedPages[pointer]  + sizeof(MyLargeBlock))
			pointer++;
		MyLargeBlock *best = sortedPages[pointer];

		if(ptr < best || ptr > (char*)best + sizeof(MyLargeBlock))
			return NULL;
		unsigned int fromBase = (unsigned int)(intptr_t)((char*)ptr - (char*)best->page);
		return (char*)best->page + (fromBase & ~(elemSize - 1)) + 4;
	}
	void Mark(unsigned int number)
	{
		__assert(number < 128);
		MyLargeBlock *curr = activePages;
		while(curr)
		{
			for(unsigned int i = 0; i < (curr == activePages ? lastNum : countInBlock); i++)
			{
				if((curr->page[i].marker & 0xff) < 128)
					curr->page[i].marker = (curr->page[i].marker & ~0xff) | number;
			}
			curr = curr->next;
		}
	}
	unsigned int FreeMarked(unsigned int number)
	{
		unsigned int freed = 0;
		MyLargeBlock *curr = activePages;
		while(curr)
		{
			for(unsigned int i = 0; i < (curr == activePages ? lastNum : countInBlock); i++)
			{
				if((curr->page[i].marker & 0xff) == number)
				{
					Free(&curr->page[i]);
					freed++;
				}
			}
			curr = curr->next;
		}
		return freed;
	}

	MySmallBlock	lastBlock;

	MySmallBlock	*freeBlocks;
	MyLargeBlock	*activePages;
	unsigned int	lastNum;

	FastVector<MyLargeBlock*>	sortedPages;
};

namespace NULLC
{
	const unsigned int poolBlockSize = 64 * 1024;

	unsigned int usedMemory = 0;

	unsigned int collectableMinimum = 1024 * 1024;
	unsigned int globalMemoryLimit = 1024 * 1024 * 1024;

	ObjectBlockPool<8, poolBlockSize / 8>		pool8;
	ObjectBlockPool<16, poolBlockSize / 16>		pool16;
	ObjectBlockPool<32, poolBlockSize / 32>		pool32;
	ObjectBlockPool<64, poolBlockSize / 64>		pool64;
	ObjectBlockPool<128, poolBlockSize / 128>	pool128;
	ObjectBlockPool<256, poolBlockSize / 256>	pool256;
	ObjectBlockPool<512, poolBlockSize / 512>	pool512;

	FastVector<void*>				globalObjects;

	double	markTime = 0.0;
	double	collectTime = 0.0;
}

void* NULLC::AllocObject(int size, unsigned typeID)
{
	if(size < 0)
	{
		nullcThrowError("Requested memory size is less than zero.");
		return NULL;
	}
	void *data = NULL;
	size += 4;

	if((unsigned int)(usedMemory + size) > globalMemoryLimit)
	{
		CollectMemory();
		if((unsigned int)(usedMemory + size) > globalMemoryLimit)
		{
			nullcThrowError("Reached global memory maximum");
			return NULL;
		}
	}else if((unsigned int)(usedMemory + size) > collectableMinimum){
		CollectMemory();
	}
	unsigned int realSize = size;
	if(size <= 64)
	{
		if(size <= 16)
		{
			if(size <= 8)
			{
				data = pool8.Alloc();
				realSize = 8;
			}else{
				data = pool16.Alloc();
				realSize = 16;
			}
		}else{
			if(size <= 32)
			{
				data = pool32.Alloc();
				realSize = 32;
			}else{
				data = pool64.Alloc();
				realSize = 64;
			}
		}
	}else{
		if(size <= 256)
		{
			if(size <= 128)
			{
				data = pool128.Alloc();
				realSize = 128;
			}else{
				data = pool256.Alloc();
				realSize = 256;
			}
		}else{
			if(size <= 512)
			{
				data = pool512.Alloc();
				realSize = 512;
			}else{
				globalObjects.push_back(NULLC::alignedAlloc(size - sizeof(markerType), 4 + sizeof(markerType)));
				if(globalObjects.back() == NULL)
				{
					nullcThrowError("Allocation failed.");
					return NULL;
				}
				realSize = *(int*)globalObjects.back() = size;
				data = (char*)globalObjects.back() + 4;
			}
		}
	}
	usedMemory += realSize;

	if(data == NULL)
	{
		nullcThrowError("Allocation failed.");
		return NULL;
	}

	memset(data, 0, size);
	*(int*)data = typeID << 8;
	return (char*)data + 4;
}

unsigned int NULLC::UsedMemory()
{
	return usedMemory;
}

NULLCArray<int> NULLC::AllocArray(int size, int count, unsigned typeID)
{
	NULLCArray<int> ret;
	ret.ptr = (char*)AllocObject(count * size, typeID);
	ret.size = count;
	return ret;
}

void NULLC::MarkMemory(unsigned int number)
{
	for(unsigned int i = 0; i < globalObjects.size(); i++)
		((unsigned int*)globalObjects[i])[1] = number;
	pool8.Mark(number);
	pool16.Mark(number);
	pool32.Mark(number);
	pool64.Mark(number);
	pool128.Mark(number);
	pool256.Mark(number);
	pool512.Mark(number);
}

bool NULLC::IsBasePointer(void* ptr)
{
	// Search in range of every pool
	if(pool8.IsBasePointer(ptr))
		return true;
	if(pool16.IsBasePointer(ptr))
		return true;
	if(pool32.IsBasePointer(ptr))
		return true;
	if(pool64.IsBasePointer(ptr))
		return true;
	if(pool128.IsBasePointer(ptr))
		return true;
	if(pool256.IsBasePointer(ptr))
		return true;
	if(pool512.IsBasePointer(ptr))
		return true;
	// Search in global pool
	for(unsigned int i = 0; i < globalObjects.size(); i++)
	{
		if((char*)ptr - 8 == globalObjects[i])
			return true;
	}
	return false;
}

void* NULLC::GetBasePointer(void* ptr)
{
	// Search in range of every pool
	if(void *base = pool8.GetBasePointer(ptr))
		return base;
	if(void *base = pool16.GetBasePointer(ptr))
		return base;
	if(void *base = pool32.GetBasePointer(ptr))
		return base;
	if(void *base = pool64.GetBasePointer(ptr))
		return base;
	if(void *base = pool128.GetBasePointer(ptr))
		return base;
	if(void *base = pool256.GetBasePointer(ptr))
		return base;
	if(void *base = pool512.GetBasePointer(ptr))
		return base;
	// Search in global pool
	for(unsigned int i = 0; i < globalObjects.size(); i++)
	{
		if(ptr >= globalObjects[i] && ptr <= (char*)globalObjects[i] + *(unsigned int*)globalObjects[i])
			return (char*)globalObjects[i] + 8;
	}
	return NULL;
}

void NULLC::CollectMemory()
{
	GC_DEBUG_PRINT("%d used memory (%d collectable cap, %d max cap)\r\n", usedMemory, collectableMinimum, globalMemoryLimit);

	double time = (double(clock()) / CLOCKS_PER_SEC);

	GC::unmanageableBase = (char*)&time;

	// All memory blocks are marked with 0
	MarkMemory(0);
	// Used memory blocks are marked with 1
	MarkUsedBlocks();

	markTime += (double(clock()) / CLOCKS_PER_SEC) - time;
	time = (double(clock()) / CLOCKS_PER_SEC);

	// Globally allocated objects marked with 0 are deleted
	unsigned int unusedBlocks = 0;
	for(unsigned int i = 0; i < globalObjects.size(); i++)
	{
		if(((unsigned int*)globalObjects[i])[1] == 0)
		{
			usedMemory -= *(unsigned int*)globalObjects[i];
			NULLC::alignedDealloc(globalObjects[i]);
			globalObjects[i] = globalObjects.back();
			globalObjects.pop_back();
			unusedBlocks++;
		}
	}
//	printf("%d unused globally allocated blocks destroyed (%d remains)\r\n", unusedBlocks, globalObjects.size());

//	printf("%d used memory\r\n", usedMemory);

	// Objects allocated from pools are freed
	unusedBlocks = pool8.FreeMarked(0);
	usedMemory -= unusedBlocks * 8;
//	printf("%d unused pool blocks freed (8 bytes)\r\n", unusedBlocks);
	unusedBlocks = pool16.FreeMarked(0);
	usedMemory -= unusedBlocks * 16;
//	printf("%d unused pool blocks freed (16 bytes)\r\n", unusedBlocks);
	unusedBlocks = pool32.FreeMarked(0);
	usedMemory -= unusedBlocks * 32;
//	printf("%d unused pool blocks freed (32 bytes)\r\n", unusedBlocks);
	unusedBlocks = pool64.FreeMarked(0);
	usedMemory -= unusedBlocks * 64;
//	printf("%d unused pool blocks freed (64 bytes)\r\n", unusedBlocks);
	unusedBlocks = pool128.FreeMarked(0);
	usedMemory -= unusedBlocks * 128;
//	printf("%d unused pool blocks freed (128 bytes)\r\n", unusedBlocks);
	unusedBlocks = pool256.FreeMarked(0);
	usedMemory -= unusedBlocks * 256;
//	printf("%d unused pool blocks freed (256 bytes)\r\n", unusedBlocks);
	unusedBlocks = pool512.FreeMarked(0);
	usedMemory -= unusedBlocks * 512;
//	printf("%d unused pool blocks freed (512 bytes)\r\n", unusedBlocks);

	GC_DEBUG_PRINT("%d used memory\r\n", usedMemory);

	collectTime += (double(clock()) / CLOCKS_PER_SEC) - time;

	if(usedMemory + (usedMemory >> 1) >= collectableMinimum)
		collectableMinimum <<= 1;
}

double NULLC::MarkTime()
{
	return markTime;
}

double NULLC::CollectTime()
{
	return collectTime;
}

int	__nullcOutputResultInt(int x)
{
	printf("%d", x);
	return 0;
}

int	__nullcOutputResultLong(long long x)
{
	printf("%lld", x);
	return 0;
}

int	__nullcOutputResultDouble(double x)
{
	printf("%f", x);
	return 0;
}

NULLCArray<int> __vtbl3761170085finalize;
void __nullcInitBaseModule()
{
	__vtbl3761170085finalize = NULLC::AllocArray(4, 1024, 4);
}

