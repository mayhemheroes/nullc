#pragma once
#ifndef NULLC_ALLOCATOR_H
#define NULLC_ALLOCATOR_H

#include "stdafx.h"
#include "Array.h"
#include "Pool.h"

struct Allocator
{
	virtual void* alloc(int size)
	{
		return NULLC::alloc(size);
	}

	virtual void dealloc(void* ptr)
	{
		NULLC::dealloc(ptr);
	}

	template<typename T>
	T* construct()
	{
		return new(alloc(sizeof(T))) T();
	}
	template<typename T>
	T* construct(int count)
	{
		void *tmp = (void*)alloc(4 + count * sizeof(T));
		new(tmp) T[count];
		return (T*)tmp;
	}

	template<typename T>
	void destruct(T* ptr)
	{
		if(!ptr)
			return;
		ptr->~T();
		dealloc((void*)ptr);
	}
	template<typename T>
	void destruct(T* ptr, size_t count)
	{
		if(!ptr)
			return;
		for(size_t i = 0; i < count; i++)
			ptr[i].~T();
		dealloc(ptr);
	}
};

template<typename T, unsigned fallbackSize>
struct GrowingAllocatorRef: Allocator
{
	GrowingAllocatorRef(T &pool): pool(pool)
	{
	}

	~GrowingAllocatorRef()
	{
		Reset();
	}

	virtual void* alloc(int size)
	{
		if(unsigned(size) > fallbackSize)
		{
			void *result = NULLC::alloc(size);
			largeObjects.push_back(result);
			return result;
		}

		return pool.Allocate(size);
	}

	virtual void dealloc(void* ptr)
	{
		(void)ptr;
	}

	void Clear()
	{
		pool.Clear();

		for(unsigned i = 0; i < largeObjects.count; i++)
			NULLC::dealloc(largeObjects.data[i]);
		largeObjects.count = 0;
	}

	void Reset()
	{
		pool.Reset();

		for(unsigned i = 0; i < largeObjects.count; i++)
			NULLC::dealloc(largeObjects.data[i]);
		largeObjects.reset();
	}

	T &pool;

	class Vector
	{
	public:
		Vector()
		{
			data = 0;
			max = 0;
			count = 0;
		}

		~Vector()
		{
			NULLC::destruct(data, max);
		}

		void reset()
		{
			NULLC::destruct(data, max);

			data = 0;
			max = 0;
			count = 0;
		}

		void push_back(void* val)
		{
			if(count == max)
				grow(count);

			data[count++] = val;
		}

		void grow(unsigned newSize)
		{
			if(max + (max >> 1) > newSize)
				newSize = max + (max >> 1);
			else
				newSize += 32;

			void **newData;

			newData = NULLC::construct<void*>(newSize);

			if(data)
			{
				memcpy(newData, data, max * sizeof(void*));

				NULLC::destruct(data, max);
			}

			data = newData;
			max = newSize;
		}

		void **data;
		unsigned max, count;
	};

	Vector largeObjects;

private:
	// Disable assignment and copy constructor
	void operator =(GrowingAllocatorRef &r);
	GrowingAllocatorRef(GrowingAllocatorRef &r);
};

#endif
