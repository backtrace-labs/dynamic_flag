/*
 * Copyright 2018 AppNexus, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "an_hook.h"

#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/* dummy stubs */
extern int an_hook_dummy(const char *regex);
extern void an_hook_init_lib_dummy(void);

#if AN_HOOK_ENABLED

struct patch_record {
	void *hook;
	void *destination;
	const char *name;
	uint8_t initial_opcode; /* initial value for fallback impl. */
	uint8_t flipped;
	uint8_t padding[6];
};

struct patch_count {
	/* If a hook is unhook, do not increment its activation count. */
	uint64_t activation;
	uint64_t unhook;
};


struct patch_list {
	size_t size;
	size_t capacity;
	const struct patch_record *data[];
};

static pthread_mutex_t patch_lock = PTHREAD_MUTEX_INITIALIZER;

/* Hook records are in an array. For each hook record, count # of activations and disable calls. */
static struct {
	struct patch_count *data;
	size_t size;
} counts = { NULL };

extern const struct patch_record __start_dynamic_flag_list[], __stop_dynamic_flag_list[];

static void init_all(void);

static struct patch_list *
patch_list_create(void)
{
	struct patch_list *list;

	assert(counts.size > 0  && "Must initialize an_hook first");
	list = calloc(1, sizeof(*list) + counts.size * sizeof(list->data[0]));
	assert(list != NULL);
	list->size = 0;
	list->capacity = counts.size;
	return list;
}

static void
patch_list_destroy(struct patch_list *list)
{

	if (list == NULL) {
		return;
	}

	free(list);
	return;
}

static void
patch_list_push(struct patch_list *list, const struct patch_record *record)
{

	assert(list->size < list->capacity);
	list->data[list->size++] = record;
	return;
}

static void
lock(void)
{
	int mutex_ret;

	mutex_ret = pthread_mutex_lock(&patch_lock);
	assert(mutex_ret == 0);

	if (__builtin_expect((counts.data == NULL), 0)) {
		size_t n = __stop_dynamic_flag_list - __start_dynamic_flag_list;

		counts.data = calloc(n, sizeof(*counts.data));
		counts.size = n;
		assert(counts.data != NULL);
		init_all();
	}

	return;
}

static void
unlock(void)
{
	int mutex_ret;

	mutex_ret = pthread_mutex_unlock(&patch_lock);
	assert(mutex_ret == 0);

	return;
}

/* Make sure there's at least one hook point. */
__attribute__((__used__)) static void
dummy(void)
{

	AN_HOOK_DUMMY(none);
}

#if !AN_HOOK_FALLBACK
#define HOOK_SIZE 5

static void
patch(const struct patch_record *record)
{
	uint8_t *address = record->hook;
	void *dst = record->destination;
	int32_t *target = (int32_t *)(address + 1);
	intptr_t offset = (uint8_t *)dst - (address + 5); /* IP offset from end of instruction. */

	assert((*address == 0xe9 || *address == 0xa9) &&
	    "Target should be a jmp rel or a testl $..., %eax");
	assert((offset == (intptr_t)*target) &&
	    "Target's offset should match with the hook destination.");

	*address = 0xe9; /* jmp rel */
	return;
}

static void
unpatch(const struct patch_record *record)
{
	uint8_t *address = record->hook;
	void *dst = record->destination;
	int32_t *target = (int32_t *)(address + 1);
	intptr_t offset = (uint8_t *)dst - (address + 5);

	assert((*address == 0xe9 || *address == 0xa9) &&
	    "Target should be a jmp rel or a testl $..., %eax");
	assert((offset == (intptr_t)*target) &&
	    "Target's offset should match with the hook destination.");

	*address = 0xa9; /* testl $..., %eax */
	return;
}

#else
#define HOOK_SIZE 3 /* REX byte + mov imm8 */

static void
patch(const struct patch_record *record)
{
	uint8_t *address = record->hook;
	/* +1 to get the immediate field after the MOV opcode. */
	uint8_t *field = address + 1;

	/*
	 * F4 is HLT, 0x0 is ADD [AL], AL.  Neither is a MOV opcode.
	 * Any mismatch must mean we have a 3-byte REX MOV, and we
	 * have to go forward one more byte.
	 */
	if (field[0] != DYNAMIC_FLAG_VALUE_ACTIVE &&
	    field[0] != DYNAMIC_FLAG_VALUE_INACTIVE) {
		field++;
	}

	assert((field[0] == DYNAMIC_FLAG_VALUE_ACTIVE) ||
	    (field[0] == DYNAMIC_FLAG_VALUE_INACTIVE));
	field[0] = DYNAMIC_FLAG_VALUE_ACTIVE;
	return;
}

static void
unpatch(const struct patch_record *record)
{
	uint8_t *address = record->hook;
	uint8_t *field = address + 1;

	if (field[0] != DYNAMIC_FLAG_VALUE_ACTIVE &&
	    field[0] != DYNAMIC_FLAG_VALUE_INACTIVE) {
		field++;
	}

	assert((field[0] == DYNAMIC_FLAG_VALUE_ACTIVE) ||
	    (field[0] == DYNAMIC_FLAG_VALUE_INACTIVE));
	field[0] = DYNAMIC_FLAG_VALUE_INACTIVE;
	return;
}
#endif

static void
default_patch(const struct patch_record *record)
{
	size_t i;

	i = record - __start_dynamic_flag_list;

	assert(i < counts.size && "Hook out of bounds?!");

	switch (record->initial_opcode) {
	case DYNAMIC_FLAG_VALUE_ACTIVE:
		counts.data[i].activation = (record->flipped != 0) ? 0 : 1;
		patch(record);
		break;
	case DYNAMIC_FLAG_VALUE_INACTIVE:
		counts.data[i].activation = (record->flipped != 0) ? 1 : 0;
		unpatch(record);
		break;
	default:
		assert((record->initial_opcode == DYNAMIC_FLAG_VALUE_ACTIVE ||
		    record->initial_opcode == DYNAMIC_FLAG_VALUE_INACTIVE) &&
		    "Initial opcode/value must be ACTIVE or INACTIVE (JMP REL32 or TEST / 0xF4 or 0)");
	}

	__builtin___clear_cache(record->hook, (char *)record->hook + HOOK_SIZE);
	return;
}

static void
activate(const struct patch_record *record)
{

	if (record->flipped != 0) {
		unpatch(record);
	} else {
		patch(record);
	}

	__builtin___clear_cache(record->hook, (char *)record->hook + HOOK_SIZE);
	return;
}

static void
deactivate(const struct patch_record *record)
{

	if (record->flipped != 0) {
		patch(record);
	} else {
		unpatch(record);
	}

	__builtin___clear_cache(record->hook, (char *)record->hook + HOOK_SIZE);
	return;
}

static void
amortize(const struct patch_list *records,
    void (*cb)(const struct patch_record *))
{
	uintptr_t first_page = UINTPTR_MAX;
	uintptr_t last_page = 0;
	uintptr_t page_size;
	size_t i, section_begin = 0;

	page_size = sysconf(_SC_PAGESIZE);

#define PATCH() do {							\
		if (section_begin < i) {				\
			mprotect((void *)(first_page * page_size),	\
			    (1 + last_page - first_page) * page_size,	\
			    PROT_READ | PROT_WRITE | PROT_EXEC);	\
			for (size_t j = section_begin; j < i; j++) {	\
				cb(records->data[j]);				\
			}						\
									\
			mprotect((void *)(first_page * page_size),	\
			    (1 + last_page - first_page) * page_size,	\
			    PROT_READ | PROT_EXEC);			\
		}							\
	} while (0)

	for (i = 0; i < records->size; i++) {
		const struct patch_record *record = records->data[i];
		uintptr_t begin_page = (uintptr_t)record->hook / page_size;
		uintptr_t end_page = ((uintptr_t)record->hook + HOOK_SIZE - 1) / page_size;

		if ((first_page - 1) <= begin_page ||
		    end_page <= (last_page + 1)) {
			if (begin_page < first_page) {
				first_page = begin_page;
			}
			if (end_page > last_page) {
				last_page = end_page;
			}
		} else {
			PATCH();
			section_begin = i;
			first_page = begin_page;
			last_page = end_page;
		}
	}

	PATCH();
	return;
}

static int
find_records(const char *pattern, struct patch_list *acc)
{
	regex_t regex;
	size_t n = __stop_dynamic_flag_list - __start_dynamic_flag_list;

	if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
		return -1;
	}

	for (size_t i = 0; i < n; i++) {
		const struct patch_record *record = __start_dynamic_flag_list + i;

		if (regexec(&regex, record->name, 0, NULL, 0) != REG_NOMATCH) {
			patch_list_push(acc, record);
		}
	}

	regfree(&regex);
	return 0;
}

static int
find_records_kind(const void **start, const void **end, const char *pattern,
    struct patch_list *acc)
{
	regex_t regex;
	size_t n = end - start;

	if (pattern != NULL && regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
		return -1;
	}

	for (size_t i = 0; i < n; i++) {
		const struct patch_record *record = start[i];

		if (pattern == NULL ||
		    regexec(&regex, record->name, 0, NULL, 0) != REG_NOMATCH) {
			patch_list_push(acc, record);
		}
	}

	if (pattern != NULL) {
		regfree(&regex);
	}

	return 0;
}

static int
cmp_patches(const void *x, const void *y)
{
	const struct patch_record * const *a = x;
	const struct patch_record * const *b = y;

	if ((*a)->hook == (*b)->hook) {
		return 0;
	}

	return ((*a)->hook < (*b)->hook) ? -1 : 1;
}

static void
init_all(void)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records("", acc);
	assert(r == 0 && "an_hook init failed.");

	qsort(acc->data, acc->size, sizeof(acc->data[0]), cmp_patches);
	amortize(acc, default_patch);
	patch_list_destroy(acc);
	return;
}

static size_t
activate_all(struct patch_list *arr)
{
	struct patch_list *to_patch;
	size_t to_patch_count;

	qsort(arr->data, arr->size, sizeof(struct patch_record *), cmp_patches);

	to_patch = patch_list_create();
	lock();
	for (size_t i = 0; i < arr->size; i++) {
		const struct patch_record *record = arr->data[i];
		size_t offset = record - __start_dynamic_flag_list;

		if (counts.data[offset].unhook > 0) {
			continue;
		}

		if (counts.data[offset].activation++ == 0) {
			patch_list_push(to_patch, record);
		}
	}

	amortize(to_patch, activate);
	unlock();

	to_patch_count = to_patch->size;
	patch_list_destroy(to_patch);
	return to_patch_count;
}

static size_t
deactivate_all(struct patch_list *records)
{
	struct patch_list *to_patch;
	size_t to_patch_count;

	qsort(records->data, records->size,
	    sizeof(struct patch_record *), cmp_patches);

	to_patch = patch_list_create();
	lock();
	for (size_t i = 0; i < records->size; i++) {
		const struct patch_record *record = records->data[i];
		size_t offset = record - __start_dynamic_flag_list;

		if (counts.data[offset].activation > 0 &&
		    --counts.data[offset].activation == 0) {
			patch_list_push(to_patch, record);
		}
	}

	amortize(to_patch, deactivate);
	unlock();

	to_patch_count = to_patch->size;
	patch_list_destroy(to_patch);
	return to_patch_count;
}

static size_t
rehook_all(struct patch_list *records)
{

	lock();
	for (size_t i = 0; i < records->size; i++) {
		const struct patch_record *record = records->data[i];
		size_t offset = record - __start_dynamic_flag_list;

		if (counts.data[offset].unhook > 0) {
			counts.data[offset].unhook--;
		}
	}

	unlock();

	return records->size;
}

static size_t
unhook_all(struct patch_list *records)
{

	lock();
	for (size_t i = 0; i < records->size; i++) {
		const struct patch_record *record = records->data[i];
		size_t offset = record - __start_dynamic_flag_list;

		counts.data[offset].unhook++;
	}

	unlock();

	return records->size;
}

int
an_hook_activate(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	activate_all(acc);

out:
	patch_list_destroy(acc);
	return r;
}

int
an_hook_deactivate(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	deactivate_all(acc);

out:
	patch_list_destroy(acc);
	return r;
}

int
an_hook_unhook(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	unhook_all(acc);

out:
	patch_list_destroy(acc);
	return r;
}

int
an_hook_rehook(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	rehook_all(acc);

out:
	patch_list_destroy(acc);
	return r;
}

int
an_hook_activate_kind_inner(const void **start, const void **end, const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records_kind(start, end, regex, acc);
	if (r != 0) {
		goto out;
	}

	activate_all(acc);

out:
	patch_list_destroy(acc);
	return r;
}

int
an_hook_deactivate_kind_inner(const void **start, const void **end, const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records_kind(start, end, regex, acc);
	if (r != 0) {
		goto out;
	}

	deactivate_all(acc);

out:
	patch_list_destroy(acc);
	return r;
}

void
an_hook_init_lib(void)
{

	lock();
	unlock();
	return;
}

#endif /* AN_HOOK_ENABLED */
