/*
 *    Stack-less Just-In-Time compiler
 *    Copyright (c) Zoltan Herczeg
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include "sljitLir.h"
#include "regexJIT.h"

// printf'ing the engine operations
#define REGEX_VERBOSE

#define MATCH_BEGIN	0x1
#define MATCH_END	0x2
#define ID_CHECK	0x4

// ---------------------------------------------------------------------
//  Structures for JIT-ed pattern matching
// ---------------------------------------------------------------------

struct regex_machine
{
	sljit_w no_states;
	sljit_w states_len;

	void *continue_match;

	// Variable sized array to contain the handler addresses
	sljit_uw entry_addresses[1];
};

struct regex_match
{
	// current and next state array
	void* current;
	void* next;
	// starting 
	sljit_w head;
	// string character index (ever increasing)
	sljit_w index;

#ifndef SLJIT_INDIRECT_CALL
	union {
		void *continue_match;
		SLJIT_CALL void (*call_continue)(struct regex_match* match, regex_char_t* input_string, int length);
	};
#else
	union {
		void **continue_match_ptr;
		SLJIT_CALL void (*call_continue)(void* current, void* next);
	};
#endif

	// Variable sized array to contain the state arrays
	sljit_w states[1];
};

// State vector
//  ITEM[0] - pointer to the label in the machine code
//  ITEM[1] - next pointer
//  ITEM[2] - string started from (optional)
//  ITEM[3] - max ID (optional)

// Register allocation
// SLJIT_GENERAL_REG1    - current state array (loaded & stored: regex_match->current)
// SLJIT_GENERAL_REG2    - next state array (loaded & stored: regex_match->next)
// SLJIT_GENERAL_REG3    - head (loaded & stored: regex_match->head)
// SLJIT_TEMPORARY_REG2  - current character
// SLJIT_TEMPORARY_REG1  - tmp1
// SLJIT_TEMPORARY_REG3  - tmp2

// Stack layout
//  SLJIT_LOCALS_REG[0] - struct regex_match*
//  SLJIT_LOCALS_REG[1] - string fragment pointer
//  SLJIT_LOCALS_REG[2] - string fragment length
//  SLJIT_LOCALS_REG[3] - current character index

// ---------------------------------------------------------------------
//  Stack management
// ---------------------------------------------------------------------

// Try to allocate 2^n blocks
#define STACK_FRAGMENT_SIZE (((8 * sizeof(struct stack_item)) - (sizeof(struct stack_fragment_data))) / (sizeof(struct stack_item)))

struct stack_item {
	int type;
	int value;
};

struct stack_fragment_data {
	struct stack_fragment *next;
	struct stack_fragment *prev;
};

struct stack_fragment {
	struct stack_fragment_data data;
	struct stack_item items[STACK_FRAGMENT_SIZE];
};

struct stack {
	struct stack_fragment *first;
	struct stack_fragment *last;
	int index;
	int count;
};

#ifdef SLJIT_DEBUG

static void stack_check(struct stack *stack)
{
	struct stack_fragment *curr;
	int found;

	if (!stack)
		return;

	SLJIT_ASSERT(stack->index >= 0 && stack->index < STACK_FRAGMENT_SIZE);

	if (stack->first == NULL) {
		SLJIT_ASSERT(stack->first == NULL && stack->last == NULL);
		SLJIT_ASSERT(stack->index == STACK_FRAGMENT_SIZE - 1 && stack->count == 0);
		return;
	}

	found = 0;
	if (stack->last == NULL) {
		SLJIT_ASSERT(stack->index == STACK_FRAGMENT_SIZE - 1 && stack->count == 0);
		found = 1;
	}
	else
		SLJIT_ASSERT(stack->index >= 0 && stack->count >= 0);

	SLJIT_ASSERT(stack->first->data.prev == NULL);
	curr = stack->first;
	while (curr) {
		if (curr == stack->last)
			found = 1;
		if (curr->data.next)
			SLJIT_ASSERT(curr->data.next->data.prev == curr);
		curr = curr->data.next;
	}
	SLJIT_ASSERT(found);
}

#endif

static void stack_init(struct stack *stack)
{
	stack->first = NULL;
	stack->last = NULL;
	stack->index = STACK_FRAGMENT_SIZE - 1;
	stack->count = 0;
}

static void stack_destroy(struct stack *stack)
{
	struct stack_fragment* curr = stack->first;
	struct stack_fragment* prev;

#ifdef SLJIT_DEBUG
	stack_check(stack);
#endif

	while (curr) {
		prev = curr;
		curr = curr->data.next;
		SLJIT_FREE(prev);
	}
}

static INLINE struct stack_item* stack_top(struct stack *stack)
{
	SLJIT_ASSERT(stack->last);
	return stack->last->items + stack->index;
}

static int stack_push(struct stack *stack, int type, int value)
{
	if (stack->last) {
		stack->index++;
		if (stack->index >= STACK_FRAGMENT_SIZE) {
			stack->index = 0;
			if (!stack->last->data.next) {
				stack->last->data.next = (struct stack_fragment*)SLJIT_MALLOC(sizeof(struct stack_fragment));
				if (!stack->last->data.next)
					return 1;
				stack->last->data.next->data.next = NULL;
				stack->last->data.next->data.prev = stack->last;
			}
			stack->last = stack->last->data.next;
		}
	}
	else if (!stack->first) {
		stack->last = (struct stack_fragment*)SLJIT_MALLOC(sizeof(struct stack_fragment));
		if (!stack->last)
			return 1;
		stack->last->data.prev = NULL;
		stack->last->data.next = NULL;
		stack->first = stack->last;
		stack->index = 0;
	}
	else {
		stack->last = stack->first;
		stack->index = 0;
	}
	stack->last->items[stack->index].type = type;
	stack->last->items[stack->index].value = value;
	stack->count++;
#ifdef SLJIT_DEBUG
	stack_check(stack);
#endif
	return 0;
}

static struct stack_item* stack_pop(struct stack *stack)
{
	struct stack_item* ret = stack_top(stack);

	if (stack->index > 0)
		stack->index--;
	else {
		stack->last = stack->last->data.prev;
		stack->index = STACK_FRAGMENT_SIZE - 1;
	}

	stack->count--;
#ifdef SLJIT_DEBUG
	stack_check(stack);
#endif
	return ret;
}

static INLINE void stack_clone(struct stack *src, struct stack *dst)
{
	*dst = *src;
}

static int stack_push_copy(struct stack *stack, int items, int length)
{
	struct stack_fragment *frag1;
	int ind1;
	struct stack_fragment *frag2;
	int ind2;
	int counter;

	SLJIT_ASSERT(stack->count >= length && items <= length && items > 0);

	// Allocate the necessary elements
	counter = items;
	frag1 = stack->last;
	ind1 = stack->index;
	while (counter > 0) {
		if (stack->index + counter >= STACK_FRAGMENT_SIZE) {
			counter -= STACK_FRAGMENT_SIZE - stack->index - 1 + 1;
			stack->index = 0;
			if (!stack->last->data.next) {
				stack->last->data.next = (struct stack_fragment*)SLJIT_MALLOC(sizeof(struct stack_fragment));
				if (!stack->last->data.next)
					return 1;
				stack->last->data.next->data.next = NULL;
				stack->last->data.next->data.prev = stack->last;
			}
			stack->last = stack->last->data.next;
		}
		else {
			stack->index += counter;
			counter = 0;
		}
	}

	frag2 = stack->last;
	ind2 = stack->index;
	while (length > 0) {
		frag2->items[ind2--] = frag1->items[ind1--];
		if (ind1 < 0) {
			ind1 = STACK_FRAGMENT_SIZE - 1;
			frag1 = frag1->data.prev;
		}
		if (ind2 < 0) {
			ind2 = STACK_FRAGMENT_SIZE - 1;
			frag2 = frag2->data.prev;
		}
		length--;
	}

#ifdef SLJIT_DEBUG
	stack_check(stack);
#endif
	stack->count += items;
	return 0;
}

// ---------------------------------------------------------------------
//  Parser
// ---------------------------------------------------------------------

enum {
	// Common
	type_begin,
	type_end,
	type_any,
	type_char,
	type_id,

	// generator only
	type_branch,
	type_jump,

	// Parser only
	type_open_br,
	type_close_br,
	type_select,
	type_asterisk,
	type_plus_sign,
	type_qestion_mark,
};

static regex_char_t* decode_number(regex_char_t *regex_string, int length, int* result)
{
	int value = 0;

	SLJIT_ASSERT(length > 0);
	if (*regex_string < '0' || *regex_string > '9') {
		*result = -1;
		return regex_string;
	}

	while (length > 0 && *regex_string >= '0' && *regex_string <= '9') {
		value = value * 10 + (*regex_string - '0');
		length--;
		regex_string++;
	}

	*result = value;
	return regex_string;
}

static int iterate(struct stack *stack, int min, int max)
{
	struct stack it;
	struct stack_item *item;
	int count = -1;
	int len = 0;
	int depth = 0;

	stack_clone(stack, &it);

	// calculate size
	while (count < 0) {
		item = stack_pop(&it);
		switch (item->type) {
		case type_id:
		case type_plus_sign:
		case type_qestion_mark:
			len++;
			break;

		case type_asterisk:
			len += 2;
			break;

		case type_close_br:
			depth++;
			break;

		case type_open_br:
			SLJIT_ASSERT(depth > 0);
			depth--;
			if (depth == 0)
				count = it.count;
			break;

		case type_select:
			SLJIT_ASSERT(depth > 0);
			len += 2;
			break;

		default:
			SLJIT_ASSERT(item->type != type_begin && item->type != type_end);
			if (depth == 0)
				count = it.count;
			len++;
			break;
		}
	}

	if (min == 0 && max == 0) {
		// {0,0} case, not {0,} case
		// delete subtree
		stack_clone(&it, stack);
		// and put an empty bracket expression instead of it
		if (stack_push(stack, type_open_br, 0))
			return REGEX_MEMORY_ERROR;
		if (stack_push(stack, type_close_br, 0))
			return REGEX_MEMORY_ERROR;
		return len;
	}

	count = stack->count - count;

	// put an open bracket before the sequence
	if (stack_push_copy(stack, 1, count))
		return -1;

	SLJIT_ASSERT(stack_push(&it, type_open_br, 0) == 0);

	// copy the data
	if (max > 0) {
		len = len * (max - 1);
		max -= min;
		// Insert ? operators
		len += max;

		if (min > 0) {
			min--;
			while (min > 0) {
				if (stack_push_copy(stack, count, count))
					return -1;
				min--;
			}
			if (max > 0) {
				if (stack_push_copy(stack, count, count))
					return -1;
				if (stack_push(stack, type_qestion_mark, 0))
					return REGEX_MEMORY_ERROR;
				count++;
				max--;
			}
		}
		else {
			SLJIT_ASSERT(max > 0);
			max--;
			count++;
			if (stack_push(stack, type_qestion_mark, 0))
				return REGEX_MEMORY_ERROR;
		}

		while (max > 0) {
			if (stack_push_copy(stack, count, count))
				return -1;
			max--;
		}
	}
	else {
		SLJIT_ASSERT(min > 0);
		min--;
		// Insert + operator
		len = len * min + 1;
		while (min > 0) {
			if (stack_push_copy(stack, count, count))
				return -1;
			min--;
		}

		if (stack_push(stack, type_plus_sign, 0))
			return REGEX_MEMORY_ERROR;
	}

	// Close the opened bracket
	if (stack_push(stack, type_close_br, 0))
		return REGEX_MEMORY_ERROR;

	return len;
}

static int parse_iterator(regex_char_t *regex_string, int length, struct stack *stack, int* encoded_len, int begin)
{
	// We only know that *regex_string == {
	int val1, val2;
	regex_char_t *base_from = regex_string;
	regex_char_t *from;

	length--;
	regex_string++;

	// Decode left value
	val2 = -1;
	if (length == 0)
		return -2;
	if (*regex_string == ',') {
		val1 = 0;
		length--;
		regex_string++;
	}
	else {
		from = regex_string;
		regex_string = decode_number(regex_string, length, &val1);
		if (val1 < 0)
			return -2;
		length -= regex_string - from;

		if (length == 0)
			return -2;
		if (*regex_string == '}') {
			val2 = val1;
			if (val1 == 0)
				val1 = -1;
		}
		else if (length >= 2 && *regex_string == '!' && regex_string[1] == '}') {
			// Non posix extension
			if (stack_push(stack, type_id, val1))
				return -1;
			(*encoded_len)++;
			return (regex_string - base_from) + 1;
		}
		else {
			if (*regex_string != ',')
				return -2;
			length--;
			regex_string++;
		}
	}

	if (begin)
		return -2;

	// Decode right value
	if (val2 == -1) {
		if (length == 0)
			return -2;
		if (*regex_string == '}')
			val2 = 0;
		else {
			from = regex_string;
			regex_string = decode_number(regex_string, length, &val2);
			length -= regex_string - from;
			if (val2 < 0 || length == 0 || *regex_string != '}' || val2 < val1)
				return -2;
			if (val2 == 0) {
				SLJIT_ASSERT(val1 == 0);
				val1 = -1;
			}
		}
	}

	// Fast cases
	if (val1 > 1 || val2 > 1) {
		val1 = iterate(stack, val1, val2);
		if (val1 < 0)
			return -1;
		*encoded_len += val1;
	}
	else if (val1 == 0 && val2 == 0) {
		if (stack_push(stack, type_asterisk, 0))
			return -1;
		*encoded_len += 2;
	}
	else if (val1 == 1 && val2 == 0) {
		if (stack_push(stack, type_plus_sign, 0))
			return -1;
		(*encoded_len)++;
	}
	else if (val1 == 0 && val2 == 1) {
		if (stack_push(stack, type_qestion_mark, 0))
			return -1;
		(*encoded_len)++;
	}
	else if (val1 == -1) {
		val1 = iterate(stack, 0, 0);
		if (val1 < 0)
			return -1;
		*encoded_len -= val1;
		SLJIT_ASSERT(*encoded_len >= 2);
	}
	else {
		// Ignore
		SLJIT_ASSERT(val1 == 1 && val2 == 1);
	}
	return regex_string - base_from;
}

static int parse(regex_char_t *regex_string, int length, struct stack *stack, int *flags)
{
	struct stack brackets;
	int encoded_len = 2;
	int depth = 0;
	int begin = 1;
	int ret_val;

	stack_init(&brackets);

	if (stack_push(stack, type_begin, 0))
		return REGEX_MEMORY_ERROR;

	if (length > 0 && *regex_string == '^') {
		*flags |= MATCH_BEGIN;
		length--;
		regex_string++;
	}

	while (length > 0) {
		switch (*regex_string) {
		case '\\' :
			length--;
			regex_string++;
			if (length == 0)
				return REGEX_INVALID_REGEX;
			if (stack_push(stack, type_char, *regex_string))
				return REGEX_MEMORY_ERROR;
			begin = 0;
			encoded_len++;
			break;

		case '.' :
			if (stack_push(stack, type_any, *regex_string))
				return REGEX_MEMORY_ERROR;
			begin = 0;
			encoded_len++;
			break;

		case '(' :
			depth++;
			if (stack_push(stack, type_open_br, 0))
				return REGEX_MEMORY_ERROR;
			begin = 1;
			break;

		case ')' :
			if (depth == 0)
				return REGEX_INVALID_REGEX;
			depth--;
			if (stack_push(stack, type_close_br, 0))
				return REGEX_MEMORY_ERROR;
			begin = 0;
			break;

		case '|' :
			if (stack_push(stack, type_select, 0))
				return REGEX_MEMORY_ERROR;
			begin = 1;
			encoded_len += 2;
			break;

		case '*' :
			if (begin)
				return REGEX_INVALID_REGEX;
			if (stack_push(stack, type_asterisk, 0))
				return REGEX_MEMORY_ERROR;
			encoded_len += 2;
			break;

		case '?' :
		case '+' :
			if (begin)
				return REGEX_INVALID_REGEX;
			if (stack_push(stack, (*regex_string == '+') ? type_plus_sign : type_qestion_mark, 0))
				return REGEX_MEMORY_ERROR;
			encoded_len++;
			break;

		case '{' :
			ret_val = parse_iterator(regex_string, length, stack, &encoded_len, begin);

			if (ret_val >= 0) {
				length -= ret_val;
				regex_string += ret_val;
			}
			else if (ret_val == -1)
				return REGEX_MEMORY_ERROR;
			else {
				SLJIT_ASSERT(ret_val == -2);
				if (stack_push(stack, type_char, '{'))
					return REGEX_MEMORY_ERROR;
				encoded_len++;
			}
			break;

		default:
			if (length == 1 && *regex_string == '$') {
				*flags |= MATCH_END;
				break;
			}
			if (stack_push(stack, type_char, *regex_string))
				return REGEX_MEMORY_ERROR;
			begin = 0;
			encoded_len++;
			break;
		}
		length--;
		regex_string++;
	}

	if (depth != 0)
		return REGEX_INVALID_REGEX;

	if (stack_push(stack, type_end, 0))
		return REGEX_MEMORY_ERROR;

	return -encoded_len;
}

// ---------------------------------------------------------------------
//  Generating machine state transitions
// ---------------------------------------------------------------------

#define PUT_TRANSITION(typ, val) \
	do { \
		--transitions_ptr; \
		transitions_ptr->type = typ; \
		transitions_ptr->value = val; \
	} while (0)

static struct stack_item* handle_iteratives(struct stack_item *transitions_ptr, struct stack_item *transitions, struct stack *depth)
{
	struct stack_item *item;

	while (1) {
		item = stack_top(depth);

		switch (item->type) {
		case type_asterisk:
			SLJIT_ASSERT(transitions[item->value].type == type_branch);
			transitions[item->value].value = transitions_ptr - transitions;
			PUT_TRANSITION(type_branch, item->value + 1);
			break;

		case type_plus_sign:
			SLJIT_ASSERT(transitions[item->value].type == type_branch);
			transitions[item->value].value = transitions_ptr - transitions;
			break;

		case type_qestion_mark:
			PUT_TRANSITION(type_branch, item->value);
			break;

		default:
			return transitions_ptr;
		}
		stack_pop(depth);
	}
}

static struct stack_item* generate_transitions(struct stack *stack, struct stack *depth, int encoded_len)
{
	struct stack_item *transitions;
	struct stack_item *transitions_ptr;
	struct stack_item *item;

	transitions = SLJIT_MALLOC(sizeof(struct stack_item) * encoded_len);
	if (!transitions)
		return NULL;

	transitions_ptr = transitions + encoded_len;
	while (stack->count > 0) {
		item = stack_pop(stack);
		switch (item->type) {
		case type_begin:
		case type_open_br:
			item = stack_pop(depth);
			if (item->type == type_select)
				PUT_TRANSITION(type_branch, item->value + 1);
			else
				SLJIT_ASSERT(item->type == type_close_br);
			if (stack->count == 0)
				PUT_TRANSITION(type_begin, 0);
			else
				transitions_ptr = handle_iteratives(transitions_ptr, transitions, depth);
			break;

		case type_end:
		case type_close_br:
			if (item->type == type_end)
				*--transitions_ptr = *item;
			if (stack_push(depth, type_close_br, transitions_ptr - transitions))
				return NULL;
			break;

		case type_select:
			item = stack_top(depth);
			if (item->type == type_select) {
				SLJIT_ASSERT(transitions[item->value].type == type_jump);
				PUT_TRANSITION(type_branch, item->value + 1);
				PUT_TRANSITION(type_jump, item->value);
				item->value = transitions_ptr - transitions;
			}
			else {
				SLJIT_ASSERT(item->type == type_close_br);
				item->type = type_select;
				PUT_TRANSITION(type_jump, item->value);
				item->value = transitions_ptr - transitions;
			}
			break;

		case type_asterisk:
		case type_plus_sign:
		case type_qestion_mark:
			if (item->type != type_qestion_mark)
				PUT_TRANSITION(type_branch, 0);
			if (stack_push(depth, item->type, transitions_ptr - transitions))
				return NULL;
			break;

		case type_any:
		case type_char:
			// Requires handle_iteratives
			*--transitions_ptr = *item;
			transitions_ptr = handle_iteratives(transitions_ptr, transitions, depth);
			break;

		default:
			*--transitions_ptr = *item;
			break;
		}
	}

	SLJIT_ASSERT(transitions == transitions_ptr);
	SLJIT_ASSERT(depth->count == 0);
	return transitions;
}

#undef PUT_TRANSITION

#ifdef REGEX_VERBOSE

static void verbose_transitions(struct stack_item *transitions_ptr, struct stack_item *states_ptr, int encoded_len, int flags)
{
	printf("-----------------\nTransitions\n-----------------\n");
	int pos = 0;
	while (encoded_len-- > 0) {
		printf("[%3d] ", pos++);
		if (states_ptr->type >= 0)
			printf("(%3d) ", states_ptr->type);
		switch (transitions_ptr->type) {
		case type_begin:
			printf("type_begin\n");
			break;

		case type_end:
			printf("type_end\n");
			break;

		case type_any:
			printf("type_any '.'\n");
			break;

		case type_char:
			printf("type_char '%c'\n", transitions_ptr->value);
			break;

		case type_id:
			printf("type_id %d\n", transitions_ptr->value);
			break;

		case type_branch:
			printf("type_branch -> %d\n", transitions_ptr->value);
			break;

		case type_jump:
			printf("type_jump -> %d\n", transitions_ptr->value);
			break;

		default:
			printf("UNEXPECTED TYPE\n");
			break;
		}
		transitions_ptr++;
		states_ptr++;
	}
	printf("flags: ");
	if (!(flags & (MATCH_BEGIN | MATCH_END | ID_CHECK)))
		printf("none");
	if (flags & MATCH_BEGIN)
		printf("MATCH_BEGIN ");
	if (flags & MATCH_END)
		printf("MATCH_END ");
	if (flags & ID_CHECK)
		printf("ID_CHECK ");
	printf("\n");
}

#endif

// ---------------------------------------------------------------------
//  Utilities
// ---------------------------------------------------------------------

static struct stack_item* generate_states(struct stack_item* transitions_ptr, int encoded_len, int* states_len_ptr, int* flags)
{
	struct stack_item *states;
	struct stack_item *states_ptr;
	int states_len = 2;

	states = SLJIT_MALLOC(sizeof(struct stack_item) * encoded_len);
	if (!states)
		return NULL;

	states_ptr = states;
	while (encoded_len > 0) {
		switch (transitions_ptr->type) {
		case type_any:
		case type_char:
			states_ptr->type = states_len++;
			break;

		case type_begin:
			states_ptr->type = 0;
			break;

		case type_end:
			states_ptr->type = 1;
			break;

		case type_id:
			if (transitions_ptr->value > 0)
				*flags |= ID_CHECK;
			states_ptr->type = -1;
			break;

		default:
			states_ptr->type = -1;
			break;
		}
		states_ptr->value = -1;
		states_ptr++;
		transitions_ptr++;
		encoded_len--;
	}
	*states_len_ptr = states_len;
	return states;
}

static int trace_transitions(int from, struct stack_item* transitions, struct stack_item* states, struct stack *stack, struct stack *depth)
{
	int id = 0;

	SLJIT_ASSERT(states[from].type >= 0);

	from++;

	// Be prepared for any paths (loops, etc)
	while (1) {
		if (transitions[from].type == type_id)
			if (id < transitions[from].value)
				id = transitions[from].value;

		if (states[from].value < id) {
			// Forward step
			if (states[from].value == -1)
				if (stack_push(stack, 0, from))
					return REGEX_MEMORY_ERROR;
			states[from].value = id;

			if (transitions[from].type == type_branch) {
				if (stack_push(depth, id, from))
					return REGEX_MEMORY_ERROR;
				from++;
				continue;
			}
			else if (transitions[from].type == type_jump) {
				from = transitions[from].value;
				continue;
			}
			else if (states[from].type < 0) {
				from++;
				continue;
			}
		}

		// Back tracking
		if (depth->count > 0) {
			id = stack_top(depth)->type;
			from = transitions[stack_pop(depth)->value].value;
			continue;
		}
		return 0;
	}
}

// ---------------------------------------------------------------------
//  Code generator
// ---------------------------------------------------------------------


// ---------------------------------------------------------------------
//  Main compiler
// ---------------------------------------------------------------------

struct regex_machine* regex_compile(regex_char_t *regex_string, int length, int *error)
{
	struct stack stack;
	struct stack depth;
	int encoded_len, states_len;
	int flags = 0, index, value, done;
	struct stack_item *transitions;
	struct stack_item *states;

	struct sljit_compiler* compiler;
	struct regex_machine* machine;

	if (error)
		*error = REGEX_NO_ERROR;

	// Step 1: parsing (Left->Right)
	// syntax check and AST generator
	stack_init(&stack);
	encoded_len = parse(regex_string, length, &stack, &flags);
	if (encoded_len >= 0) {
		stack_destroy(&stack);
		if (error)
			*error = encoded_len;
		return NULL;
	}
	encoded_len = -encoded_len;

	// Step 2: Right->Left generating branches
	stack_init(&depth);
	transitions = generate_transitions(&stack, &depth, encoded_len);
	stack_destroy(&stack);
	stack_destroy(&depth);
	if (!transitions) {
		if (error)
			*error = REGEX_MEMORY_ERROR;
		return NULL;
	}

	// Step 3: Left->Right
	// Calculate the number of states
	states = generate_states(transitions, encoded_len, &states_len, &flags);
	if (!states) {
		SLJIT_FREE(transitions);
		if (error)
			*error = REGEX_MEMORY_ERROR;
		return NULL;
	}

#ifdef REGEX_VERBOSE
	verbose_transitions(transitions, states, encoded_len, flags);
#endif

	// Step 4: Left->Right generate code
	stack_init(&stack);
	stack_init(&depth);
	done = 0;
	machine = NULL;
	compiler = NULL;

	do {
		// A do {} while(0) expression is udes to avoid goto.

		machine = (struct regex_machine*)SLJIT_MALLOC(sizeof(struct regex_machine) + (states_len - 1) * sizeof(sljit_uw));
		if (!machine)
			break;

		compiler = sljit_create_compiler();
		if (!compiler)
			break;

		// Step 4.1: Generate entry
		// struct regex_match* pointer, 
		if (sljit_emit_enter(compiler, 0, 3, 2 * sizeof(sljit_uw)))
			break;

#ifdef REGEX_VERBOSE
		printf("-----------------\nTrace\n-----------------\n");
#endif

		index = 0;
		while (index < encoded_len - 1) {
			if (states[index].type >= 0) {
				if (trace_transitions(index, transitions, states, &stack, &depth))
					break;
#ifdef REGEX_VERBOSE
				printf("(%3d): ", states[index].type);
#endif

				while (stack.count > 0) {
					value = stack_pop(&stack)->value;
					if (states[value].type >= 0) {
#ifdef REGEX_VERBOSE
						printf("-> (%3d:%3d) ", states[value].type, states[value].value);
#endif
					}
					states[value].value = -1;
				}

#ifdef REGEX_VERBOSE
				printf("\n");
#endif
			}
			index++;
		}

		if (index == encoded_len - 1) {
			if ((flags & ID_CHECK) && !(flags & MATCH_BEGIN))
				machine->no_states = 4;
			else if (!(flags & ID_CHECK) && (flags & MATCH_BEGIN))
				machine->no_states = 2;
			else
				machine->no_states = 3;

			machine->states_len = states_len;

			machine->continue_match = sljit_generate_code(compiler);
#ifdef REGEX_VERBOSE
			printf("Continue match: %p\n", machine->continue_match);
#endif
			if (machine->continue_match)
				done = 1;
		}
	} while (0);

	stack_destroy(&stack);
	stack_destroy(&depth);
	SLJIT_FREE(transitions);
	SLJIT_FREE(states);
	if (compiler)
		sljit_free_compiler(compiler);
	if (done)
		return machine;

	if (machine) {
		SLJIT_FREE(machine);
	}
	if (error)
		*error = REGEX_MEMORY_ERROR;
	return NULL;
}

void regex_free_machine(struct regex_machine* machine)
{
	sljit_free_code(machine->continue_match);
	SLJIT_FREE(machine);
}

// ---------------------------------------------------------------------
//  Mathching utilities
// ---------------------------------------------------------------------

struct regex_match* regex_begin_match(struct regex_machine* machine)
{
	sljit_w length = machine->no_states * machine->states_len;
	sljit_w *ptr1;
	sljit_w *ptr2;
	sljit_w *end;
	sljit_w *entry_addresses;

	struct regex_match* match = (struct regex_match*)SLJIT_MALLOC(sizeof(struct regex_match) + (length * 2 - 1) * sizeof(sljit_w));
	if (!match)
		return NULL;

	ptr1 = match->states;
	ptr2 = match->states + length;
	end = ptr2;
	entry_addresses = (sljit_w*)machine->entry_addresses;

	match->current = ptr1;
	match->next = ptr2;

	switch (machine->no_states) {
	case 2:
		while (ptr1 < end) {
			*ptr1++ = *entry_addresses;
			*ptr2++ = *entry_addresses++;
			*ptr1++ = -1;
			*ptr2++ = -1;
		}
		break;

	case 3:
		while (ptr1 < end) {
			*ptr1++ = *entry_addresses;
			*ptr2++ = *entry_addresses++;
			*ptr1++ = -1;
			*ptr2++ = -1;
			*ptr1++ = 0;
			*ptr2++ = 0;
		}
		break;

	case 4:
		while (ptr1 < end) {
			*ptr1++ = *entry_addresses;
			*ptr2++ = *entry_addresses++;
			*ptr1++ = -1;
			*ptr2++ = -1;
			*ptr1++ = 0;
			*ptr2++ = 0;
			*ptr1++ = 0;
			*ptr2++ = 0;
		}
		break;

	default:
		SLJIT_ASSERT_IMPOSSIBLE();
		break;
	}

	SLJIT_ASSERT(ptr1 == end);

#ifdef SLJIT_INDIRECT_CALL
	match->continue_match_ptr = &machine->continue_match;
#else
	match->continue_match = machine->continue_match;
#endif

printf("Init match\n");
	return match;
}

void regex_free_match(struct regex_match* match)
{
	SLJIT_FREE(match);
}

void regex_continue_match(struct regex_match* match, regex_char_t* input_string, int length)
{

}

#ifdef REGEX_VERBOSE
void regex_continue_match_verbose(struct regex_match* match, regex_char_t* input_string, int length)
{


}
#endif
