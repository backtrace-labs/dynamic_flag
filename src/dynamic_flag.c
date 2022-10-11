/*
 * Copyright 2021 Backtrace I/O, Inc.
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

#include "dynamic_flag.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/* dummy stubs */
extern int dynamic_flag_dummy(const char *regex);
extern void dynamic_flag_init_lib_dummy(void);
extern long long dynamic_flag_list_state_dummy(const char *regex,
    long long (*cb)(void *ctx, const struct dynamic_flag_state *), void *ctx);

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE > 0

/**
 * Patch records are defined by inline assembly blocks in the
 * `dynamic_flag_list` section.
 *
 * There is also a pointer to each record in the kind-specific
 * `dynamic_flag_${KIND}_list` section.
 *
 * When a function with flags is inlined, each instantiation will have
 * its own record.
 */
struct patch_record {
	/*
	 * Address of the instruction to patch.  For the asm goto
	 * implementation (IMPLEMENTATION_STYLE == 2), that's also the
	 * address of the opcode byte.  For the fallback
	 * implementation, this could be a REX byte, and the immediate
	 * byte is at the end of the instruction, so we have to scan
	 * forward by one or two bytes.
	 */
	void *hook;

	/*
	 * The destination instruction when the hook instruction is a
	 * `jmp` (for the asm goto implementation style), 0 otherwise.
	 */
	void *destination;

	/*
	 * The flag name as a C string, followed by a docstring; a
	 * missing docstring is represented as an empty C string
	 * (i.e., there's always a second NUL terminator).
	 */
	const char *name_doc;

	/*
	 * The value to patch at the hook when the library is
	 * initialised.  This functionality is used for `DF_OPT`
	 * flags, where we want the code enabled unconditionally if
	 * the dynamic flag library can't reach the flags, and
	 * otherwise disabled until explicitly turned enabled.
	 */
	uint8_t initial_opcode; /* initial value for fallback impl. */

	/* 
	 * Flipped records should enter the slow path (jmp to the
	 * destination / set a non-zero immediate value) when their
	 * flag is disabled, and stay on the fast path when it is
	 * enabled.
	 */
	uint8_t flipped;
	uint8_t padding[6];
} __attribute__((__packed__));

/**
 * Internal metadata for each patch record: current activation and
 * unhook count.
 *
 * If `activation > 0`, the flag is enabled.  If `unhook > 0`, the
 * flag is unhooked and `activation` should not be incremented.
 */
struct patch_count {
	/* If a hook is unhook, do not increment its activation count. */
	uint64_t activation;
	uint64_t unhook;
};

/**
 * We store pointers to patch records that match a given criterion in
 * these patch lists.  There can never be more pointers than the total
 * number of records.
 */
struct patch_list {
	size_t size;
	size_t capacity;
	const struct patch_record *data[];
};

/**
 * The patch_lock protects write access to the `patch_count` data and
 * to the machine code itself.
 */
static pthread_mutex_t patch_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Hook records are in an array. For each hook record, count # of
 * activations and disable calls.
 *
 * This array is initialized on demand in `lock()`.
 */
static struct {
	struct patch_count *data;
	size_t size;
} counts = { NULL };

extern const struct patch_record __start_dynamic_flag_list[], __stop_dynamic_flag_list[];

static void init_all(void);

/**
 * Returns a new patch list that's correctly sized for the patch
 * records the library is aware of.
 */
static struct patch_list *
patch_list_create(void)
{
	struct patch_list *list;

	assert(counts.size > 0 && "Must initialize dynamic_flag first");
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

/**
 * Acquires the patch lock.  Initialises the `patch_count` array if
 * necessary and the initial hook states.
 */
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

	DYNAMIC_FLAG_DUMMY(none,
	    "This dummy flag does nothing. It lets the dynamic_flag library "
	    "compile even when no other flag is defined.");
	return;
}

#if DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 1
#define HOOK_SIZE 3 /* REX byte + mov imm8 */

/**
 * Switches to the flag's slow path by updating a non-zero value in
 * the `MOV` instruction's immediate field.
 */
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

/**
 * Switches to the flag's fast path by updating a zero value in the
 * `MOV` instruction's immediate field.
 */
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
#elif DYNAMIC_FLAG_IMPLEMENTATION_STYLE == 2
#define HOOK_SIZE 5  /* jmp rel32 or testl %eax, imm. */

/**
 * Switches to the flag's slow path by setting the opcode to `jmp rel`.
 */
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

/**
 * Switches to the flag's fast path by setting the opcode to `test`.
 */
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
#endif

/**
 * Sets the flag's initial state to that configured in its patch
 * record, and updates the patch's activation count accordingly.
 */
static void
initial_patch(const struct patch_record *record)
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

/**
 * Enables a flag.  I.e., switches to the fast path if the record is
 * `flipped`, and to the slow path otherwise.
 */
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

/**
 * Disables a flag.  I.e., switches to the slow path if the record is
 * `flipped`, and to the fast path otherwise.
 */
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

/**
 * Invokes `cb` on a list of records sorted by hook instruction
 * address.
 *
 * The hook instruction is on writable page(s) when `cb` is called,
 * and quickly reset to read-only/executable.
 *
 * This pair of mprotect is slow, so `amortize` batches calls for
 * contiguous pages.
 */
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
		/* The initial range is empty, and can always be extended. */
		bool empty_range = first_page > last_page;
		/*
		 * Otherwise, we can extend the current range by up to
		 * one page in either direction.  If begin/end page
		 * fall in that extended range, we want to adjoin it
		 * to the `[first_page, last_page]` range.  Limited
		 * extension guarantees that we only mprotect adjacent
		 * pages that we would have mprotect-ed anyway.
		 *
		 * There is potential for unsigned overflow in the
		 * subtraction, but only for the zero page, which is
		 * usually unmappable. If we do have hook instructions
		 * on the zero page, the wraparound is defined
		 * behaviour and will merely result in less
		 * amortisation, which is still correct.
		 */
		bool can_extend = (first_page - 1) <= begin_page &&
			end_page <= (last_page + 1);

		if (empty_range || can_extend) {
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

/**
 * Compiles `pattern` as a POSIX extended regular expression that's
 * implicitly anchored at the first character of the string (at the
 * first character of the flag name)
 */
static int
compile_regex(regex_t *regex, const char *pattern)
{
	char *to_free = NULL;
	int r;

	if (pattern != NULL && pattern[0] != '^') {
		r = asprintf(&to_free, "^%s", pattern);
		if (r < 0)
			return r;
		pattern = to_free;
	}

	r = regcomp(regex, pattern, REG_EXTENDED | REG_NOSUB);
	free(to_free);
	return r;
}

/**
 * Stores patch records that match `pattern` in `acc`.
 */
static int
find_records(const char *pattern, struct patch_list *acc)
{
	regex_t regex;
	size_t n = __stop_dynamic_flag_list - __start_dynamic_flag_list;

	if (compile_regex(&regex, pattern) != 0) {
		return -1;
	}

	for (size_t i = 0; i < n; i++) {
		const struct patch_record *record = __start_dynamic_flag_list + i;

		if (regexec(&regex, record->name_doc, 0, NULL, 0) != REG_NOMATCH) {
			patch_list_push(acc, record);
		}
	}

	regfree(&regex);
	return 0;
}

/**
 * Stores patch records in the
 * `__{start,stop}_dynamic_flag_${KIND}_list` array that match
 * `pattern` in `acc`.
 */
static int
find_records_kind(const void **start, const void **end, const char *pattern,
    struct patch_list *acc)
{
	regex_t regex;
	size_t n = end - start;

	if (pattern != NULL && compile_regex(&regex, pattern) != 0) {
		return -1;
	}

	for (size_t i = 0; i < n; i++) {
		const struct patch_record *record = start[i];

		if (pattern == NULL ||
		    regexec(&regex, record->name_doc, 0, NULL, 0) != REG_NOMATCH) {
			patch_list_push(acc, record);
		}
	}

	if (pattern != NULL) {
		regfree(&regex);
	}

	return 0;
}

/**
 * Compares `patch_record`s by `hook` address.
 */
static int
cmp_patches(const void *x, const void *y)
{
	const struct patch_record *const *a = x;
	const struct patch_record *const *b = y;

	if ((*a)->hook == (*b)->hook) {
		return 0;
	}

	return ((*a)->hook < (*b)->hook) ? -1 : 1;
}

/**
 * Initializes the flags' states.
 */
static void
init_all(void)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records("", acc);
	assert(r == 0 && "dynamic_flag init failed.");

	qsort(acc->data, acc->size, sizeof(acc->data[0]), cmp_patches);
	amortize(acc, initial_patch);
	patch_list_destroy(acc);
	return;
}

/**
 * Increments by one the activation count of all flags in `records`.
 */
static size_t
activate_all(struct patch_list *records)
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

/**
 * Decrements by one the activation count of all flags in `records`.
 */
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

/**
 * Decrements by one the unhook count of all flags in `records`.
 */
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

/**
 * Increments by one the unhook count of all flags in `records`.
 */
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

ssize_t
dynamic_flag_activate(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	activate_all(acc);
	r = acc->size;

out:
	patch_list_destroy(acc);
	return r;
}

ssize_t
dynamic_flag_deactivate(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	deactivate_all(acc);
	r = acc->size;

out:
	patch_list_destroy(acc);
	return r;
}

ssize_t
dynamic_flag_unhook(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	unhook_all(acc);
	r = acc->size;

out:
	patch_list_destroy(acc);
	return r;
}

ssize_t
dynamic_flag_rehook(const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	rehook_all(acc);
	r = acc->size;

out:
	patch_list_destroy(acc);
	return r;
}

/**
 * Compares patch records roughly alphabetically.
 *
 * Compares everything up to the list number (i.e., kind, name, file)
 * with strcmp.
 *
 * If only one flag has a docstring, it compares lower.
 *
 * Otherwise, the flag with the lesser line number comes first.
 */
static int
cmp_patches_alpha(const void *x, const void *y)
{
	const struct patch_record *const *a = x;
	const struct patch_record *const *b = y;
	const char *a_name = (*a)->name_doc;
	const char *b_name = (*b)->name_doc;
	const char *a_colon, *b_colon;
	unsigned long long a_line, b_line;
	ssize_t colon_idx;
	int r;

	a_colon = strrchr(a_name, ':');
	b_colon = strrchr(b_name, ':');
	colon_idx = a_colon - a_name;

	/* If the prefixes definitely don't match, just strcmp. */
	if (a_colon == NULL || b_colon == NULL ||
	    (colon_idx != b_colon - b_name)) {
		return strcmp(a_name, b_name);
	}

	/* Compare everything up to the line number. */
	r = strncmp(a_name, b_name, colon_idx);
	if (r != 0) {
		return r;
	}

	/* Same kind, name, file.  Show longer docstrings first. */
	{
		const char *a_doc;
		const char *b_doc;

		a_doc = a_colon + strlen(a_colon) + 1;
		b_doc = b_colon + strlen(b_colon) + 1;

		/* Only check if one has a docstring. */
		if (a_doc[0] != '\0' || b_doc[0] != '\0') {
			size_t a_doc_len;
			size_t b_doc_len;

			/* Only b has a docstring, it comes first. */
			if (a_doc[0] == '\0') {
				return 1;
			}

			/* Only a has a docstring, it comes first. */
			if (b_doc[0] == '\0') {
				return -1;
			}

			a_doc_len = strlen(a_doc);
			b_doc_len = strlen(b_doc);

			if (a_doc_len != b_doc_len) {
				return (a_doc_len > b_doc_len) ? -1 : 1;
			}
		}
	}

	a_line = strtoull(a_colon + 1, NULL, 10);
	b_line = strtoull(b_colon + 1, NULL, 10);
	if (a_line != b_line) {
		return (a_line < b_line) ? -1 : 1;
	}

	return 0;
}

ssize_t
dynamic_flag_list_state(const char *regex,
    ssize_t (*cb)(void *ctx, const struct dynamic_flag_state *), void *ctx)
{
	struct patch_list *acc;
	ssize_t r;

	acc = patch_list_create();
	r = find_records(regex, acc);
	if (r != 0) {
		goto out;
	}

	qsort(acc->data, acc->size,
	    sizeof(struct patch_record *), cmp_patches_alpha);

	for (size_t i = 0; i < acc->size; i++) {
		struct dynamic_flag_state state;
		const struct patch_record *record = acc->data[i];
		size_t record_idx = record - __start_dynamic_flag_list;

		state = (struct dynamic_flag_state) {
			.name = record->name_doc,
			.doc = record->name_doc + 1 + strlen(record->name_doc),

			/* Yeah, this is racy. */
			.activation = counts.data[record_idx].activation,
			.unhook = counts.data[record_idx].unhook,

			.hook = record->hook,
			.destination = record->destination,
		};

		if (i > 0 &&
		    strcmp(acc->data[i - 1]->name_doc, acc->data[i]->name_doc) == 0)
			state.duplicate = 1;

		r = cb(ctx, &state);
		if (r != 0)
			goto out;
	}

	r = acc->size;

out:
	patch_list_destroy(acc);
	return r;
}

ssize_t
dynamic_flag_list_fprintf_cb(void *ctx, const struct dynamic_flag_state *state)
{
	char activation[32] = "off";
	char unhook[64] = "";
	FILE *stream = ctx ?: stderr;

	if (state->duplicate == true)
		return 0;

	if (state->activation > 0) {
		int r;

		r = snprintf(activation, sizeof(activation), "%" PRIu64 "",
		    state->activation);
		assert((size_t)r < sizeof(activation));
	}

	if (state->unhook > 0) {
		int r;

		r = snprintf(
		    unhook, sizeof(unhook), ", unhook=%" PRIu64 "", state->unhook);
		assert((size_t)r < sizeof(unhook));
	}

	fprintf(stream, "%s (%s%s)%s%s\n", state->name, activation, unhook,
	    (state->doc[0] == '\0') ? "" : ": ", state->doc);
	return 0;
}

ssize_t
dynamic_flag_activate_kind_inner(const void **start, const void **end,
    const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records_kind(start, end, regex, acc);
	if (r != 0) {
		goto out;
	}

	activate_all(acc);
	r = acc->size;

out:
	patch_list_destroy(acc);
	return r;
}

ssize_t
dynamic_flag_deactivate_kind_inner(const void **start, const void **end,
    const char *regex)
{
	struct patch_list *acc;
	int r;

	acc = patch_list_create();
	r = find_records_kind(start, end, regex, acc);
	if (r != 0) {
		goto out;
	}

	deactivate_all(acc);
	r = acc->size;

out:
	patch_list_destroy(acc);
	return r;
}

void
dynamic_flag_init_lib(void)
{

	lock();
	unlock();
	return;
}
#endif /* DYNAMIC_FLAG_IMPLEMENTATION_STYLE > 0 */
