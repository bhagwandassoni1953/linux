// SPDX-License-Identifier: GPL-2.0
/*
 * jump label x86 support
 *
 * Copyright (C) 2009 Jason Baron <jbaron@redhat.com>
 *
 */
#include <linux/jump_label.h>
#include <linux/memory.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/jhash.h>
#include <linux/cpu.h>
#include <asm/kprobes.h>
#include <asm/alternative.h>
#include <asm/text-patching.h>

union jump_code_union {
	char code[JUMP_LABEL_NOP_SIZE];
	struct {
		char jump;
		int offset;
	} __attribute__((packed));
};

static void bug_at(unsigned char *ip, int line)
{
	/*
	 * The location is not an op that we were expecting.
	 * Something went wrong. Crash the box, as something could be
	 * corrupting the kernel.
	 */
	pr_crit("jump_label: Fatal kernel bug, unexpected op at %pS [%p] (%5ph) %d\n", ip, ip, ip, line);
	BUG();
}

static void __ref __jump_label_transform(struct jump_entry *entry,
					 enum jump_label_type type,
					 int init)
{
	union jump_code_union jmp;
	const unsigned char default_nop[] = { STATIC_KEY_INIT_NOP };
	const unsigned char *ideal_nop = ideal_nops[NOP_ATOMIC5];
	const void *expect, *code;
	int line;

	jmp.jump = 0xe9;
	jmp.offset = jump_entry_target(entry) -
		     (jump_entry_code(entry) + JUMP_LABEL_NOP_SIZE);

	if (type == JUMP_LABEL_JMP) {
		if (init) {
			expect = default_nop; line = __LINE__;
		} else {
			expect = ideal_nop; line = __LINE__;
		}

		code = &jmp.code;
	} else {
		if (init) {
			expect = default_nop; line = __LINE__;
		} else {
			expect = &jmp.code; line = __LINE__;
		}

		code = ideal_nop;
	}

	if (memcmp((void *)jump_entry_code(entry), expect, JUMP_LABEL_NOP_SIZE))
		bug_at((void *)jump_entry_code(entry), line);

	/*
	 * As long as only a single processor is running and the code is still
	 * not marked as RO, text_poke_early() can be used; Checking that
	 * system_state is SYSTEM_BOOTING guarantees it. It will be set to
	 * SYSTEM_SCHEDULING before other cores are awaken and before the
	 * code is write-protected.
	 *
	 * At the time the change is being done, just ignore whether we
	 * are doing nop -> jump or jump -> nop transition, and assume
	 * always nop being the 'currently valid' instruction
	 */
	if (init || system_state == SYSTEM_BOOTING) {
		text_poke_early((void *)jump_entry_code(entry), code,
				JUMP_LABEL_NOP_SIZE);
		return;
	}

	text_poke_bp((void *)jump_entry_code(entry), code, JUMP_LABEL_NOP_SIZE,
		     (void *)jump_entry_code(entry) + JUMP_LABEL_NOP_SIZE);
}

void arch_jump_label_transform(struct jump_entry *entry,
			       enum jump_label_type type)
{
	mutex_lock(&text_mutex);
	__jump_label_transform(entry, type, 0);
	mutex_unlock(&text_mutex);
}

static enum {
	JL_STATE_START,
	JL_STATE_NO_UPDATE,
	JL_STATE_UPDATE,
} jlstate __initdata_or_module = JL_STATE_START;

__init_or_module void arch_jump_label_transform_static(struct jump_entry *entry,
				      enum jump_label_type type)
{
	/*
	 * This function is called at boot up and when modules are
	 * first loaded. Check if the default nop, the one that is
	 * inserted at compile time, is the ideal nop. If it is, then
	 * we do not need to update the nop, and we can leave it as is.
	 * If it is not, then we need to update the nop to the ideal nop.
	 */
	if (jlstate == JL_STATE_START) {
		const unsigned char default_nop[] = { STATIC_KEY_INIT_NOP };
		const unsigned char *ideal_nop = ideal_nops[NOP_ATOMIC5];

		if (memcmp(ideal_nop, default_nop, 5) != 0)
			jlstate = JL_STATE_UPDATE;
		else
			jlstate = JL_STATE_NO_UPDATE;
	}
	if (jlstate == JL_STATE_UPDATE)
		__jump_label_transform(entry, type, 1);
}
